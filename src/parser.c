#include "parser.h"

#include "diag.h"

#include "limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many variables one closure can capture. Each costs a word in the
 * environment and a word in the cell, so the bound is generous but real. */
#define MAX_CAPTURES 64

/* A local variable binding in a lexical scope. */
typedef struct Var {
    char *name;
    Type *type;
    int offset;    /* byte offset from rbp within the function frame */
    int agg_param; /* parameter of struct/union type: slot holds a pointer */
    int captured;  /* referenced by a closure: the slot holds a box pointer */
    int owner;     /* func_counter value of the function that declared it */
    int used;      /* referenced anywhere; drives -Wunused-local */
    int is_param;  /* a parameter rather than a local statement */
    int is_synth;  /* compiler-generated, or in a body we never see: not warnable */
    Span decl_span; /* where the name was written, for diagnostics */
    struct Var *next;
} Var;

typedef struct Scope {
    Var *vars;
    struct Scope *parent;
    /* The outermost scope of a function body. Name lookup stops here: a variable
     * declared outside a function lives in a different frame, so naming it from
     * inside would read a slot holding something else entirely. */
    int is_fn_body;
} Scope;

/* A function declared inside another function, mapping the name the program
 * writes to the symbol emitted for it. */
typedef struct LocalFn {
    char *name;
    char *sym;
    struct LocalFn *next;
} LocalFn;

/* A function signature discovered by the pre-scan, used to resolve and check
 * calls (including forward references). */
typedef struct {
    char *name;
    Type *ret;
    Type **ptypes;
    int nparams;
    int is_ext;     /* extension method: first param is the receiver */
    /* Declared `extern`: implemented in C, so the assembly symbol is the name
     * as written and is not put in the Z namespace. */
    int is_extern;
    /* Declared `export`: defined in Z, but the symbol keeps the written name
     * and is emitted .globl so C can call it. */
    int is_export;
    Type *ext_recv; /* receiver type of an extension method */
} Sig;

/* A `const` binding: a named compile-time constant. Consts never occupy a
 * frame slot; a reference is replaced by the literal at parse time. */
typedef struct {
    char *name;
    Type *type;
    long long ival; /* TK_INT / TK_BOOL */
    int str_id;     /* TK_STRING */
} ConstDef;

/* An active generic type-parameter binding: name -> resolved Type (either a
 * TK_TYPEPARAM placeholder while parsing the template, or a concrete type
 * during monomorphization). */
typedef struct {
    char *name;
    Type *ty;
} TypeBind;

/* A registered generic function template, re-parsed per concrete
 * instantiation. tok_start/tok_end delimit the whole declaration (return type
 * through body) so it can be re-parsed from the token stream. */
typedef struct {
    char *name;
    int ntparams;
    char **tparams;
    int tok_start, tok_end;
    int is_ext;
    Type **ptypes; /* parameter type expressions of the template (may contain params) */
    int nparams;
    int vis_start; /* index of first user-visible param (1 if hidden sret $ret) */
    int nvisible;  /* number of user-visible params */
    Type *ret_type;
    char **mangled; /* mangled instance names, parallel to inst_types */
    Type ***inst_types;
    int ninstances;
} Generic;

typedef struct {
    Arena *arena;
    Token *toks;
    int ntoks;
    int pos;
    TypeCtx ty;

    Scope *scope;
    int next_offset; /* byte-offset allocator for the current function frame */

    Sig *sigs;
    int nsigs;
    int sig_cap;

    Type *cur_ret; /* return type of the function being parsed */
    int foreach_counter;
    int loop_depth; /* break/continue must sit inside a loop */
    /* Set while parsing the operand of `&`, so a `.name` on a class receiver
     * builds a bound method pointer instead of reporting a missing field. */
    int take_method_addr;
    StringTable *strings;
    /* While parsing a struct method body: the enclosing struct and the `this`
     * receiver variable (an E_VAR for the hidden first parameter). */
    StructDef *cur_msd;
    Expr *cur_this;

    /* ---- generics (monomorphization) ---- */
    TypeBind *tbinds; /* active type-parameter bindings (name -> concrete/param) */
    int ntbind, tbind_cap;
    Generic *generics; /* registered generic function templates */
    int ngenerics, generic_cap;
    ConstDef *consts;
    int nconsts, const_cap;

    Stmt **pending; /* concrete instances awaiting append to program items */
    int npending, pending_cap;
    int in_instantiate; /* set while re-parsing a template for an instance */

    /* ---- closures ----
     *
     * See the closure section below. `captured` is every variable in the current
     * function that a lambda captured: their frame slots hold a box rather than
     * the value. `lam_caps` is per-lambda -- the variables *this* lambda
     * captured, in the order the body first mentioned them, which is the order
     * its environment is laid out in. */
    int lambda_counter;
    int inline_label; /* labels for spliced inlined bodies, unique across the program */
    int func_counter; /* owner id stamped on each Var; see capture_for */
    struct Var *captured[MAX_CAPTURES];
    int ncaptured;
    struct Var *lam_caps[MAX_CAPTURES];
    int nlamcaps;
    int lam_owner;  /* func id of the lambda being parsed */
    int lam_active; /* nonzero while inside a lambda body */
    int cur_owner;  /* func id of the function whose frame is being built */

    /* The `Result` type the expression being parsed is expected to have, when
     * that is known: the declared type of a variable's initializer, the return
     * type of a `return`, or a parameter's type. `Ok` and `Err` need it because
     * neither can be written without the type it is completing. NULL whenever
     * there is no expectation, which is the normal case for everything else. */
    Type *expect_result;

    /* 1 when -O2 or above is in effect. Set once by parse_program from the
     * command line; the tree rewrites that improve code rather than check it --
     * closed-form loops, and whatever joins them -- are gated on it, so that -O1
     * output stays exactly what it was. */
    int opt_level;

    /* Functions declared inside another function. Each is emitted under a symbol
     * derived from the enclosing function's id, because two functions in
     * different bodies may both declare `helper` and one symbol cannot serve
     * both. This maps the name the program writes to the symbol emitted. */
    struct LocalFn *local_fns;
} Parser;

static int is_kind(Type *t, TypeKind k);

/* True when any `return` in this body yields a closure. Only the last statement
 * of a block can be a value-returning one in practice, but a return nested in an
 * `if` still counts, so the walk covers every branch. */
static int expr_is_closure(Expr *e) {
    if (e == NULL)
        return 0;
    if (e->kind == E_CLOSURE)
        return 1;
    if (expr_is_closure(e->lhs) || expr_is_closure(e->rhs) || expr_is_closure(e->env))
        return 1;
    for (int i = 0; i < e->nargs; i++)
        if (expr_is_closure(e->args[i]))
            return 1;
    for (int i = 0; i < e->narms; i++)
        if (expr_is_closure(e->arms[i].body))
            return 1;
    if (e->kind == E_VAR && is_kind(e->type, TK_CLOSURE))
        return 1;
    return 0;
}

static int stmt_returns_closure(Stmt *s) {
    if (s == NULL)
        return 0;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            if (stmt_returns_closure(s->items[i]))
                return 1;
        return 0;
    case S_FOR:
        return stmt_returns_closure(s->for_init) || stmt_returns_closure(s->body);
    case S_IF:
    case S_WHILE:
        return stmt_returns_closure(s->body) || stmt_returns_closure(s->orelse);
    case S_RETURN:
        return expr_is_closure(s->expr);
    default:
        return 0;
    }
}

static int body_returns_closure(Stmt *body) { return stmt_returns_closure(body); }

/* After a function's body is parsed, every slot its lambdas captured is known.
 * This pass marks the declaration and every reference of each such slot, so
 * codegen can tell a boxed local from a plain one by looking at the node.
 *
 * It walks only this function's own body. A hoisted lambda is a separate
 * program item, and the captures it was rewritten to reach are indirection
 * through the environment rather than frame slots, so there is nothing to mark
 * inside one. */
static void mark_boxed_expr(Parser *p, Expr *e) {
    if (e == NULL)
        return;
    if (e->kind == E_VAR) {
        for (int i = 0; i < p->ncaptured; i++) {
            if (p->captured[i]->offset == e->slot) {
                e->boxed = 1;
                break;
            }
        }
    }
    mark_boxed_expr(p, e->lhs);
    mark_boxed_expr(p, e->rhs);
    mark_boxed_expr(p, e->env);
    for (int i = 0; i < e->nargs; i++)
        mark_boxed_expr(p, e->args[i]);
    for (int i = 0; i < e->narms; i++)
        mark_boxed_expr(p, e->arms[i].body);
}

static void mark_boxed_stmt(Parser *p, Stmt *s) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            mark_boxed_stmt(p, s->items[i]);
        return;
    case S_FOR:
        mark_boxed_stmt(p, s->for_init);
        mark_boxed_stmt(p, s->body);
        return;
    case S_IF:
    case S_WHILE:
        mark_boxed_stmt(p, s->body);
        mark_boxed_stmt(p, s->orelse);
        return;
    case S_VAR:
        if (s->slot > 0) {
            for (int i = 0; i < p->ncaptured; i++) {
                if (p->captured[i]->offset == s->slot) {
                    s->boxed = 1;
                    break;
                }
            }
        }
        mark_boxed_expr(p, s->init);
        return;
    case S_EXPR:
    case S_RETURN:
        mark_boxed_expr(p, s->expr);
        return;
    default:
        return;
    }
}





/* Forward declarations. */
static Expr *parse_expr(Parser *p);
static Stmt *parse_stmt(Parser *p);
static void close_form_stmts(Parser *p, Stmt **items, int n);
static void inline_program(Parser *p, Stmt *program);
static Stmt *close_form_loop(Parser *p, Stmt *loop, Stmt **siblings, int sib_index);
static Stmt *parse_block(Parser *p);
static Stmt *parse_func(Parser *p, int nested);
static Type *parse_type_at(Parser *p, int i);
static Expr *parse_match(Parser *p, Span start);
static Expr *parse_lambda(Parser *p, Span span);
static void capture_for(Parser *p, struct Var *v, Span span);
static int lambda_ahead(Parser *p, int i);
static int is_kind(Type *t, TypeKind k);
static int body_returns_closure(Stmt *body);
static Expr *parse_unary(Parser *p);
static Expr *make_cvt(Parser *p, Type *to, Expr *e, Span span);
static void mark_synth(Parser *p, const char *name);
static void skip_braced_block(Parser *p);
static ConstDef *const_find(Parser *p, const char *name);
static Expr *const_expr(Parser *p, ConstDef *c, Span span);
static int const_fold_static(Parser *p, Expr *e, long long *out);

/* ---- token helpers ---- */

static Token *peek(Parser *p, int off) {
    int i = p->pos + off;
    if (i >= p->ntoks)
        i = p->ntoks - 1;
    return &p->toks[i];
}

static Token *cur(Parser *p) { return peek(p, 0); }

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->ntoks - 1)
        p->pos++;
    return t;
}

static int at(Parser *p, TokenKind kind) { return cur(p)->kind == kind; }

static int match(Parser *p, TokenKind kind) {
    if (at(p, kind)) {
        advance(p);
        return 1;
    }
    return 0;
}

/* ---- AST constructors ---- */

static Expr *new_expr(Parser *p, ExprKind kind, Span span) {
    Expr *e = arena_alloc(p->arena, sizeof *e);
    e->kind = kind;
    e->span = span;
    e->type = NULL;
    e->slot = -1;
    e->str_id = -1;
    return e;
}

static Stmt *new_stmt(Parser *p, StmtKind kind, Span span) {
    Stmt *s = arena_alloc(p->arena, sizeof *s);
    s->kind = kind;
    s->span = span;
    s->type = NULL;
    s->slot = -1;
    return s;
}

static int is_kind(Type *t, TypeKind k) { return t != NULL && t->kind == k; }
static int is_unk(Type *t) { return t == NULL; }

static Type *base_type_from_token(Parser *p, TokenKind k) {
    switch (k) {
    case T_KW_INT:
        return type_int(&p->ty);
    case T_KW_BOOL:
        return type_bool(&p->ty);
    case T_KW_FLOAT:
        return type_f64(&p->ty);
    case T_KW_STRING:
        return type_string(&p->ty);
    case T_KW_VOID:
        return type_void(&p->ty);
    default:
        return NULL;
    }
}

static int is_type_token(TokenKind k) {
    /* `fn` starts a function type, so anywhere a type may begin it counts. */
    return k == T_KW_INT || k == T_KW_BOOL || k == T_KW_FLOAT || k == T_KW_STRING ||
           k == T_KW_VOID || k == T_KW_FN || k == T_KW_METHOD || k == T_KW_CLOSURE;
}

/* Parses a type possibly followed by `*` stars and `[]` array suffixes.
 * Used in declarations, parameters, and return positions. */
/* Resolves a type-name token: builtins, or a declared struct by name. */
static Type *base_type_or_name(Parser *p, Token *t) {
    Type *b = base_type_from_token(p, t->kind);
    if (b != NULL)
        return b;
    if (t->kind == T_IDENT) {
        /* An interface name resolves here too, so a parameter, a return type and
         * a local can all be written `I` with nothing else to go on. */
        Type *ifc = type_find_iface(&p->ty, t->text);
        if (ifc != NULL)
            return ifc;
        /* An in-scope generic type-parameter binding (T) shadows any struct of
         * the same name. */
        for (int i = p->ntbind - 1; i >= 0; i--)
            if (strcmp(p->tbinds[i].name, t->text) == 0)
                return p->tbinds[i].ty;
        Type *st = type_find_struct(&p->ty, t->text);
        if (st != NULL)
            return st;
        return type_find_union(&p->ty, t->text);
    }
    return NULL;
}

static Type *parse_type(Parser *p);
static Type *parse_type_at(Parser *p, int i);

/* Reads a function type: `fn` "(" type ("," type)* ")" "->" type. A null
 * parameter list means the function takes nothing. */
static Type *parse_fn_type(Parser *p) {
    /* `fn(...)` is a plain code address; `method(...)` binds a receiver; and
     * `closure(...)` is a lambda that captured something. Same syntax, different
     * value -- and the three are distinct types, because they are called
     * differently: only a method or a closure passes a hidden first argument. */
    int mptr = cur(p)->kind == T_KW_METHOD;
    int clos = cur(p)->kind == T_KW_CLOSURE;
    advance(p);
    if (!match(p, T_LPAREN)) {
        diag_error(cur(p)->span, "expected '(' after 'fn'");
        return NULL;
    }
    /* Arena-allocated: the Type keeps this pointer, so it must outlive this
     * function. A stack array would dangle the moment we return. */
    Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
    int np = 0;
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (np >= Z_MAX_ARGS) {
                diag_error(cur(p)->span, "a function type may take at most %d parameters",
                           Z_MAX_ARGS);
                break;
            }
            Type *pt = parse_type(p);
            if (pt == NULL) {
                diag_error(cur(p)->span, "expected a parameter type in function type");
                break;
            }
            ptypes[np++] = pt;
            if (!match(p, T_COMMA))
                break;
        }
    }
    if (!match(p, T_RPAREN)) {
        diag_error(cur(p)->span, "expected ')' to close the function type's parameters");
        return NULL;
    }
    if (!match(p, T_ARROW)) {
        diag_error(cur(p)->span, "expected '->' and a return type after the function type");
        return NULL;
    }
    Type *ret = parse_type(p);
    if (ret == NULL) {
        diag_error(cur(p)->span, "expected a return type in function type");
        return NULL;
    }
    if (clos)
        return type_closure(&p->ty, ptypes, np, ret);
    return mptr ? type_mptr(&p->ty, ptypes, np, ret) : type_fnptr(&p->ty, ptypes, np, ret);
}

/* `Result<T,E>` is a builtin generic type: two type arguments in angle brackets.
 * Recognized by name rather than by a keyword so that a program could still use
 * `Result` as an ordinary identifier outside type position. */
static int at_result_type(Parser *p, int i) {
    return i + 1 < p->ntoks && p->toks[i].kind == T_IDENT &&
           strcmp(p->toks[i].text, "Result") == 0 && p->toks[i + 1].kind == T_LT;
}

/* Parses `Result<T,E>` with the cursor on `Result`. Returns NULL and reports if
 * the arguments do not name one-word types. */
static Type *parse_result_type(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* Result */
    if (!match(p, T_LT)) {
        diag_error(start, "'Result' needs two type arguments: Result<T, E>");
        return NULL;
    }
    Type *ok = parse_type(p);
    if (ok == NULL || !match(p, T_COMMA)) {
        diag_error(start, "'Result' needs two type arguments: Result<T, E>");
        return NULL;
    }
    Type *err = parse_type(p);
    if (err == NULL) {
        diag_error(start, "'Result' needs two type arguments: Result<T, E>");
        return NULL;
    }
    if (!match(p, T_GT)) {
        diag_error(cur(p)->span, "expected '>' to close 'Result<...>'");
        return NULL;
    }
    Type *r = type_result(&p->ty, ok, err);
    if (r == NULL) {
        diag_error(start,
                   "'Result<%s, %s>' is not available: each side must be one word "
                   "(int, bool, float, string, or a pointer)",
                   type_name(&p->ty, ok), type_name(&p->ty, err));
        return NULL;
    }
    return r;
}

static Type *parse_type(Parser *p) {
    if (at(p, T_KW_FN) || at(p, T_KW_METHOD) || at(p, T_KW_CLOSURE))
        return parse_fn_type(p);
    if (at_result_type(p, p->pos))
        return parse_result_type(p);
    Type *t = base_type_or_name(p, cur(p));
    if (t == NULL)
        return NULL;
    advance(p);
    for (;;) {
        if (at(p, T_STAR)) {
            advance(p);
            t = type_ptr(&p->ty, t);
        } else if (at(p, T_LBRACKET) && peek(p, 1)->kind == T_RBRACKET) {
            advance(p);
            advance(p);
            t = type_array(&p->ty, t, -1);
        } else {
            break;
        }
    }
    return t;
}

/* Same as parse_type but reads from an explicit token index (used by the
 * pre-scan, which must not disturb the parser position). */
static Type *parse_type_at(Parser *p, int i) {
    if (i >= p->ntoks)
        return NULL;
    /* A function type in a pre-scanned parameter list: the parameter types are
     * separated by commas at depth 0 of the inner list, so a flat scan is
     * enough -- the only thing that matters is how many there are. */
    if (p->toks[i].kind == T_KW_FN || p->toks[i].kind == T_KW_METHOD ||
        p->toks[i].kind == T_KW_CLOSURE) {
        int mptr = p->toks[i].kind == T_KW_METHOD;
        int j = i + 1;
        if (j < p->ntoks && p->toks[j].kind == T_LPAREN) {
            j++;
            Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
            int np = 0;
            while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
                if (np < Z_MAX_ARGS) {
                    Type *pt = parse_type_at(p, j);
                    if (pt == NULL)
                        break;
                    ptypes[np++] = pt;
                }
                while (j < p->ntoks && p->toks[j].kind != T_COMMA &&
                       p->toks[j].kind != T_RPAREN)
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                    j++;
            }
            while (j < p->ntoks && p->toks[j].kind != T_RPAREN)
                j++;
            if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                j++; /* step past ')' so the '->' is the current token */
            Type *ret = type_void(&p->ty);
            if (j < p->ntoks && p->toks[j].kind == T_ARROW) {
                Type *r = parse_type_at(p, j + 1);
                if (r != NULL)
                    ret = r;
            }
            if (p->toks[i].kind == T_KW_CLOSURE)
                return type_closure(&p->ty, ptypes, np, ret);
            return mptr ? type_mptr(&p->ty, ptypes, np, ret)
                        : type_fnptr(&p->ty, ptypes, np, ret);
        }
        return NULL;
    }
    if (at_result_type(p, i)) {
        /* Reuse the real parser by pointing a shallow copy of the cursor at the
         * type arguments. parse_type walks p->toks and p->pos, so a copy of the
         * Parser with only the position moved parses the same thing without
         * disturbing the caller's cursor. */
        Parser sub = *p;
        sub.pos = i;
        Type *r = parse_result_type(&sub);
        if (r == NULL)
            return NULL;
        i = sub.pos;
        for (;;) {
            if (i < p->ntoks && p->toks[i].kind == T_STAR) {
                i++;
                r = type_ptr(&p->ty, r);
            } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                       p->toks[i + 1].kind == T_RBRACKET) {
                i += 2;
                r = type_array(&p->ty, r, -1);
            } else {
                break;
            }
        }
        return r;
    }
    Type *t = base_type_or_name(p, &p->toks[i]);
    if (t == NULL)
        return NULL;
    i++;
    for (;;) {
        if (i < p->ntoks && p->toks[i].kind == T_STAR) {
            i++;
            t = type_ptr(&p->ty, t);
        } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                   p->toks[i + 1].kind == T_RBRACKET) {
            i += 2;
            t = type_array(&p->ty, t, -1);
        } else {
            break;
        }
    }
    return t;
}

static void scope_push(Parser *p) {
    Scope *s = arena_alloc(p->arena, sizeof *s);
    s->vars = NULL;
    s->parent = p->scope;
    p->scope = s;
}

/* Leaves a scope, reporting any variable declared in it that was never
 * referenced. Runs whether or not the block turned out to be well-formed, so a
 * program that fails to compile still gets its warnings. */
static void scope_pop(Parser *p) {
    Scope *s = p->scope;
    if (s == NULL)
        return;
    if (warn_enabled(W_UNUSED_LOCAL)) {
        for (Var *v = s->vars; v != NULL; v = v->next) {
            if (!v->used && !v->is_synth)
                diag_warn(W_UNUSED_LOCAL, v->decl_span, "'%s' is declared but never used", v->name);
        }
    }
    p->scope = s->parent;
}

static Var *lookup_var_local(Parser *p, const char *name) {
    for (Var *v = p->scope->vars; v != NULL; v = v->next) {
        if (strcmp(v->name, name) == 0)
            return v;
    }
    return NULL;
}

/* Marks a just-declared binding as compiler-generated, so -Wunused-local does
 * not report the desugarer's own temporaries or a hidden result buffer. */
static void mark_synth(Parser *p, const char *name) {
    Var *v = lookup_var_local(p, name);
    if (v != NULL)
        v->is_synth = 1;
}

static Var *lookup_var(Parser *p, const char *name) {
    for (Scope *s = p->scope; s != NULL; s = s->parent) {
        for (Var *v = s->vars; v != NULL; v = v->next) {
            if (strcmp(v->name, name) == 0) {
                /* Resolving the name is a use. lookup_var_local deliberately
                 * does not do this, since it also serves the redeclaration
                 * check, where merely testing for a name is not a use. */
                v->used = 1;
                return v;
            }
        }
        /* A function body is a frame of its own, so its outermost scope is where
         * name lookup stops. Continuing past it would let a function read a
         * variable declared beside it: `var g = 5; int f() { return g; }` used
         * to compile, and returned 0, because slot numbers are assigned per
         * function and f read whatever f's own frame happened to hold. */
        if (s->is_fn_body)
            break;
    }
    return NULL;
}

static int align_up(int n, int a) {
    if (a <= 0)
        return n;
    return (n + a - 1) / a * a;
}

/* Every name a reader could have meant at the point they wrote one: the
 * variables in scope, the constants, the functions and the type names.
 *
 * "undefined variable 'gt'" is true and useless on its own -- the reader has a
 * name they typed one character away and no way to learn what it was. This
 * collects the candidates so diag_suggest can find it. The list is bounded and
 * deduplicated by `out_used` because the same function is a candidate for both
 * an undefined variable and an undefined type, and a repeated entry can only
 * make the distance look better than it is. */
/* Candidate names for a "did you mean", split by what kind of name the reader
 * was reaching for.
 *
 * The split is the point. Offering `char_at` when someone wrote `char` in type
 * position, or `len` when they wrote `Vec`, is worse than saying nothing: the
 * name is *there* in the message, and it is not a type, so the reader's next
 * move is to wonder what is wrong with the compiler. Each list holds only names
 * that would actually have worked in the position. */
typedef enum { SK_VALUE, SK_TYPE, SK_FUNC } SuggKind;

#define MAX_SUGGEST 512
static int collect_names(Parser *p, SuggKind kind, const char **out, int cap) {
    int n = 0;
    /* The type and value lists are disjoint, and the function list gets a
     * reserved tail so the built-ins always fit. */
    int limit = cap - 24;
    if (kind == SK_FUNC) {
        for (int i = 0; i < p->nsigs && n < limit; i++) {
            if (p->sigs[i].name[0] != '$')
                out[n++] = p->sigs[i].name;
        }
        /* The built-in names are candidates too, and they live in no table the
         * parser owns -- they are the two macros in limits.h, reached through
         * the same lookup a call site uses. */
#define Z_ADD_BUILTIN(nm, sym, ret, ps)                                                       \
    if (n < cap)                                                                              \
    out[n++] = nm;
        Z_BUILTIN_LIST(Z_ADD_BUILTIN)
#undef Z_ADD_BUILTIN
#define Z_ADD_INTRIN(nm, k)                                                                   \
    if (n < cap)                                                                              \
    out[n++] = nm;
        Z_INTRINSIC_LIST(Z_ADD_INTRIN)
#undef Z_ADD_INTRIN
        /* Local functions are registered under a mangled name; the name as
         * written is what the reader can type, so recover it. */
        for (LocalFn *f = p->local_fns; f != NULL && n < cap; f = f->next)
            out[n++] = f->name;
        return n;
    }
    if (kind == SK_VALUE) {
        for (Scope *s = p->scope; s != NULL; s = s->parent) {
            for (Var *v = s->vars; v != NULL && n < limit; v = v->next) {
                /* The desugarer's own temporaries are not suggestions: nobody
                 * meant to type `$env`. */
                if (v->name == NULL || v->name[0] == '$' || v->name[0] == '\0')
                    continue;
                out[n++] = v->name;
            }
            if (s->is_fn_body)
                break;
        }
        for (int i = 0; i < p->nconsts && n < limit; i++)
            out[n++] = p->consts[i].name;
    } else {
        for (int i = 0; i < p->ty.nstructs && n < limit; i++)
            out[n++] = p->ty.structs[i]->name;
        for (int i = 0; i < p->ty.nunion && n < limit; i++)
            out[n++] = p->ty.unions[i]->name;
        for (int i = 0; i < p->ty.niface && n < limit; i++)
            out[n++] = p->ty.ifaces[i]->name;
    }
    return n;
}

/* Queues the "did you mean" note for a name the compiler could not resolve.
 * `what` is the word the note uses for the kind of thing. */
static void suggest_in_scope(Parser *p, SuggKind kind, const char *what, const char *name) {
    const char *cands[MAX_SUGGEST];
    int n = collect_names(p, kind, cands, MAX_SUGGEST);
    diag_suggest_note(what, name, cands, n);
}

/* The struct behind a type that may or may not be a pointer to one, or NULL. */
static StructDef *sdef_of(Type *t) {
    if (is_kind(t, TK_STRUCT))
        return t->sdef;
    if (is_kind(t, TK_PTR) && is_kind(t->base, TK_STRUCT))
        return t->base->sdef;
    return NULL;
}

/* Every name reachable with `.` on a type: fields, auto-property backing
 * fields, and methods. A member lookup that fails is the other place a reader
 * is one keystroke from the right answer, and the list is short enough that
 * offering the whole set is more useful than picking one. */
#define MAX_MEMBERS 256
static int collect_member_names(StructDef *sd, const char **out, int cap) {
    int n = 0;
    if (sd == NULL)
        return 0;
    for (int i = 0; i < sd->nfields && n < cap; i++)
        out[n++] = sd->fields[i].name;
    for (int i = 0; i < sd->nprops && n < cap; i++)
        out[n++] = sd->props[i].name;
    for (int i = 0; i < sd->nmethods && n < cap; i++)
        out[n++] = sd->methods[i]->name;
    /* A subclass can reach everything its base declares, and a reader who
     * cannot see a member usually looked for it on the base type. */
    for (StructDef *b = sd->base; b != NULL && n < cap; b = b->base) {
        for (int i = 0; i < b->nfields && n < cap; i++)
            out[n++] = b->fields[i].name;
        for (int i = 0; i < b->nprops && n < cap; i++)
            out[n++] = b->props[i].name;
        for (int i = 0; i < b->nmethods && n < cap; i++)
            out[n++] = b->methods[i]->name;
    }
    return n;
}

static void suggest_member(StructDef *sd, const char *what, const char *name) {
    const char *cands[MAX_MEMBERS];
    int n = collect_member_names(sd, cands, MAX_MEMBERS);
    diag_suggest_note(what, name, cands, n);
}

/* The symbol a call to `name` should reach. A function declared inside another
 * one was emitted under a mangled name, and its signature is filed under that
 * same mangled name, so both the lookup and the emitted call have to go through
 * here. Anything not declared locally keeps the name the program wrote. */
static const char *resolve_fn_sym(Parser *p, const char *name) {
    for (LocalFn *f = p->local_fns; f != NULL; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f->sym;
    return name;
}

static int declare_var(Parser *p, const char *name, Type *type) {
    /* Shadowing an outer variable in a nested block is legal (as in C#); only
     * redeclaring within the same scope is an error. */
    Var *existing = lookup_var_local(p, name);
    if (existing != NULL) {
        diag_error(cur(p)->span, "variable '%s' is already declared in this scope", name);
        return -1;
    }
    /* Shadowing an outer variable in a nested block is legal; it is also almost
     * always a mistake, because the inner value is what the rest of the block
     * sees and the outer one silently stops evolving. */
    if (warn_enabled(W_SHADOWED_LOCAL) && lookup_var(p, name) != NULL)
        diag_warn(W_SHADOWED_LOCAL, cur(p)->span,
                  "'%s' shadows a declaration in an enclosing scope", name);
    /* Locals live inline in the frame, growing *upward* from their base but
     * placed *below* rbp-8 so a multi-byte value never overwrites the saved
     * frame pointer / return address. A value of `size` bytes is anchored so
     * its top byte is just below the current high-water mark. */
    int size = type_size(type);
    if (size < 8)
        size = 8;
    int align = type_align(type);
    if (align < 8)
        align = 8;
    p->next_offset = align_up(p->next_offset, align);
    int base = 8 + p->next_offset + size; /* value base = rbp - base */
    p->next_offset += size;
    Var *v = arena_alloc(p->arena, sizeof *v);
    v->name = arena_strdup(p->arena, name);
    v->type = type;
    v->offset = base;
    /* Stamped so a reference from inside a lambda can tell, by ownership alone,
     * whether this is one of its own locals or something it captured. Without
     * the stamp every variable looks captured -- the lambda's own parameters
     * included, which is how the first version ended up building an environment
     * out of the parameters it had just been given. */
    v->owner = p->cur_owner;
    v->captured = 0;
    v->is_param = 0;
    v->is_synth = 0;
    v->decl_span = cur(p)->span;
    v->used = 0;
    v->next = p->scope->vars;
    p->scope->vars = v;
    return base;
}

/* Consumes a `{ .. }` block without parsing it, so a construct that has already
 * been reported as malformed does not cascade into an error for every name
 * inside its body. Used on the error path only. */
static void skip_braced_block(Parser *p) {
    if (!at(p, T_LBRACE))
        return;
    int depth = 0;
    do {
        if (at(p, T_LBRACE))
            depth++;
        else if (at(p, T_RBRACE))
            depth--;
        else if (at(p, T_EOF))
            return;
        advance(p);
    } while (depth > 0);
}

/* ---- function signature table (pre-scan) ---- */

static void add_sig_ex(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                       int is_ext, int is_extern);

static void add_sig(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                    int is_ext) {
    add_sig_ex(p, name, ret, ptypes, nparams, is_ext, 0);
}

static void add_sig_ex(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                       int is_ext, int is_extern) {
    if (p->nsigs == p->sig_cap) {
        int ncap = p->sig_cap == 0 ? 8 : p->sig_cap * 2;
        Sig *ns = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Sig));
        if (p->sigs != NULL)
            memcpy(ns, p->sigs, (size_t)p->nsigs * sizeof(Sig));
        p->sigs = ns;
        p->sig_cap = ncap;
    }
    Sig *s = &p->sigs[p->nsigs++];
    s->name = arena_strdup(p->arena, name);
    s->ret = ret;
    s->nparams = nparams;
    s->is_ext = is_ext;
    s->is_extern = is_extern;
    s->is_export = 0;
    s->ext_recv = (is_ext && nparams > 0) ? ptypes[0] : NULL;
    s->ptypes = NULL;
    if (nparams > 0) {
        s->ptypes = arena_alloc_array(p->arena, (size_t)nparams, sizeof(Type *));
        memcpy(s->ptypes, ptypes, (size_t)nparams * sizeof(Type *));
    }
}

static Sig *find_sig(Parser *p, const char *name) {
    for (int i = 0; i < p->nsigs; i++) {
        if (strcmp(p->sigs[i].name, name) == 0)
            return &p->sigs[i];
    }
    return NULL;
}

/* Renders a declared signature the way the reader wrote it, as a buffer in the
 * arena.
 *
 * "argument 2 of 'read' expects 'int' but got 'string'" states the violation.
 * It does not state the constraint, and a constraint is what tells you what to
 * do: the reader has to go and look the declaration up to find out what `read`
 * wanted and whether the 2 is even a parameter. Putting the declaration in the
 * message is the difference between reporting a problem and handing over the
 * thing needed to fix it. */
static char *sig_text(Parser *p, const char *name, const Sig *s) {
    size_t cap = 64;
    for (int i = 0; i < s->nparams; i++)
        cap += 32;
    char *buf = arena_alloc(p->arena, cap);
    size_t off = 0;
    if (s->ret != NULL)
        off += (size_t)snprintf(buf, cap, "%s ", type_name(&p->ty, s->ret));
    off += (size_t)snprintf(buf + off, cap - off, "%s(", name);
    for (int i = 0; i < s->nparams && off < cap; i++) {
        if (i > 0)
            off += (size_t)snprintf(buf + off, cap - off, ", ");
        if (off < cap)
            off += (size_t)snprintf(buf + off, cap - off, "%s",
                                    s->ptypes[i] ? type_name(&p->ty, s->ptypes[i]) : "?");
    }
    if (off < cap)
        snprintf(buf + off, cap - off, ")");
    return buf;
}

/* Given a token index that begins a type, returns the index just past it. */
static int skip_type_tokens(Parser *p, int i);

/* If `i` points at `fn`, returns the index just past the whole function type
 * `fn(params) -> ret`, including any `*`/`[]` on the return type. Falls back to
 * just past the keyword if the type is malformed, so a syntax error is reported
 * by the real parser rather than here. */
static int skip_fn_type(Parser *p, int i) {
    i++; /* fn */
    if (i >= p->ntoks || p->toks[i].kind != T_LPAREN)
        return i;
    int depth = 0;
    for (; i < p->ntoks; i++) {
        if (p->toks[i].kind == T_LPAREN)
            depth++;
        else if (p->toks[i].kind == T_RPAREN) {
            depth--;
            if (depth == 0) {
                i++;
                break;
            }
        }
    }
    if (i < p->ntoks && p->toks[i].kind == T_ARROW)
        i++;
    /* The return type may itself be a function type. */
    if (i < p->ntoks &&
        (p->toks[i].kind == T_KW_FN || p->toks[i].kind == T_KW_METHOD ||
         p->toks[i].kind == T_KW_CLOSURE))
        return skip_fn_type(p, i);
    return skip_type_tokens(p, i);
}

static int skip_type_tokens(Parser *p, int i) {
    if (i >= p->ntoks)
        return i;
    if (p->toks[i].kind == T_KW_FN || p->toks[i].kind == T_KW_METHOD ||
        p->toks[i].kind == T_KW_CLOSURE)
        return skip_fn_type(p, i);
    if (!is_type_token(p->toks[i].kind) && p->toks[i].kind != T_IDENT)
        return i;
    if (at_result_type(p, i)) {
        /* `Result<T,E>` carries its own type arguments, so step over the whole
         * thing. Skipping only the name would leave the `<` behind, and every
         * caller here is deciding where a type ends. The angle brackets nest, and
         * the lexer makes `>>` a single token, so `Result<Result<int,string>,int>`
         * needs the `>>` to count as two closers. */
        int depth = 0;
        for (; i < p->ntoks; i++) {
            TokenKind k = p->toks[i].kind;
            if (k == T_LT) {
                depth++;
            } else if (k == T_GT) {
                if (--depth == 0) {
                    i++;
                    break;
                }
            } else if (k == T_SHR) {
                depth -= 2;
                i++;
                if (depth <= 0)
                    break;
            } else if (k == T_EOF) {
                break;
            }
        }
    } else {
        i++;
    }
    for (;;) {
        if (i < p->ntoks && p->toks[i].kind == T_STAR) {
            i++;
        } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                   p->toks[i + 1].kind == T_RBRACKET) {
            i += 2;
        } else {
            break;
        }
    }
    return i;
}

/* If `i` points at '<', returns the index just after the matching '>'. */
static int skip_generic_params(Parser *p, int i) {
    if (i >= p->ntoks || p->toks[i].kind != T_LT)
        return i;
    int depth = 0;
    while (i < p->ntoks) {
        if (p->toks[i].kind == T_LT)
            depth++;
        else if (p->toks[i].kind == T_GT) {
            depth--;
            if (depth == 0)
                return i + 1;
        }
        i++;
    }
    return i;
}

/* A top-level function starts with a (possibly pointer/array) type, a name,
 * and '('. A generic function is `name<T,...>(`. Uses absolute token indices. */
static int at_func_decl(Parser *p) {
    int j = p->pos;
    if (j < p->ntoks && (p->toks[j].kind == T_KW_EXTERN || p->toks[j].kind == T_KW_EXPORT))
        j++; /* `extern int f(...);` / `export int f(...) { }` */
    j = skip_type_tokens(p, j);
    if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
        return 0;
    j++;
    if (j < p->ntoks && p->toks[j].kind == T_LT) {
        /* Require `<IDENT` so a comparison `a < b` is not mistaken for a
         * generic parameter list. */
        if (j + 1 >= p->ntoks || p->toks[j + 1].kind != T_IDENT)
            return 0;
        j = skip_generic_params(p, j);
    }
    return j < p->ntoks && p->toks[j].kind == T_LPAREN;
}

/* An explicitly typed local declaration at the start of a statement:
 * `Type name`, where the type may be a keyword, a struct/union/enum name, a type
 * parameter or a function type, followed by any number of `*` and `[]`.
 *
 * This is the same shape at_func_decl recognizes, and the difference is the token
 * after the name: `(` makes it a nested function, anything else a declaration.
 * Requiring the name to be an identifier is what keeps an ordinary expression
 * statement out -- `foo();` and `foo.bar();` have `(` or `.` there, never a
 * second identifier. */
static int at_typed_decl(Parser *p) {
    int j = p->pos;
    if (j >= p->ntoks)
        return 0;
    if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
        return 0;
    j = skip_type_tokens(p, j);
    return j < p->ntoks && p->toks[j].kind == T_IDENT;
}

/* Registers every top-level `struct Name` so the signature pre-scan can
 * resolve struct-typed parameters (declarations are parsed later). */
static void prescan_struct_names(Parser *p) {
    for (int i = 0; i + 1 < p->ntoks; i++) {
        if ((p->toks[i].kind == T_KW_STRUCT || p->toks[i].kind == T_KW_CLASS) &&
            p->toks[i + 1].kind == T_IDENT) {
            StructDef *sd = type_define_struct(&p->ty, p->toks[i + 1].text);
            if (sd != NULL && p->toks[i].kind == T_KW_CLASS)
                sd->is_class = 1;
            i += 1;
        }
        if (p->toks[i].kind == T_KW_ENUM && p->toks[i + 1].kind == T_IDENT) {
            type_define_union(&p->ty, p->toks[i + 1].text);
            i += 1;
        }
    }
}

/* Registers the *signature* of a method without its body, for the pre-scan.
 *
 * Field layout alone was not enough to let a type be used before it is declared:
 * `Sq` has to be seen to have the methods an interface requires, and a struct
 * declared below the function that converts it had none registered yet. The body
 * is left to the real parser, which finds this entry by name and fills it in
 * rather than adding a second one -- so the method is emitted once.
 *
 * Returns the index just past the signature, or `i` unchanged if this is not a
 * method. Recognised shapes: `[virtual|override] type name ( params )`, and a
 * property `type name {`, which is a zero-argument method of the field's type. */
static int scan_method_signature(Parser *p, int i, StructDef *sd) {
    int j = i;
    int is_virtual = 0, is_override = 0;
    if (j < p->ntoks && p->toks[j].kind == T_KW_VIRTUAL) {
        is_virtual = 1;
        j++;
    } else if (j < p->ntoks && p->toks[j].kind == T_KW_OVERRIDE) {
        is_virtual = 1;
        is_override = 1;
        j++;
    }
    int tstart = j;
    if (j >= p->ntoks)
        return i;
    if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
        return i;
    /* A constructor is named after the class: `C(params)`. It has no return
     * type, so the name is where the type would be. */
    int is_ctor = 0;
    if (j < p->ntoks && p->toks[j].kind == T_IDENT &&
        strcmp(p->toks[j].text, sd->name) == 0 && j + 1 < p->ntoks &&
        p->toks[j + 1].kind == T_LPAREN) {
        is_ctor = 1;
    } else {
        j = skip_type_tokens(p, j);
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            return i;
    }
    char *mname = p->toks[j].text;
    j++;
    if (j >= p->ntoks)
        return i;

    /* A property: `type Name { ... }` reads as a zero-argument method. */
    if (!is_ctor && j < p->ntoks && p->toks[j].kind == T_LBRACE) {
        if (struct_find_method(sd, mname) != NULL)
            return i;
        Parser sub = *p;
        sub.pos = tstart;
        Type *rt = parse_type(&sub);
        if (rt == NULL)
            return i;
        StructMethod *m = arena_alloc(p->arena, sizeof *m);
        m->name = arena_strdup(p->arena, mname);
        m->ret = rt;
        m->ptypes = NULL;
        m->nparams = 0;
        m->body = NULL; /* the real parser fills this in */
        m->is_virtual = 0;
        m->is_override = 0;
        m->vtable_index = -1;
        struct_add_method(&p->ty, sd, m);
        return j; /* the '{' and everything in it is the real parser's */
    }

    if (p->toks[j].kind != T_LPAREN)
        return i;
    /* Scan the parameter list for its shape only; the types come from the real
     * parse, and a signature here is enough to satisfy an interface. */
    int depth = 1;
    int nparams = 0;
    int expect_type = 1;
    for (j++; j < p->ntoks && depth > 0; j++) {
        TokenKind k = p->toks[j].kind;
        if (k == T_LPAREN) {
            depth++;
        } else if (k == T_RPAREN) {
            depth--;
        } else if (k == T_COMMA && depth == 1) {
            expect_type = 1;
        } else if (depth == 1 && expect_type &&
                   (is_type_token(k) || k == T_IDENT)) {
            expect_type = 0;
            nparams++;
        }
    }
    if (depth != 0)
        return i;
    if (is_ctor || struct_find_method(sd, mname) != NULL)
        return i; /* a constructor is not part of an interface's surface */
    Parser sub = *p;
    sub.pos = tstart;
    Type *rt = parse_type(&sub);
    if (rt == NULL)
        return i;
    Type **ptypes = arena_alloc_array(p->arena, (size_t)(nparams > 0 ? nparams : 1),
                                      sizeof(Type *));
    int np = 0;
    for (int k = tstart; k < j && np < nparams; k++) {
        if (!is_type_token(p->toks[k].kind) && p->toks[k].kind != T_IDENT)
            continue;
        int e = skip_type_tokens(p, k);
        if (e >= p->ntoks || p->toks[e].kind != T_IDENT)
            continue;
        Parser s2 = *p;
        s2.pos = k;
        Type *pt = parse_type(&s2);
        if (pt != NULL)
            ptypes[np++] = pt;
        k = e;
    }
    StructMethod *m = arena_alloc(p->arena, sizeof *m);
    m->name = arena_strdup(p->arena, mname);
    m->ret = rt;
    m->ptypes = ptypes;
    m->nparams = np;
    m->body = NULL; /* the real parser fills this in */
    m->is_virtual = is_virtual;
    m->is_override = is_override;
    m->vtable_index = -1;
    struct_add_method(&p->ty, sd, m);
    return j;
}

/* Index just past one plain field, `type name ;`, or `i` unchanged if this is
 * not one. A plain field is exactly that shape, which is what lets the pre-scan
 * stop at the first method and leave it to the real parser -- registering a
 * method twice would emit it twice. */
static int scan_plain_field(Parser *p, int i, StructDef *sd) {
    if (i >= p->ntoks)
        return i;
    if (!is_type_token(p->toks[i].kind) && p->toks[i].kind != T_IDENT)
        return i;
    int j = skip_type_tokens(p, i);
    if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
        return i;
    char *name = p->toks[j].text;
    j++;
    if (j >= p->ntoks || p->toks[j].kind != T_SEMI)
        return i;
    j++;
    /* Resolve the type with the cursor on it, without moving the caller's. */
    Parser sub = *p;
    sub.pos = i;
    Type *ft = parse_type(&sub);
    if (ft == NULL)
        return i;
    struct_add_field(&p->ty, sd, name, ft);
    return j;
}

/* Gives every struct, class and enum its layout before anything is parsed.
 *
 * The name pre-scan above exists so a signature can name a struct declared later
 * in the file. A name alone is not enough: `p.x` needs the field and
 * `new Pt(3, 4)` needs the size, and both were filled in by parse_struct_decl,
 * which runs in source order. A struct declared after its use was therefore
 * registered as a name and nothing more, and using it read a struct with no
 * fields at all.
 *
 * Only plain fields are read here, and only as far as they run. A method, a
 * property or a constructor needs the real parser, so the scan stops at the
 * first of those; the declaration is completed as before when the parser reaches
 * it, after discarding what this pass filled in so the fields are not added
 * twice. The result is that a type may be used before it is declared. */
static void prescan_struct_fields(Parser *p) {
    for (int i = 0; i < p->ntoks; i++) {
        int is_class = p->toks[i].kind == T_KW_CLASS;
        if (!is_class && p->toks[i].kind != T_KW_STRUCT)
            continue;
        int j = i + 1;
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            continue;
        char *name = p->toks[j].text;
        j++;
        StructDef *sd = type_define_struct(&p->ty, name);
        if (sd == NULL)
            continue;
        sd->is_class = is_class;
        int base_size = 0;
        if (is_class) {
            sd->align = 8;
            sd->size = 8; /* vptr */
            base_size = 8;
            if (j < p->ntoks && p->toks[j].kind == T_COLON) {
                j++;
                if (j < p->ntoks && p->toks[j].kind == T_IDENT) {
                    Type *bt = type_find_struct(&p->ty, p->toks[j].text);
                    if (bt != NULL && bt->sdef->is_class) {
                        sd->base = bt->sdef;
                        base_size = bt->sdef->size;
                        sd->size = base_size;
                    }
                    j++;
                }
            }
        }
        if (j >= p->ntoks || p->toks[j].kind != T_LBRACE)
            continue;
        j++;
        int base_off = base_size;
        while (j < p->ntoks) {
            if (p->toks[j].kind == T_RBRACE)
                break;
            int next = scan_plain_field(p, j, sd);
            if (next != j) {
                j = next;
                continue;
            }
            next = scan_method_signature(p, j, sd);
            if (next != j) {
                j = next;
                /* Step over the body. A method's is not this pass's to read, but
                 * stopping in front of it would mean every member after the first
                 * method went unregistered -- so a struct with two methods looked
                 * like it had one. */
                if (j < p->ntoks && p->toks[j].kind == T_LBRACE) {
                    int d = 0;
                    while (j < p->ntoks) {
                        if (p->toks[j].kind == T_LBRACE) {
                            d++;
                        } else if (p->toks[j].kind == T_RBRACE) {
                            d--;
                            if (d == 0) {
                                j++;
                                break;
                            }
                        }
                        j++;
                    }
                } else if (j < p->ntoks && p->toks[j].kind == T_FATARROW) {
                    /* An expression-bodied member: skip to its semicolon. */
                    while (j < p->ntoks && p->toks[j].kind != T_SEMI && p->toks[j].kind != T_EOF)
                        j++;
                    if (j < p->ntoks)
                        j++;
                }
                continue;
            }
            break;
        }
        (void)base_off;
        if (sd->align < 1)
            sd->align = 1;
        sd->size = (sd->size + sd->align - 1) / sd->align * sd->align;
        sd->complete = 1;
        sd->prescanned = 1;
        i = j - 1;
    }

    /* Interfaces: the same problem again. A signature that names an interface is
     * prescanned before any interface declaration is parsed, so without this the
     * name did not resolve yet and a function returning one came back typed
     * `int`. The methods are read here too, because a call site resolves a method
     * name to an itab index, and that has to work whatever order the declarations
     * are in. An interface is only signatures, so there is no body to skip. */
    for (int i = 0; i < p->ntoks; i++) {
        if (p->toks[i].kind != T_KW_INTERFACE)
            continue;
        int j = i + 1;
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            continue;
        char *name = p->toks[j].text;
        j++;
        IfaceDef *id = type_define_iface(&p->ty, name);
        if (id == NULL || id->prescanned)
            continue;
        if (j >= p->ntoks || p->toks[j].kind != T_LBRACE)
            continue;
        j++;
        while (j < p->ntoks && p->toks[j].kind != T_RBRACE && p->toks[j].kind != T_EOF) {
            int tstart = j;
            if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
                break;
            j = skip_type_tokens(p, j);
            if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
                break;
            char *mname = p->toks[j].text;
            j++;
            if (j >= p->ntoks || p->toks[j].kind != T_LPAREN)
                break;
            int depth = 1;
            for (j++; j < p->ntoks && depth > 0; j++) {
                if (p->toks[j].kind == T_LPAREN)
                    depth++;
                else if (p->toks[j].kind == T_RPAREN)
                    depth--;
            }
            if (depth != 0)
                break;
            Parser sub = *p;
            sub.pos = tstart;
            Type *rt = parse_type(&sub);
            if (rt == NULL)
                break;
            /* Re-read the parameter list for its types. */
            Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
            int np = 0;
            for (int k = tstart + 1; k < j - 1 && np < Z_MAX_ARGS; k++) {
                if (!is_type_token(p->toks[k].kind) && p->toks[k].kind != T_IDENT)
                    continue;
                int e = skip_type_tokens(p, k);
                if (e >= p->ntoks || p->toks[e].kind != T_IDENT)
                    continue;
                Parser s2 = *p;
                s2.pos = k;
                Type *pt = parse_type(&s2);
                if (pt != NULL)
                    ptypes[np++] = pt;
                k = e;
            }
            if (iface_find_method(id, mname) == NULL)
                iface_add_method(&p->ty, id, mname, type_fnptr(&p->ty, ptypes, np, rt));
            if (j < p->ntoks && p->toks[j].kind == T_SEMI)
                j++;
            else
                break;
        }
        id->prescanned = 1;
        i = j - 1;
    }

    /* Enums: the same problem, with variants instead of fields. A `match` on an
     * enum declared later needs its variant list to check exhaustiveness and to
     * bind payloads. */
    for (int i = 0; i < p->ntoks; i++) {
        if (p->toks[i].kind != T_KW_ENUM)
            continue;
        int j = i + 1;
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            continue;
        char *name = p->toks[j].text;
        j++;
        UnionDef *ud = type_define_union(&p->ty, name);
        if (ud == NULL)
            continue;
        if (j >= p->ntoks || p->toks[j].kind != T_LBRACE)
            continue;
        j++;
        while (j < p->ntoks && p->toks[j].kind != T_RBRACE && p->toks[j].kind != T_EOF) {
            if (p->toks[j].kind != T_IDENT)
                break;
            char *vname = p->toks[j].text;
            j++;
            Type *ftypes[16];
            const char *fnames[16];
            int nf = 0;
            if (j < p->ntoks && p->toks[j].kind == T_LPAREN) {
                j++;
                while (j < p->ntoks && p->toks[j].kind != T_RPAREN && p->toks[j].kind != T_EOF) {
                    if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
                        break;
                    int k = skip_type_tokens(p, j);
                    if (k >= p->ntoks || p->toks[k].kind != T_IDENT)
                        break;
                    Parser sub = *p;
                    sub.pos = j;
                    Type *ft = parse_type(&sub);
                    if (nf < 16) {
                        ftypes[nf] = ft;
                        fnames[nf] = p->toks[k].text;
                    }
                    nf++;
                    j = k + 1;
                    if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                        j++;
                }
                if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                    j++;
            }
            union_add_variant(&p->ty, ud, vname, ftypes, fnames, nf);
            if (j < p->ntoks && p->toks[j].kind == T_COMMA) {
                j++;
                continue;
            }
            break;
        }
        union_finish(ud);
        ud->prescanned = 1;
        i = j - 1;
    }
}


static void prescan_signatures(Parser *p) {
    int depth = 0;
    int i = 0;
    while (i < p->ntoks) {
        Token *t = &p->toks[i];
        if (t->kind == T_LBRACE) {
            depth++;
            i++;
            continue;
        }
        if (t->kind == T_RBRACE) {
            depth--;
            i++;
            continue;
        }
        /* `extern int f(...);` declares a C symbol and `export int f(...) {...}`
         * publishes a Z one. Both need their signature recorded here so calls
         * type-check, and both make codegen use the name as written. */
        if (depth == 0 && (t->kind == T_KW_EXTERN || t->kind == T_KW_EXPORT)) {
            int kw_extern = t->kind == T_KW_EXTERN;
            int j = skip_type_tokens(p, i + 1);
            if (j + 1 < p->ntoks && p->toks[j].kind == T_IDENT &&
                p->toks[j + 1].kind == T_LPAREN) {
                j += 2;
                Type *ptypes[16];
                int np = 0;
                while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
                    if (is_type_token(p->toks[j].kind) || p->toks[j].kind == T_IDENT) {
                        int k = skip_type_tokens(p, j);
                        if (k < p->ntoks && p->toks[k].kind == T_IDENT) {
                            if (np < 16)
                                ptypes[np++] = parse_type_at(p, j);
                            j = k + 1;
                            while (j < p->ntoks && p->toks[j].kind != T_COMMA &&
                                   p->toks[j].kind != T_RPAREN)
                                j++;
                            if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                                j++;
                            continue;
                        }
                    }
                    j++;
                }
                if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_SEMI && kw_extern) {
                    Type *ret = parse_type_at(p, i + 1);
                    add_sig_ex(p, p->toks[skip_type_tokens(p, i + 1)].text, ret, ptypes, np, 0, 1);
                    i = j + 1;
                    continue;
                }
                if (j < p->ntoks && p->toks[j].kind == T_LBRACE && !kw_extern) {
                    /* Record the signature, then skip the body by brace
                     * matching, exactly as the plain function path does. */
                    Type *ret = parse_type_at(p, i + 1);
                    add_sig_ex(p, p->toks[skip_type_tokens(p, i + 1)].text, ret, ptypes, np, 0, 0);
                    Sig *ex = find_sig(p, p->toks[skip_type_tokens(p, i + 1)].text);
                    if (ex != NULL)
                        ex->is_export = 1;
                    int d = 0;
                    while (j < p->ntoks) {
                        if (p->toks[j].kind == T_LBRACE)
                            d++;
                        else if (p->toks[j].kind == T_RBRACE) {
                            d--;
                            if (d == 0) {
                                j++;
                                break;
                            }
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
            }
        }
        /* A lambda's parameter list is shaped exactly like a function
         * declaration's -- a type, a name, and a '(' -- so the scan below would
         * read `(int x) => ...` as the declaration of a function called `x` and
         * register a nonsense signature under a name the program never declared.
         *
         * Skipping the body is enough to tell them apart: a lambda can only
         * appear where an expression can, and the `=>` is unambiguous. The skip
         * runs to the statement's semicolon, tracking brace depth so a
         * block-bodied lambda's own semicolons are not mistaken for the end. */
        if (depth == 0 && t->kind == T_FATARROW) {
            int braces = 0;
            while (i < p->ntoks) {
                TokenKind k = p->toks[i].kind;
                if (k == T_LBRACE) {
                    braces++;
                } else if (k == T_RBRACE) {
                    if (braces == 0)
                        break; /* the end of the enclosing block */
                    braces--;
                } else if (k == T_SEMI && braces == 0) {
                    i++;
                    break;
                } else if (k == T_EOF) {
                    break;
                }
                i++;
            }
            continue;
        }
        if (depth == 0 && (is_type_token(t->kind) || t->kind == T_IDENT)) {
            int j = skip_type_tokens(p, i);
            if (j + 1 < p->ntoks && p->toks[j].kind == T_IDENT && p->toks[j + 1].kind == T_LPAREN) {
                /* Parse a candidate parameter list. */
                j += 2; /* skip name and '(' */
                Type *ptypes[16];
                int np = 0;
                int is_ext = 0;
                if (j < p->ntoks && p->toks[j].kind == T_KW_THIS) {
                    is_ext = 1;
                    j++;
                }
                while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
                    if (is_type_token(p->toks[j].kind) || p->toks[j].kind == T_IDENT) {
                        int k = skip_type_tokens(p, j);
                        if (k < p->ntoks && p->toks[k].kind == T_IDENT) {
                            if (np < 16)
                                ptypes[np++] = parse_type_at(p, j);
                            j = k + 1; /* skip param name */
                            while (j < p->ntoks && p->toks[j].kind != T_COMMA &&
                                   p->toks[j].kind != T_RPAREN) {
                                j++;
                            }
                            if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                                j++;
                            continue;
                        }
                    }
                    j++;
                }
                if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_LBRACE) {
                    Type *ret = parse_type_at(p, i);
                    add_sig(p, p->toks[skip_type_tokens(p, i)].text, ret, ptypes, np, is_ext);
                    /* Skip the function body via brace matching. */
                    int d = 0;
                    while (j < p->ntoks) {
                        if (p->toks[j].kind == T_LBRACE)
                            d++;
                        else if (p->toks[j].kind == T_RBRACE) {
                            d--;
                            if (d == 0) {
                                j++;
                                break;
                            }
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
                if (j < p->ntoks && p->toks[j].kind == T_FATARROW) {
                    /* Expression-bodied function: record signature, skip to ';'. */
                    Type *ret = parse_type_at(p, i);
                    add_sig(p, p->toks[skip_type_tokens(p, i)].text, ret, ptypes, np, is_ext);
                    j++;
                    while (j < p->ntoks && p->toks[j].kind != T_SEMI && p->toks[j].kind != T_EOF)
                        j++;
                    if (j < p->ntoks)
                        j++;
                    i = j;
                    continue;
                }
                i = j;
                continue;
            }
        }
        i++;
    }
}

/* ---- expression parsing (precedence climbing) ---- */

static int binop_prec(TokenKind k) {
    switch (k) {
    case T_OR:
        return 1;
    case T_AND:
        return 2;
    case T_PIPE:
        return 3;
    case T_CARET:
        return 4;
    case T_AMP:
        return 5; /* binary &: shares its token with unary address-of */
    case T_EQ:
    case T_NE:
        return 6;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        return 7;
    case T_SHL:
    case T_SHR:
        return 8;
    case T_PLUS:
    case T_MINUS:
        return 9;
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
        return 10;
    default:
        return 0;
    }
}

/* Arity of a built-in intrinsic, or 0 if the name is not one. */
static int intrinsic_arity(const char *name) {
#define Z_ARITY(n, k)                                                                       \
    if (strcmp(name, n) == 0)                                                              \
    return k;
    Z_INTRINSIC_LIST(Z_ARITY)
#undef Z_ARITY
    return 0;
}

/* Materialises a concrete value as an interface value, when that is what the
 * destination type calls for. Returns `e` unchanged when no conversion is due,
 * so every site that assigns one expression to a place of a known type can call
 * this without testing first.
 *
 * Which types satisfy an interface is decided here rather than declared, so a
 * struct implements one by having the methods and nothing else. A class goes in
 * its object pointer; a struct value has to be copied to the heap first, because
 * the cell outlives whatever frame the value was in -- the same reason a closure
 * environment is on the heap. */
static Expr *to_iface(Parser *p, Expr *e, Type *want, Span span) {
    if (e == NULL || want == NULL || !is_kind(want, TK_IFACE) || is_unk(e->type))
        return e;
    if (is_kind(e->type, TK_IFACE))
        return e; /* interface to the same interface: the cell is already there */
    IfaceDef *id = want->idef;
    if (id == NULL)
        return e;
    int *slots = arena_alloc_array(p->arena, (size_t)(id->nmethods > 0 ? id->nmethods : 1),
                                   sizeof(int));
    if (!iface_implemented_by(&p->ty, id, e->type, slots)) {
        /* Name the first method that is actually at fault, and say which kind of
         * fault it is: "does not implement I" on its own leaves the reader to work
         * out whether a method is absent or merely has the wrong shape, and
         * reporting a method that is present and correct is worse than useless. */
        const char *absent = NULL;
        const char *mismatched = NULL;
        StructDef *sd = is_kind(e->type, TK_STRUCT)  ? e->type->sdef
                        : is_kind(e->type, TK_PTR) && is_kind(e->type->base, TK_STRUCT)
                            ? e->type->base->sdef
                            : NULL;
        if (sd != NULL) {
            for (int i = 0; i < id->nmethods; i++) {
                StructMethod *m = struct_find_method(sd, id->methods[i].name);
                if (m == NULL) {
                    if (absent == NULL)
                        absent = id->methods[i].name;
                } else if (mismatched == NULL && !iface_method_matches(m, id->methods[i].sig)) {
                    mismatched = id->methods[i].name;
                }
            }
        }
        if (sd == NULL)
            diag_error(span, "'%s' cannot implement '%s': it is not a struct or class",
                       type_name(&p->ty, e->type), id->name);
        else if (absent != NULL)
            diag_error(span, "'%s' does not implement '%s': it has no method '%s'",
                       type_name(&p->ty, e->type), id->name, absent);
        else if (mismatched != NULL)
            diag_error(span, "'%s' does not implement '%s': '%s' does not match the "
                             "signature the interface requires",
                       type_name(&p->ty, e->type), id->name, mismatched);
        else
            diag_error(span, "'%s' does not implement '%s'", type_name(&p->ty, e->type),
                       id->name);
        e->type = NULL;
        return e;
    }
    StructDef *impl = is_kind(e->type, TK_STRUCT) ? e->type->sdef : e->type->base->sdef;
    /* A class satisfies an interface through its vtable, so every method it
     * offers has to be virtual: a non-virtual one has no slot to dispatch
     * through, and an itab entry pointing at it directly would bind the
     * implementation the conversion site happened to name. */
    if (impl->is_class) {
        for (int i = 0; i < id->nmethods; i++) {
            StructMethod *m = struct_find_method(impl, id->methods[i].name);
            if (m == NULL || (!m->is_virtual && !m->is_override)) {
                diag_error(span,
                           "'%s' cannot implement '%s' through a class: '%s' must be "
                           "declared 'virtual' to be dispatched dynamically",
                           type_name(&p->ty, e->type), id->name, id->methods[i].name);
                e->type = NULL;
                return e;
            }
        }
    }
    Expr *c = new_expr(p, E_IFACE, span);
    c->lhs = e;
    c->type = want;
    c->impl = impl;
    c->idef = id;
    return c;
}

/* Looks a standard-library name up in Z_BUILTIN_LIST, returning the runtime
 * symbol to call and, through the out parameters, its return type code and its
 * parameter type codes. Returns NULL when the name is not a built-in. */
static const char *builtin_lookup(const char *name, char *ret_code, const char **params) {
#define Z_FIND(n, sym, ret, ps)                                                              \
    if (strcmp(name, n) == 0) {                                                              \
        *ret_code = ret;                                                                     \
        *params = ps;                                                                        \
        return sym;                                                                          \
    }
    Z_BUILTIN_LIST(Z_FIND)
#undef Z_FIND
    return NULL;
}

/* The type a signature code stands for. */
static Type *builtin_type(Parser *p, char code) {
    switch (code) {
    case 'i':
        return type_int(&p->ty);
    case 'b':
        return type_bool(&p->ty);
    case 's':
        return type_string(&p->ty);
    case 'S':
        /* The array length is -1 for every Z-written array type; the real
         * length lives in the header the runtime allocated. */
        return type_array(&p->ty, type_string(&p->ty), -1);
    default:
        return type_void(&p->ty);
    }
}

static int is_cmp_op(TokenKind k) {
    return k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE;
}

static int is_lvalue(Expr *e) {
    return e != NULL &&
           (e->kind == E_VAR || e->kind == E_INDEX || e->kind == E_DEREF || e->kind == E_FIELD);
}

/* Maps a binary operator to its operator-overload method-name suffix, or NULL
 * if the operator cannot be overloaded. */
static const char *op_method_suffix(TokenKind op) {
    switch (op) {
    case T_PLUS:
        return "Add";
    case T_MINUS:
        return "Sub";
    case T_STAR:
        return "Mul";
    case T_SLASH:
        return "Div";
    case T_PERCENT:
        return "Rem";
    case T_EQ:
        return "Eq";
    case T_NE:
        return "Neq";
    case T_LT:
        return "Lt";
    case T_LE:
        return "Le";
    case T_GT:
        return "Gt";
    case T_GE:
        return "Ge";
    default:
        return NULL;
    }
}

/* Folds a binary operation on two integer literals at compile time. Returns a
 * fresh E_INT, or NULL if the operands are not both integer literals. */
static Expr *fold_int_binary(Parser *p, TokenKind op, Expr *lhs, Expr *rhs, Span span) {
    if (lhs->kind != E_INT || rhs->kind != E_INT)
        return NULL;
    long long a = lhs->ival, b = rhs->ival, r = 0;
    switch (op) {
    case T_PLUS:
        r = a + b;
        break;
    case T_MINUS:
        r = a - b;
        break;
    case T_STAR:
        r = a * b;
        break;
    case T_SLASH:
        if (b == 0)
            return NULL; /* leave division by zero for runtime */
        r = a / b;
        break;
    case T_PERCENT:
        if (b == 0)
            return NULL;
        r = a % b;
        break;
    case T_AMP:
        r = a & b;
        break;
    case T_PIPE:
        r = a | b;
        break;
    case T_CARET:
        r = a ^ b;
        break;
    case T_SHL:
        if (b < 0 || b > 63)
            return NULL; /* undefined in C; leave it to the runtime check */
        r = a << b;
        break;
    case T_SHR:
        if (b < 0 || b > 63)
            return NULL;
        r = a >> b;
        break;
    case T_LT:
        r = a < b;
        break;
    case T_LE:
        r = a <= b;
        break;
    case T_GT:
        r = a > b;
        break;
    case T_GE:
        r = a >= b;
        break;
    case T_EQ:
        r = a == b;
        break;
    case T_NE:
        r = a != b;
        break;
    default:
        return NULL;
    }
    Expr *e = new_expr(p, E_INT, span);
    e->ival = r;
    e->type = (op == T_LT || op == T_LE || op == T_GT || op == T_GE || op == T_EQ || op == T_NE)
                  ? type_bool(&p->ty)
                  : type_int(&p->ty);
    return e;
}

/* Folds `a && b` / `a || b` on two boolean literals. */
static Expr *fold_bool_logic(Parser *p, TokenKind op, Expr *lhs, Expr *rhs, Span span) {
    if (lhs->kind != E_BOOL || rhs->kind != E_BOOL)
        return NULL;
    Expr *e = new_expr(p, E_BOOL, span);
    e->ival = op == T_AND ? (lhs->ival && rhs->ival) : (lhs->ival || rhs->ival);
    e->type = type_bool(&p->ty);
    return e;
}

/* Wraps `e` in a conversion to `to`, unless it is already that type or its type
 * is not yet known. Both the implicit int-to-float widening and an explicit
 * `(float)` / `(int)` cast go through here, so the code generator only ever sees
 * one conversion node and never has to ask where a conversion came from. */
static Expr *make_cvt(Parser *p, Type *to, Expr *e, Span span) {
    if (e == NULL || to == NULL || is_unk(e->type) || type_equals(e->type, to))
        return e;
    Expr *c = new_expr(p, E_CVT, span);
    c->lhs = e;
    c->type = to;
    return c;
}

static Expr *make_binary(Parser *p, TokenKind op, Expr *lhs, Expr *rhs, Span span) {
    Expr *e = new_expr(p, E_BINARY, span);
    e->op = op;
    e->lhs = lhs;
    e->rhs = rhs;

    if (is_unk(lhs->type) || is_unk(rhs->type)) {
        e->type = NULL;
        return e;
    }

    /* Constant folding: fold binary operations on literal operands. */
    {
        Expr *folded = NULL;
        if (op == T_AND || op == T_OR)
            folded = fold_bool_logic(p, op, lhs, rhs, span);
        else
            folded = fold_int_binary(p, op, lhs, rhs, span);
        if (folded != NULL)
            return folded;
    }

    /* Operator overloading: if either operand is a struct declaring
     * `op_<Name>`, rewrite the binary expression into a method call on the
     * left operand. */
    const char *suffix = op_method_suffix(op);
    if (suffix != NULL) {
        Type *st = lhs->type;
        if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
            st = st->base;
        if (is_kind(st, TK_STRUCT)) {
            char mname[64];
            snprintf(mname, sizeof mname, "op_%s", suffix);
            StructMethod *m = struct_find_method(st->sdef, mname);
            if (m != NULL && m->nparams == 1) {
                /* The RHS argument: if the operator takes a struct parameter it
                 * is passed by pointer, otherwise the scalar value is passed. */
                if (!is_unk(rhs->type) && !is_unk(m->ptypes[0])) {
                    int ok = is_kind(m->ptypes[0], TK_PTR)
                                 ? type_equals(m->ptypes[0]->base, rhs->type)
                                 : type_equals(m->ptypes[0], rhs->type);
                    if (!ok) {
                        Type *want =
                            is_kind(m->ptypes[0], TK_PTR) ? m->ptypes[0]->base : m->ptypes[0];
                        diag_error(span, "operator %s expects '%s' but got '%s'",
                                   token_kind_name(op), type_name(&p->ty, want),
                                   type_name(&p->ty, rhs->type));
                    }
                }
                char *mang = arena_alloc(p->arena, strlen(st->sdef->name) + strlen(mname) + 3);
                snprintf(mang, strlen(st->sdef->name) + strlen(mname) + 3, "%s__%s", st->sdef->name,
                         mname);
                Expr *call = new_expr(p, E_CALL, span);
                call->name = mang;
                Expr **args = arena_alloc_array(p->arena, 2, sizeof(Expr *));
                args[0] = lhs; /* receiver */
                args[1] = rhs;
                call->args = args;
                call->nargs = 2;
                call->type = m->ret;
                return call;
            }
        }
    }

    /* String concatenation: string+string, string+int, int+string. */
    if (op == T_PLUS && (is_kind(lhs->type, TK_STRING) || is_kind(rhs->type, TK_STRING))) {
        int lok = is_kind(lhs->type, TK_STRING) || is_kind(lhs->type, TK_INT) ||
                  is_kind(lhs->type, TK_BOOL) || is_kind(lhs->type, TK_F64);
        int rok = is_kind(rhs->type, TK_STRING) || is_kind(rhs->type, TK_INT) ||
                  is_kind(rhs->type, TK_BOOL) || is_kind(rhs->type, TK_F64);
        if (lok && rok) {
            e->type = type_string(&p->ty);
            return e;
        }
    }

    if (op == T_AND || op == T_OR) {
        if (!is_kind(lhs->type, TK_BOOL) || !is_kind(rhs->type, TK_BOOL)) {
            diag_error(span, "operands of '%s' must be 'bool'", token_kind_name(op));
            e->type = NULL;
            return e;
        }
        e->type = type_bool(&p->ty);
        return e;
    }

    /* A generic template is validated with TK_TYPEPARAM placeholders; the real
     * operand check runs again during each monomorphized instantiation. Allow
     * type-parameter operands here so the template body type-checks. */
    int lparam = is_kind(lhs->type, TK_TYPEPARAM);
    int rparam = is_kind(rhs->type, TK_TYPEPARAM);

    if (is_cmp_op(op)) {
        /* `p == null` / `p != null`: the literal carries no type of its own, so
         * compare it against any reference-like operand. */
        if ((lhs->kind == E_NULL || rhs->kind == E_NULL) && (op == T_EQ || op == T_NE)) {
            e->type = type_bool(&p->ty);
            return e;
        }
        /* Two function pointers can be compared for identity, but not
         * ordered -- there is no meaningful "less than" for code addresses. */
        if (is_kind(lhs->type, TK_FNPTR) || is_kind(rhs->type, TK_FNPTR)) {
            int same = type_equals(lhs->type, rhs->type);
            if (!same || (op != T_EQ && op != T_NE)) {
                if (same) {
                    diag_error(span, "function pointers support only '==' and '!='");
                } else {
                    diag_error(span, "cannot compare '%s' with '%s'",
                               type_name(&p->ty, lhs->type), type_name(&p->ty, rhs->type));
                }
                e->type = NULL;
                return e;
            }
            e->type = type_bool(&p->ty);
            return e;
        }
        int lok = is_kind(lhs->type, TK_INT) || is_kind(lhs->type, TK_BOOL) || lparam;
        int rok = is_kind(rhs->type, TK_INT) || is_kind(rhs->type, TK_BOOL) || rparam;
        /* Two floats compare in all six relational operators, and a float may be
         * compared with an int by widening the int -- so `x == 0` does not
         * quietly mean "the integer zero" for a float x. */
        if (is_kind(lhs->type, TK_F64) || is_kind(rhs->type, TK_F64)) {
            if (!lparam && !rparam) {
                if (!is_kind(lhs->type, TK_F64))
                    lhs = make_cvt(p, type_f64(&p->ty), lhs, span);
                if (!is_kind(rhs->type, TK_F64))
                    rhs = make_cvt(p, type_f64(&p->ty), rhs, span);
                e->lhs = lhs;
                e->rhs = rhs;
                e->type = type_bool(&p->ty);
                return e;
            }
        }
        /* Strings compare lexicographically in all six relational operators. */
        int lstr = is_kind(lhs->type, TK_STRING);
        int rstr = is_kind(rhs->type, TK_STRING);
        int ok = (lstr && rstr) || ((lok || lstr) && (rok || rstr) &&
                                    type_equals(lhs->type, rhs->type));
        if (!ok) {
            diag_error(span, "cannot compare '%s' with '%s'", type_name(&p->ty, lhs->type),
                       type_name(&p->ty, rhs->type));
            e->type = NULL;
            return e;
        }
        e->type = type_bool(&p->ty);
        return e;
    }

    /* Arithmetic. A `float` operand makes the whole expression a `float`, and an
     * `int` operand is widened to match -- the one conversion Z performs on its
     * own, made explicit here as a node so the code generator has exactly one
     * shape to lower.
     *
     * `%` is the exception: a non-integer has no remainder, so it stays
     * integer-only rather than quietly truncating. */
    {
        int lf = is_kind(lhs->type, TK_F64), rf = is_kind(rhs->type, TK_F64);
        if ((lf || rf) && !lparam && !rparam) {
            int allowed = op == T_PLUS || op == T_MINUS || op == T_STAR || op == T_SLASH;
            if (!allowed) {
                diag_error(span, "operator %s is not defined for '%s' and '%s'",
                           token_kind_name(op), type_name(&p->ty, lhs->type),
                           type_name(&p->ty, rhs->type));
                e->type = NULL;
                return e;
            }
            if (!lf)
                lhs = make_cvt(p, type_f64(&p->ty), lhs, span);
            if (!rf)
                rhs = make_cvt(p, type_f64(&p->ty), rhs, span);
            e->lhs = lhs;
            e->rhs = rhs;
            e->type = type_f64(&p->ty);
            return e;
        }
    }

    if ((!is_kind(lhs->type, TK_INT) && !lparam) || (!is_kind(rhs->type, TK_INT) && !rparam)) {
        diag_error(span, "operator %s is not defined for '%s' and '%s'", token_kind_name(op),
                   type_name(&p->ty, lhs->type), type_name(&p->ty, rhs->type));
        e->type = NULL;
        return e;
    }
    e->type = lparam ? lhs->type : type_int(&p->ty);
    return e;
}

/* ---- generic helpers ---- */

static void bind_type(Parser *p, char *name, Type *ty) {
    if (p->ntbind == p->tbind_cap) {
        p->tbind_cap = p->tbind_cap ? p->tbind_cap * 2 : 8;
        p->tbinds = arena_alloc_array(p->arena, (size_t)p->tbind_cap, sizeof(TypeBind));
    }
    p->tbinds[p->ntbind].name = name;
    p->tbinds[p->ntbind].ty = ty;
    p->ntbind++;
}

/* True if `name` already has an active type-parameter binding. During
 * instantiation the concrete binding is pre-established, and re-parsing the
 * template must not replace it with a fresh placeholder. */
static int is_bound(Parser *p, const char *name) {
    for (int i = p->ntbind - 1; i >= 0; i--)
        if (strcmp(p->tbinds[i].name, name) == 0)
            return 1;
    return 0;
}

static Generic *find_generic(Parser *p, const char *name) {
    for (int i = 0; i < p->ngenerics; i++)
        if (strcmp(p->generics[i].name, name) == 0)
            return &p->generics[i];
    return NULL;
}

static void add_generic(Parser *p, Generic g) {
    if (p->ngenerics == p->generic_cap) {
        p->generic_cap = p->generic_cap ? p->generic_cap * 2 : 8;
        p->generics = arena_alloc_array(p->arena, (size_t)p->generic_cap, sizeof(Generic));
    }
    p->generics[p->ngenerics++] = g;
}

static void add_pending(Parser *p, Stmt *fn) {
    if (p->npending == p->pending_cap) {
        int ncap = p->pending_cap ? p->pending_cap * 2 : 8;
        Stmt **bigger = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Stmt *));
        /* Copy what is already queued. Growing into a fresh block and leaving
         * the old contents behind loses every entry added so far, so the ninth
         * queued function replaced the first eight with garbage -- which is why
         * a program with eight closures worked and one with nine crashed the
         * compiler rather than failing in a way that pointed here. */
        if (p->npending > 0)
            memcpy(bigger, p->pending, (size_t)p->npending * sizeof(Stmt *));
        p->pending = bigger;
        p->pending_cap = ncap;
    }
    p->pending[p->npending++] = fn;
}


/* Builds a mangled instance name like `max__int` or `foo__int_bool`. */
static char *mangle_instance(Parser *p, const char *name, Type **types, int n) {
    size_t cap = strlen(name) + 8 * (size_t)n + 8;
    char *buf = arena_alloc(p->arena, cap);
    size_t off = (size_t)snprintf(buf, cap, "%s__", name);
    for (int i = 0; i < n; i++) {
        const char *tn = type_name(&p->ty, types[i]);
        for (const char *c = tn; *c && off + 2 < cap; c++) {
            char ch = *c;
            buf[off++] =
                ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
                    ? ch
                    : '_';
        }
        if (i + 1 < n && off + 1 < cap)
            buf[off++] = '_';
    }
    buf[off] = '\0';
    return buf;
}

/* Re-parses a generic template `g` with its type parameters bound to the given
 * concrete types, producing a specialized function appended to the pending
 * list. Returns the mangled instance name. */
/* Structurally unifies a template parameter type against a concrete argument
 * type, binding any type parameters it contains (handles `T`, `T[]`, `T*`).
 * Returns 1 on success, 0 on a binding conflict. */
static int unify_type(Parser *p, Generic *g, Type *tmpl, Type *arg, Type **concrete, Span span) {
    if (tmpl == NULL || is_unk(tmpl) || is_unk(arg))
        return 1;
    if (is_kind(tmpl, TK_TYPEPARAM)) {
        int ti = -1;
        for (int j = 0; j < g->ntparams; j++)
            if (strcmp(g->tparams[j], tmpl->pname) == 0)
                ti = j;
        if (ti < 0)
            return 1;
        if (concrete[ti] == NULL) {
            concrete[ti] = arg;
        } else if (!type_equals(concrete[ti], arg)) {
            diag_error(span, "type parameter '%s' is bound to both '%s' and '%s'", g->tparams[ti],
                       type_name(&p->ty, concrete[ti]), type_name(&p->ty, arg));
            return 0;
        }
        return 1;
    }
    if (is_kind(tmpl, TK_PTR) && is_kind(arg, TK_PTR))
        return unify_type(p, g, tmpl->base, arg->base, concrete, span);
    if (is_kind(tmpl, TK_ARRAY) && is_kind(arg, TK_ARRAY))
        return unify_type(p, g, tmpl->base, arg->base, concrete, span);
    return 1;
}

static char *instantiate(Parser *p, Generic *g, Type **concrete, Span span) {
    char *mangled = mangle_instance(p, g->name, concrete, g->ntparams);
    (void)span;

    /* Reuse a previously-created instance for the same type arguments. */
    for (int i = 0; i < g->ninstances; i++) {
        int same = 1;
        for (int j = 0; j < g->ntparams; j++)
            if (!type_equals(g->inst_types[i][j], concrete[j])) {
                same = 0;
                break;
            }
        if (same)
            return g->mangled[i];
    }

    /* Save parser state so re-parsing the template is invisible to the caller. */
    int saved_pos = p->pos;
    Scope *saved_scope = p->scope;
    int saved_off = p->next_offset;
    Type *saved_ret = p->cur_ret;
    StructDef *saved_msd = p->cur_msd;
    Expr *saved_this = p->cur_this;
    int saved_ntbind = p->ntbind;

    for (int i = 0; i < g->ntparams; i++)
        bind_type(p, g->tparams[i], concrete[i]);

    p->pos = g->tok_start;
    p->in_instantiate = 1;
    /* A diagnostic raised while re-parsing the template points at a span in the
     * template, which is a different place from the call that caused it. Saying
     * which instance this is, and where it was asked for, is the difference
     * between a message the reader can act on and one they have to guess at. */
    diag_push_ctx(DIAG_CTX_INSTANCE, mangled, span);
    Stmt *inst = parse_func(p, 0);
    diag_pop_ctx();
    p->in_instantiate = 0;
    inst->fname = mangled;
    inst->is_generic_template = 0;
    if (inst->is_ext) {
        /* keep ext flag; receiver param is already typed concretely */
    }
    add_pending(p, inst);

    /* Register the concrete signature so the call type-checks and codegen
     * resolves the mangled symbol. Only user-visible params belong in the sig
     * (the hidden sret pointer, if any, is handled by codegen via the call's
     * result type). */
    int ivis = (is_kind(inst->ret_type, TK_STRUCT) || is_kind(inst->ret_type, TK_UNION)) ? 1 : 0;
    int invis = inst->nparams - ivis;
    Type **cpt = arena_alloc_array(p->arena, (size_t)(invis > 0 ? invis : 1), sizeof(Type *));
    for (int i = 0; i < invis; i++)
        cpt[i] = inst->params[ivis + i]->type;
    add_sig(p, mangled, inst->ret_type, cpt, invis, inst->is_ext);

    /* Cache the instance (grow, preserving prior entries). */
    int old_n = g->ninstances;
    char **old_m = old_n ? arena_alloc_array(p->arena, (size_t)old_n, sizeof(char *)) : NULL;
    Type ***old_t = old_n ? arena_alloc_array(p->arena, (size_t)old_n, sizeof(Type **)) : NULL;
    for (int i = 0; i < old_n; i++) {
        old_m[i] = g->mangled[i];
        old_t[i] = g->inst_types[i];
    }
    g->mangled = arena_alloc_array(p->arena, (size_t)(old_n + 1), sizeof(char *));
    g->inst_types = arena_alloc_array(p->arena, (size_t)(old_n + 1), sizeof(Type **));
    for (int i = 0; i < old_n; i++) {
        g->mangled[i] = old_m[i];
        g->inst_types[i] = old_t[i];
    }
    g->mangled[old_n] = mangled;
    g->inst_types[old_n] = concrete;
    g->ninstances = old_n + 1;

    /* Restore parser state. */
    p->pos = saved_pos;
    p->scope = saved_scope;
    p->next_offset = saved_off;
    p->cur_ret = saved_ret;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
    p->ntbind = saved_ntbind;
    return mangled;
}

static Expr *parse_call(Parser *p, char *name, Span span) {
    /* `base(args)` inside a class constructor calls the base constructor. */
    if (strcmp(name, "base") == 0 && at(p, T_LPAREN) && p->cur_msd != NULL &&
        p->cur_msd->base != NULL) {
        advance(p); /* '(' */
        Expr **bargs = arena_alloc_array(p->arena, 4, sizeof(Expr *));
        int bn = 0;
        if (!at(p, T_RPAREN)) {
            for (;;) {
                if (bn == 4) {
                    Expr **nb = arena_alloc_array(p->arena, (size_t)(bn * 2), sizeof(Expr *));
                    memcpy(nb, bargs, (size_t)bn * sizeof(Expr *));
                    bargs = nb;
                }
                bargs[bn++] = parse_expr(p);
                if (!match(p, T_COMMA))
                    break;
            }
        }
        match(p, T_RPAREN);
        StructDef *bs = p->cur_msd->base;
        StructMethod *bctor = struct_find_method(bs, bs->name);
        if (bctor == NULL) {
            diag_error(span, "base class '%s' has no constructor", bs->name);
            Expr *err = new_expr(p, E_INT, span);
            err->type = NULL;
            return err;
        }
        /* Base ctor: this (as the base pointer) + args. */
        int capb = bn + 1;
        Expr **full = arena_alloc_array(p->arena, (size_t)capb, sizeof(Expr *));
        full[0] = p->cur_this;
        for (int i = 0; i < bn; i++)
            full[i + 1] = bargs[i];
        char *bm = arena_alloc(p->arena, strlen(bs->name) * 2 + 4);
        snprintf(bm, strlen(bs->name) * 2 + 4, "%s__%s", bs->name, bs->name);
        Expr *call = new_expr(p, E_CALL, span);
        call->name = bm;
        call->args = full;
        call->nargs = capb;
        call->type = type_void(&p->ty);
        return call;
    }
    advance(p); /* '(' */
    Expr *e = new_expr(p, E_CALL, span);
    e->name = name;

    int cap = 4, n = 0;
    Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
    /* When the callee's signature is already known, a parameter's type can tell
     * an `Ok(...)`/`Err(...)` in argument position what to build. Resolved before
     * the arguments because the arguments are parsed first; a callee that is not
     * yet known (a generic, a builtin) simply gives no expectation, and `Ok` says
     * so rather than guessing. */
    Sig *pre = find_sig(p, name);
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (n == cap) {
                int ncap = cap * 2;
                Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(na, args, (size_t)n * sizeof(Expr *));
                args = na;
                cap = ncap;
            }
            Type *saved_expect = p->expect_result;
            p->expect_result = pre != NULL && n < pre->nparams && type_is_result(pre->ptypes[n])
                                    ? pre->ptypes[n]
                                    : NULL;
            args[n++] = parse_expr(p);
            p->expect_result = saved_expect;
            if (!match(p, T_COMMA))
                break;
        }
    }
    e->args = args;
    e->nargs = n;
    if (!match(p, T_RPAREN)) {
        diag_error(cur(p)->span, "expected ')' after arguments");
    }

    /* Builtin print. */
    if (strcmp(name, "print") == 0) {
        if (n != 1) {
            diag_error(span, "print expects 1 argument but got %d", n);
            e->type = type_void(&p->ty);
            return e;
        }
        Type *at0 = args[0]->type;
        /* An unknown type means the argument already failed to compile and the
         * real error has been reported. Saying anything about it here is a
         * second complaint about one mistake, and it buries the first. */
        if (!is_unk(at0) && !is_kind(at0, TK_INT) && !is_kind(at0, TK_BOOL) &&
            !is_kind(at0, TK_STRING) && !is_kind(at0, TK_F64)) {
            diag_error_code(span, "type_mismatch",
                            "print expects 'int', 'bool', 'float' or 'string' but got '%s'",
                            type_name(&p->ty, at0));
        }
        e->type = type_void(&p->ty);
        return e;
    }

    /* Built-in intrinsic. Checked only after the user-function lookup would
     * have failed, so a declaration named `min` shadows the intrinsic. */
    if (find_sig(p, name) == NULL && find_generic(p, name) == NULL) {
        int arity = intrinsic_arity(name);
        if (arity > 0) {
            if (n != arity) {
                diag_error(span, "'%s' expects %d argument%s but got %d", name, arity,
                           arity == 1 ? "" : "s", n);
                e->type = NULL;
                return e;
            }
            for (int i = 0; i < n; i++) {
                if (!is_kind(args[i]->type, TK_INT) && !is_unk(args[i]->type)) {
                    diag_error(args[i]->span, "argument %d of '%s' expects 'int' but got '%s'",
                               i + 1, name, type_name(&p->ty, args[i]->type));
                }
            }
            Expr *in = new_expr(p, E_INTRINSIC, span);
            in->name = arena_strdup(p->arena, name);
            in->args = args;
            in->nargs = n;
            in->type = type_int(&p->ty);
            return in;
        }
    }

    /* `Ok(v)` and `Err(e)`: the two ways to build a `Result`.
     *
     * Which one is meant, and with which types, has to come from where the value
     * is going -- the declared type of the variable, the enclosing function's
     * return type, or a parameter's type. There is no way to spell it inline:
     * `Ok(3)` alone does not say what the error type is, and guessing one would
     * make `Result<int,string>` and `Result<int,io>` silently different types
     * that then fail to unify. The expectation is recorded in p->expect_result
     * by the three places that know it.
     *
     * Resolved before the user-function lookup only in the sense that this is
     * tried first; a program that declares its own `Ok` still gets its own. */
    if ((strcmp(name, "Ok") == 0 || strcmp(name, "Err") == 0) && find_sig(p, name) == NULL &&
        find_generic(p, name) == NULL) {
        int is_ok = name[0] == 'O';
        if (n != 1) {
            diag_error(span, "'%s' expects 1 argument but got %d", name, n);
            e->type = NULL;
            return e;
        }
        Type *want = p->expect_result;
        if (want == NULL || !type_is_result(want)) {
            diag_error(span,
                       "'%s' needs to know the other type: declare the variable's type, or "
                       "use it where a 'Result<T, E>' is expected",
                       name);
            e->type = NULL;
            return e;
        }
        Type *slot = is_ok ? type_result_ok(want) : type_result_err(want);
        if (!is_unk(args[0]->type) && !type_equals(args[0]->type, slot))
            diag_error(args[0]->span, "argument 1 of '%s' expects '%s' but got '%s'", name,
                       type_name(&p->ty, slot), type_name(&p->ty, args[0]->type));
        Expr *u = new_expr(p, E_UNIONLIT, span);
        u->variant = &want->udef->variants[is_ok ? 0 : 1];
        u->args = args;
        u->nargs = 1;
        u->type = want;
        *e = *u;
        return e;
    }

    /* Standard-library built-in. Resolved after the user-function lookup would
     * have failed, for the same shadowing reason as an intrinsic: a program that
     * declares its own `len` gets its own. The call is checked against the
     * built-in's real signature and given a real result type, so from here on it
     * is an ordinary call and needs no special handling anywhere else. */
    if (find_sig(p, name) == NULL && find_generic(p, name) == NULL) {
        char ret_code = 0;
        const char *params = NULL;
        const char *sym = builtin_lookup(name, &ret_code, &params);
        if (sym != NULL) {
            int want = (int)strlen(params);
            if (n != want) {
                diag_error(span, "'%s' expects %d argument%s but got %d", name, want,
                           want == 1 ? "" : "s", n);
                e->type = NULL;
                return e;
            }
            for (int i = 0; i < n; i++) {
                if (is_unk(args[i]->type))
                    continue;
                Type *wt = builtin_type(p, params[i]);
                if (!type_equals(args[i]->type, wt)) {
                    diag_error(args[i]->span, "argument %d of '%s' expects '%s' but got '%s'",
                               i + 1, name, type_name(&p->ty, wt),
                               type_name(&p->ty, args[i]->type));
                }
            }
            /* is_extern makes codegen call the symbol verbatim, which is what
             * reaches the runtime instead of the z$ namespaced name. */
            e->name = arena_strdup(p->arena, sym);
            e->is_extern = 1;
            e->type = builtin_type(p, ret_code);
            return e;
        }
    }

    /* Generic function: infer type arguments from the call and instantiate. */
    Generic *g = find_generic(p, name);
    if (g != NULL) {
        if (g->nvisible != n) {
            diag_error(span, "'%s' expects %d argument%s but got %d", name, g->nvisible,
                       g->nvisible == 1 ? "" : "s", n);
            e->type = NULL;
            return e;
        }
        /* Bind each type parameter by unifying the template parameter types
         * with the argument types (handles `T`, `T[]`, `T*`). */
        Type **concrete = arena_alloc_array(p->arena, (size_t)g->ntparams, sizeof(Type *));
        int ok = 1;
        for (int i = 0; i < n && ok; i++)
            ok = unify_type(p, g, g->ptypes[g->vis_start + i], args[i]->type, concrete,
                            args[i]->span);
        for (int j = 0; j < g->ntparams; j++) {
            if (concrete[j] == NULL || is_unk(concrete[j])) {
                diag_error(span, "cannot infer type parameter '%s' of '%s'", g->tparams[j], name);
                ok = 0;
            }
        }
        if (!ok) {
            e->type = NULL;
            return e;
        }
        char *mangled = instantiate(p, g, concrete, span);
        name = mangled;
        e->name = mangled;
    }

    /* User function. A function declared inside another one answers to a mangled
     * symbol, and its signature was filed under that symbol, so both the lookup
     * and the emitted call go through the same resolution. */
    const char *sym = resolve_fn_sym(p, name);
    Sig *s = find_sig(p, sym);
    if (s == NULL) {
        suggest_in_scope(p, SK_FUNC, "function", name);
        diag_error_code(span, "undefined_function", "undefined function '%s'", name);
        e->type = NULL;
        return e;
    }
    e->name = arena_strdup(p->arena, sym);
    if (s->nparams != n) {
        diag_note("'%s' is declared %s", name, sig_text(p, name, s));
        diag_error_code(span, "wrong_arity", "'%s' expects %d argument%s but got %d", name,
                        s->nparams, s->nparams == 1 ? "" : "s", n);
        e->type = NULL;
        return e;
    }
    if (n > Z_MAX_ARGS) {
        diag_error(span, "call to '%s' passes %d arguments but the limit is %d", name, n,
                   Z_MAX_ARGS);
        e->type = NULL;
        return e;
    }
    int said_sig = 0;
    for (int i = 0; i < n; i++) {
        /* A concrete value going to an interface parameter is boxed on the way
         * in, so the conversion is made before the types are compared. */
        if (is_kind(s->ptypes[i], TK_IFACE) && !is_kind(args[i]->type, TK_IFACE))
            args[i] = to_iface(p, args[i], s->ptypes[i], args[i]->span);
        if (!is_unk(args[i]->type) && !is_unk(s->ptypes[i]) &&
            !type_equals(args[i]->type, s->ptypes[i])) {
            /* Every wrong argument is reported -- fixing one and rebuilding to
             * discover the next is a worse loop than being told all at once --
             * but the declaration is shown once, since repeating it under each
             * one turns a diagnosis into a wall. */
            if (!said_sig) {
                diag_note("'%s' is declared %s", name, sig_text(p, name, s));
                said_sig = 1;
            }
            if (type_widens_to(s->ptypes[i], args[i]->type))
                diag_note("an 'int' converts to a 'float' implicitly; the reverse "
                          "truncates, so it needs an explicit 'float(x)'");
            diag_error_code(args[i]->span, "type_mismatch",
                            "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                            type_name(&p->ty, s->ptypes[i]), type_name(&p->ty, args[i]->type));
        }
    }
    e->type = s->ret;
    /* An `extern` target keeps its own symbol: the C definition is the one
     * that has to be found at link time. An `export`ed one does too, so calls
     * inside Z reach the same symbol C does. */
    e->is_extern = s->is_extern || s->is_export;
    return e;
}

/* Postfix suffixes: indexing, .length, postfix ++/--, and `?`. */
static Expr *parse_postfix(Parser *p, Expr *e) {
    for (;;) {
        if (at(p, T_QUESTION) && type_is_result(e->type)) {
            /* `expr?` -- yield the Ok payload, or return the error from here.
             *
             * This cannot desugar to statements: it is an expression, and the
             * early return has to happen in the middle of evaluating whatever
             * contains it. So it stays a node and the code generator emits the
             * branch inline, the same sequence an explicit `match` followed by a
             * `return` would produce.
             *
             * A `?` whose left side is *not* a Result is the ternary's, and is
             * left alone: the two share a token, and a Result is never a valid
             * condition, so the type decides and nothing well-typed is
             * ambiguous. */
            Span span = cur(p)->span;
            advance(p);
            Expr *t = new_expr(p, E_TRY, span);
            t->lhs = e;
            Type *rt = e->type;
            Type *ret = p->cur_ret;
            if (is_unk(ret) || !type_is_result(ret)) {
                diag_error(span,
                           "'?' can only be used in a function that returns a "
                           "'Result<T, E>'; this one returns '%s'",
                           is_unk(ret) ? "nothing" : type_name(&p->ty, ret));
                t->type = NULL;
                e = t;
                continue;
            }
            Type *want_err = type_result_err(rt);
            Type *have_err = type_result_err(ret);
            if (!type_equals(want_err, have_err)) {
                diag_error(span,
                           "'?' propagates '%s' but this function returns '%s'",
                           type_name(&p->ty, want_err), type_name(&p->ty, have_err));
                t->type = NULL;
                e = t;
                continue;
            }
            t->type = type_result_ok(rt);
            e = t;
            continue;
        }
        if (at(p, T_LBRACKET)) {
            Span span = cur(p)->span;
            advance(p);
            Expr *idx = parse_expr(p);
            if (!match(p, T_RBRACKET)) {
                diag_error(cur(p)->span, "expected ']' after index");
            }
            Expr *ix = new_expr(p, E_INDEX, span);
            ix->lhs = e;
            ix->rhs = idx;
            Type *bt = e->type;
            if (is_kind(bt, TK_ARRAY) || is_kind(bt, TK_PTR)) {
                ix->type = bt->base;
            } else {
                if (!is_unk(bt)) {
                    diag_error(span, "cannot index %s", type_name(&p->ty, bt));
                }
                ix->type = NULL;
            }
            e = ix;
        } else if (at(p, T_DOT)) {
            Span span = cur(p)->span;
            advance(p);
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected field name after '.'");
                return e;
            }
            char *name = cur(p)->text;
            advance(p);
            /* A call on an interface value: `v.m(args)`. The method name is
             * resolved to an index in the interface's declaration order, and that
             * index is what the call site loads from the itab. Everything about
             * the signature comes from the interface, not from whatever type
             * happens to be in the cell -- that is the point of the interface. */
            if (is_kind(e->type, TK_IFACE)) {
                IfaceDef *id = e->type->idef;
                IfaceMethod *im = iface_find_method(id, name);
                if (im == NULL) {
                    diag_error(span, "'%s' does not require a method '%s'",
                               id != NULL ? id->name : "this interface", name);
                    return e;
                }
                if (!at(p, T_LPAREN)) {
                    diag_error(span, "'%s' is a method, so it needs an argument list",
                               name);
                    return e;
                }
                advance(p); /* '(' */
                int cap = 4, n = 0;
                Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap * 2;
                            Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                match(p, T_RPAREN);
                int want = im->sig->nparams;
                if (n != want) {
                    diag_error(span, "'%s' expects %d argument%s but got %d", name, want,
                               want == 1 ? "" : "s", n);
                } else {
                    for (int i = 0; i < n; i++) {
                        if (is_kind(im->sig->ptypes[i], TK_IFACE) &&
                            !is_kind(args[i]->type, TK_IFACE))
                            args[i] = to_iface(p, args[i], im->sig->ptypes[i], args[i]->span);
                        if (!is_unk(args[i]->type) && !is_unk(im->sig->ptypes[i]) &&
                            !type_equals(args[i]->type, im->sig->ptypes[i]))
                            diag_error(args[i]->span, "argument %d of '%s' expects '%s' but got '%s'",
                                       i + 1, name, type_name(&p->ty, im->sig->ptypes[i]),
                                       type_name(&p->ty, args[i]->type));
                    }
                }
                Expr *ic = new_expr(p, E_ICALL, span);
                ic->lhs = e;
                ic->args = args;
                ic->nargs = n;
                ic->vtable_index = (int)(im - id->methods);
                ic->type = im->sig->ret;
                e = parse_postfix(p, ic);
                continue;
            }
            /* method call: recv.Name(args) */
            Type *st = e->type;
            if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
                st = st->base;
            /* A field of function-pointer type, called like a method, is a call
             * through that pointer rather than a missing method. Detect it here
             * so the method-call block is skipped entirely and the field access
             * below runs; the postfix loop then dispatches the call. */
            int fp_field = 0;
            if (is_kind(st, TK_STRUCT)) {
                Field *fld0 = struct_find_field(st->sdef, name);
                fp_field = fld0 != NULL && (is_kind(fld0->type, TK_FNPTR) ||
                                            is_kind(fld0->type, TK_MPTR));
            }
            /* `&obj.M`: bind the receiver now and yield a callable. */
            if (p->take_method_addr && is_kind(st, TK_STRUCT) && !fp_field &&
                !at(p, T_LPAREN)) {
                StructMethod *bm = struct_find_method(st->sdef, name);
                if (bm == NULL) {
                    suggest_member(st->sdef, "method", name);
                    diag_error_code(span, "no_such_method", "type %s has no method '%s'",
                                    st->sdef->name, name);
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                    continue;
                }
                if (!st->sdef->is_class) {
                    diag_error(span,
                               "cannot take a method pointer to struct '%s': its methods take "
                               "the receiver by value, so there is nothing to bind",
                               st->sdef->name);
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                    continue;
                }
                /* A class variable already holds the object pointer, so it is
                 * the receiver; `*p` on a class pointer yields the same. */
                Expr *rcv = e;
                if (is_kind(e->type, TK_PTR) && e->kind == E_DEREF)
                    rcv = e->lhs;
                /* A method's parameter list excludes the receiver, so nparams
                 * and ptypes are exactly what a caller sees. */
                int np = bm->nparams;
                Type **pts = NULL;
                if (np > 0) {
                    pts = arena_alloc_array(p->arena, (size_t)np, sizeof(Type *));
                    for (int i = 0; i < np; i++)
                        pts[i] = bm->ptypes[i];
                }
                char *mang = arena_alloc(p->arena, strlen(st->sdef->name) + strlen(name) + 3);
                snprintf(mang, strlen(st->sdef->name) + strlen(name) + 3, "%s__%s",
                         st->sdef->name, name);
                Expr *mp = new_expr(p, E_MPTR, span);
                mp->name = mang;
                mp->lhs = rcv;
                /* A virtual method is bound through the object's vtable, so the
                 * pointer dispatches on the runtime type rather than pinning the
                 * implementation named here. */
                mp->vtable_index = bm->is_virtual ? bm->vtable_index : -1;
                mp->type = type_mptr(&p->ty, pts, np, bm->ret);
                e = mp;
                continue;
            }
            if (at(p, T_LPAREN) && is_kind(st, TK_STRUCT) && !fp_field) {
                StructMethod *m = struct_find_method(st->sdef, name);
                if (m == NULL) {
                    suggest_member(st->sdef, "method", name);
                    diag_error_code(span, "no_such_method", "type %s has no method '%s'",
                                    type_name(&p->ty, st), name);
                    while (!at(p, T_RPAREN) && !at(p, T_EOF))
                        advance(p);
                    match(p, T_RPAREN);
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                    continue;
                }
                advance(p); /* '(' */
                int cap = 4, n = 0;
                Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                args[n++] = e; /* receiver becomes `this` */
                if (n == cap) {
                    int ncap = cap * 2;
                    Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                    memcpy(na, args, (size_t)n * sizeof(Expr *));
                    args = na;
                    cap = ncap;
                }
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap * 2;
                            Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                if (!match(p, T_RPAREN)) {
                    diag_error(cur(p)->span, "expected ')' after method arguments");
                }
                int nargs = n - 1;
                if (nargs != m->nparams) {
                    diag_error(span, "'%s' expects %d argument%s but got %d", name, m->nparams,
                               m->nparams == 1 ? "" : "s", nargs);
                } else {
                    for (int i = 0; i < nargs; i++) {
                        if (!is_unk(args[i + 1]->type) && !is_unk(m->ptypes[i]) &&
                            !type_equals(args[i + 1]->type, m->ptypes[i])) {
                            diag_error(args[i + 1]->span,
                                       "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                                       type_name(&p->ty, m->ptypes[i]),
                                       type_name(&p->ty, args[i + 1]->type));
                        }
                    }
                }
                /* Mangle against the DECLARING class so inherited non-virtual
                 * methods call the base implementation. */
                StructDef *owner = struct_method_owner(st->sdef, name);
                if (owner == NULL)
                    owner = st->sdef;
                char *mang = arena_alloc(p->arena, strlen(owner->name) + strlen(name) + 3);
                snprintf(mang, strlen(owner->name) + strlen(name) + 3, "%s__%s", owner->name, name);
                if (st->sdef->is_class && m->is_virtual) {
                    /* Dynamic dispatch through the receiver's vtable. */
                    Expr *call = new_expr(p, E_VCALL, span);
                    call->name = mang; /* direct fallback / debug */
                    call->vtable_index = m->vtable_index;
                    call->args = args;
                    call->nargs = n;
                    call->type = m->ret;
                    e = call;
                    continue;
                }
                Expr *call = new_expr(p, E_CALL, span);
                call->name = mang;
                call->args = args;
                call->nargs = n;
                call->type = m->ret;
                e = call;
                continue;
            }
            /* Extension method: recv.Name(args) -> Name(recv, args). */
            if (at(p, T_LPAREN)) {
                Sig *ext = NULL;
                for (int si = 0; si < p->nsigs; si++) {
                    Sig *cand = &p->sigs[si];
                    if (cand->is_ext && strcmp(cand->name, name) == 0) {
                        if (!is_unk(e->type) && !is_unk(cand->ext_recv) &&
                            type_equals(e->type, cand->ext_recv)) {
                            ext = cand;
                            break;
                        }
                        if (ext == NULL)
                            ext = cand; /* fallback: first by name */
                    }
                }
                if (ext != NULL) {
                    advance(p); /* '(' */
                    int cap = 4, n = 0;
                    Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                    args[n++] = e; /* receiver becomes the first argument */
                    if (n == cap) {
                        int ncap = cap * 2;
                        Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                        memcpy(na, args, (size_t)n * sizeof(Expr *));
                        args = na;
                        cap = ncap;
                    }
                    if (!at(p, T_RPAREN)) {
                        for (;;) {
                            if (n == cap) {
                                int ncap = cap * 2;
                                Expr **na =
                                    arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                                memcpy(na, args, (size_t)n * sizeof(Expr *));
                                args = na;
                                cap = ncap;
                            }
                            args[n++] = parse_expr(p);
                            if (!match(p, T_COMMA))
                                break;
                        }
                    }
                    if (!match(p, T_RPAREN))
                        diag_error(cur(p)->span, "expected ')' after method arguments");
                    if (n != ext->nparams) {
                        diag_error(span, "'%s' expects %d argument%s but got %d", name,
                                   ext->nparams, ext->nparams == 1 ? "" : "s", n - 1);
                    }
                    Expr *call = new_expr(p, E_CALL, span);
                    call->name = arena_strdup(p->arena, ext->name);
                    call->args = args;
                    call->nargs = n;
                    call->type = ext->ret;
                    e = call;
                    continue;
                }
            }
            if (is_kind(e->type, TK_ARRAY) && strcmp(name, "length") == 0) {
                Expr *f = new_expr(p, E_FIELD, span);
                f->lhs = e;
                f->name = name;
                f->type = type_int(&p->ty);
                e = f;
            } else {
                /* Struct field access: s.f (also through a struct pointer). */
                Type *st = e->type;
                if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
                    st = st->base;
                if (is_kind(st, TK_STRUCT)) {
                    Field *fld = struct_find_field(st->sdef, name);
                    if (fld == NULL) {
                        suggest_member(st->sdef, "field or method", name);
                        diag_error_code(span, "no_such_field", "type %s has no field '%s'",
                                        type_name(&p->ty, st), name);
                        e = new_expr(p, E_INT, span);
                        e->type = NULL;
                    } else {
                        Expr *f = new_expr(p, E_FIELD, span);
                        f->lhs = e;
                        f->name = name;
                        f->field_off = fld->offset;
                        f->type = fld->type;
                        e = f;
                    }
                } else {
                    if (!is_unk(e->type)) {
                        suggest_member(sdef_of(e->type), "field or method", name);
                        diag_error_code(span, "no_such_field", "type %s has no field '%s'",
                                        type_name(&p->ty, e->type), name);
                    }
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                }
            }
        } else if (at(p, T_PLUSPLUS) || at(p, T_MINUSMINUS)) {
            /* Desugar postfix ++/-- to `x = x ± 1` (value is the new value; a
             * documented simplification of true postfix semantics). */
            TokenKind op = cur(p)->kind == T_PLUSPLUS ? T_PLUS : T_MINUS;
            Span span = cur(p)->span;
            advance(p);
            if (!is_lvalue(e) || !is_kind(e->type, TK_INT)) {
                if (is_lvalue(e) && !is_unk(e->type)) {
                    diag_error(span, "operator %s requires 'int'", token_kind_name(op));
                }
                e = new_expr(p, E_INT, span);
                e->type = type_int(&p->ty);
                e->ival = 0;
                continue;
            }
            Expr *one = new_expr(p, E_INT, span);
            one->ival = 1;
            one->type = type_int(&p->ty);
            Expr *asg = new_expr(p, E_ASSIGN, span);
            asg->op = op;
            asg->compound = 1;
            asg->lhs = e;
            asg->rhs = one; /* `x = x op 1` */
            asg->type = type_int(&p->ty);
            e = asg;
        } else if (at(p, T_LPAREN) &&
                   (is_kind(e->type, TK_FNPTR) || is_kind(e->type, TK_MPTR))) {
            /* A call through a function pointer reached by a postfix: a struct
             * field, an array element, or a dereference. The bare-identifier
             * case is handled in parse_primary, where the name is still
             * current; here it is not. */
            Span span = cur(p)->span;
            Type *ft = e->type;
            advance(p); /* '(' */
            Expr **args = NULL;
            int n = 0, cap = 0;
            if (!at(p, T_RPAREN)) {
                for (;;) {
                    if (n == cap) {
                        int ncap = cap == 0 ? 4 : cap * 2;
                        Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                        if (args != NULL)
                            memcpy(na, args, (size_t)n * sizeof(Expr *));
                        args = na;
                        cap = ncap;
                    }
                    args[n++] = parse_expr(p);
                    if (!match(p, T_COMMA))
                        break;
                }
            }
            if (!match(p, T_RPAREN))
                diag_error(cur(p)->span, "expected ')' after arguments");
            if (n != ft->nparams) {
                diag_error(span, "function pointer expects %d argument%s but got %d",
                           ft->nparams, ft->nparams == 1 ? "" : "s", n);
            } else {
                for (int i = 0; i < n; i++) {
                    if (!is_unk(args[i]->type) && !type_equals(args[i]->type, ft->ptypes[i]))
                        diag_error(args[i]->span,
                                   "argument %d expects '%s' but got '%s'", i + 1,
                                   type_name(&p->ty, ft->ptypes[i]),
                                   type_name(&p->ty, args[i]->type));
                }
            }
            /* A bound receiver occupies rdi, which is also where a
             * struct-returning call puts its result buffer. A plain function
             * pointer has no receiver, so it returns a struct normally. */
            if (is_kind(ft, TK_MPTR) &&
                (is_kind(ft->ret, TK_STRUCT) || is_kind(ft->ret, TK_UNION)))
                diag_error(span, "a method pointer cannot return a struct");
            Expr *ic = new_expr(p, E_ICALL, span);
            ic->lhs = e;
            ic->args = args;
            ic->nargs = n;
            ic->type = ft->ret;
            e = ic;
        } else {
            break;
        }
    }
    return e;
}

/* Parses a base type followed by `*` stars but NOT `[]` (used by `new T[n]`,
 * where the brackets are the element count). */
static Type *parse_type_base(Parser *p) {
    /* `new fn(int) -> int[3]` allocates an array of function pointers, so a
     * function type is a valid element type here. */
    if (at(p, T_KW_FN) || at(p, T_KW_METHOD) || at(p, T_KW_CLOSURE)) {
        Type *ft = parse_fn_type(p);
        if (ft != NULL) {
            while (at(p, T_STAR)) {
                advance(p);
                ft = type_ptr(&p->ty, ft);
            }
        }
        return ft;
    }
    Type *t = base_type_or_name(p, cur(p));
    if (t == NULL)
        return NULL;
    advance(p);
    while (at(p, T_STAR)) {
        advance(p);
        t = type_ptr(&p->ty, t);
    }
    return t;
}

/* new Name(a, b, ...) -> a struct value; new T[n] -> a heap array. */
static Expr *parse_new(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* 'new' */

    /* Struct literal: `new Name(args)`. */
    if (cur(p)->kind == T_IDENT && peek(p, 1)->kind == T_LPAREN) {
        char *sname = cur(p)->text;
        advance(p);
        advance(p); /* '(' */
        Type *st = type_find_struct(&p->ty, sname);
        if (st == NULL) {
            suggest_in_scope(p, SK_TYPE, "type", sname);
            diag_error_code(start, "unknown_type", "unknown type '%s'", sname);
            int guard = 0;
            while (!at(p, T_RPAREN) && !at(p, T_EOF) && guard++ < 256)
                advance(p);
            match(p, T_RPAREN);
            return new_expr(p, E_INT, start);
        }
        int cap = 4, n = 0;
        Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
        if (!at(p, T_RPAREN)) {
            for (;;) {
                if (n == cap) {
                    int ncap = cap * 2;
                    Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                    memcpy(na, args, (size_t)n * sizeof(Expr *));
                    args = na;
                    cap = ncap;
                }
                args[n++] = parse_expr(p);
                if (!match(p, T_COMMA))
                    break;
            }
        }
        if (!match(p, T_RPAREN))
            diag_error(cur(p)->span, "expected ')' after struct fields");
        if (st->sdef->is_class) {
            /* `new C(args)`: heap object. A constructor is a method named like
             * the class; if absent, fields are simply zero-initialized. */
            StructMethod *ctor = struct_find_method(st->sdef, sname);
            Expr *e = new_expr(p, E_NEWCLASS, start);
            e->args = args;
            e->nargs = n;
            e->type = type_ptr(&p->ty, st);
            if (ctor != NULL) {
                if (ctor->nparams != n) {
                    diag_error(start, "constructor '%s' expects %d argument%s but got %d", sname,
                               ctor->nparams, ctor->nparams == 1 ? "" : "s", n);
                }
                char *cm = arena_alloc(p->arena, strlen(sname) * 2 + 4);
                snprintf(cm, strlen(sname) * 2 + 4, "%s__%s", sname, sname);
                e->name = cm;
            }
            return e;
        }
        /* The positional constructor takes only the real (non-property) fields. */
        int nreal = 0;
        for (int i = 0; i < st->sdef->nfields; i++) {
            if (!st->sdef->fields[i].is_prop)
                nreal++;
        }
        if (n != nreal) {
            diag_error(start, "'%s' expects %d field%s but got %d", sname, nreal,
                       nreal == 1 ? "" : "s", n);
        } else {
            int fi = 0;
            for (int i = 0; i < st->sdef->nfields; i++) {
                Field *f = &st->sdef->fields[i];
                if (f->is_prop)
                    continue;
                if (fi < n && !is_unk(args[fi]->type) && !type_equals(args[fi]->type, f->type)) {
                    diag_error(args[fi]->span, "field '%s' expects '%s' but got '%s'", f->name,
                               type_name(&p->ty, f->type), type_name(&p->ty, args[fi]->type));
                }
                fi++;
            }
        }
        Expr *e = new_expr(p, E_STRUCTLIT, start);
        e->name = sname;
        e->args = args;
        e->nargs = n;
        e->type = st;
        return e;
    }

    Type *base = parse_type_base(p);
    if (base == NULL) {
        diag_error(cur(p)->span, "expected element type after 'new'");
        return new_expr(p, E_INT, start);
    }
    if (!match(p, T_LBRACKET)) {
        diag_error(cur(p)->span, "expected '[' after 'new' element type");
        return new_expr(p, E_INT, start);
    }
    Expr *count = parse_expr(p);
    if (!match(p, T_RBRACKET)) {
        diag_error(cur(p)->span, "expected ']' after array length");
    }
    if (!is_unk(count->type) && !is_kind(count->type, TK_INT)) {
        diag_error(start, "array length must be 'int' but got %s", type_name(&p->ty, count->type));
    }
    Expr *e = new_expr(p, E_NEW, start);
    e->lhs = count;
    e->type = type_array(&p->ty, base, -1);
    return e;
}

/* Builds an interned string literal expression from a byte range, returning a
 * fresh E_STRING node of type string. */
static Expr *make_str_expr(Parser *p, const char *bytes, int len, Span span) {
    Expr *e = new_expr(p, E_STRING, span);
    e->str_id = string_intern(p->strings, bytes, len);
    e->type = type_string(&p->ty);
    return e;
}

/* Builds a literal segment of an interpolated string, dropping the backslash
 * from an escaped brace.
 *
 * The lexer leaves `\{` and `\}` in an interpolated string's raw text precisely
 * so that this pass can tell a literal brace from a hole delimiter: by the time
 * the holes are split out, an escaped brace and a real one are otherwise the
 * same byte, and `$\"a\{b\"` would try to interpolate a variable named b. */
static Expr *make_interp_part(Parser *p, const char *src, int len, Span span) {
    char *out = arena_alloc(p->arena, (size_t)len + 1);
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len && (src[i + 1] == '{' || src[i + 1] == '}'))
            i++;
        out[n++] = src[i];
    }
    return make_str_expr(p, out, n, span);
}

/* Lexes and parses an embedded interpolation expression (the text inside
 * {..} of $"...{expr}...") as a sub-stream, sharing the enclosing scope and
 * type context so local variables and the string table resolve correctly. */
static Expr *parse_embedded(Parser *p, const char *src, int len, Span span) {
    (void)span;
    int n = 0;
    /* Use the real string table so embedded string literals share ids with
     * the outer program. */
    Token *toks = lex_all(p->arena, src, len, p->strings, &n);
    Parser sub = *p; /* share arena, scope, types */
    sub.toks = toks;
    sub.ntoks = n;
    sub.pos = 0;
    Expr *e = parse_expr(&sub);
    return e;
}

/* Parses an interpolated string token into a left-associative chain of string
 * concatenations: $"a{x}b" becomes "a" + x + "b". Embedded holes are parsed
 * with parse_embedded. Returns an expression of type string. */
static Expr *parse_interp(Parser *p, Token *t) {
    Span span = t->span;
    Expr *acc = NULL;
    const char *s = t->text;
    const char *seg = s;
    const char *cur = s;
    while (*cur) {
        /* An escaped brace is a literal brace, never a hole delimiter, and it is
         * two bytes here rather than one. */
        if (*cur == '\\' && cur[1] != '\0') {
            cur += 2;
            continue;
        }
        if (*cur == '{') {
            int litlen = (int)(cur - seg);
            Expr *lit = make_interp_part(p, seg, litlen, span);
            acc = acc ? make_binary(p, T_PLUS, acc, lit, span) : lit;
            /* Scan to the matching '}', stepping over escapes so a brace inside
             * a string literal in the hole does not close it early. */
            const char *e = cur + 1;
            int depth = 1;
            while (*e && depth > 0) {
                if (*e == '\\' && e[1] != '\0') {
                    e += 2;
                    continue;
                }
                if (*e == '{')
                    depth++;
                else if (*e == '}')
                    depth--;
                if (depth > 0)
                    e++;
            }
            if (depth > 0) {
                diag_error(span, "unterminated '{' in an interpolated string");
                e = cur + 1 + strlen(cur + 1);
            }
            int exprlen = (int)(e - (cur + 1));
            Expr *sub = parse_embedded(p, cur + 1, exprlen, span);
            acc = acc ? make_binary(p, T_PLUS, acc, sub, span) : sub;
            cur = (*e == '}') ? e + 1 : e;
            seg = cur;
        } else {
            cur++;
        }
    }
    int litlen = (int)(cur - seg);
    Expr *tail = make_interp_part(p, seg, litlen, span);
    acc = acc ? make_binary(p, T_PLUS, acc, tail, span) : tail;
    return acc;
}

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case T_INT: {
        advance(p);
        Expr *e = new_expr(p, E_INT, t->span);
        e->ival = t->ival;
        e->type = type_int(&p->ty);
        return parse_postfix(p, e);
    }
    case T_F64: {
        advance(p);
        Expr *e = new_expr(p, E_F64, t->span);
        e->dval = t->dval;
        e->type = type_f64(&p->ty);
        return parse_postfix(p, e);
    }
    case T_STRING: {
        advance(p);
        Expr *e = new_expr(p, E_STRING, t->span);
        e->str_id = t->str_id;
        e->type = type_string(&p->ty);
        return parse_postfix(p, e);
    }
    case T_INTERP:
        advance(p);
        return parse_postfix(p, parse_interp(p, t));
    case T_KW_MATCH: {
        Expr *m = parse_match(p, t->span);
        return parse_postfix(p, m);
    }
    case T_KW_THIS: {
        /* `this` inside a class/struct method body refers to the receiver. */
        advance(p);
        if (p->cur_this == NULL) {
            diag_error(t->span, "'this' is only valid inside a method");
            return new_expr(p, E_INT, t->span);
        }
        Expr *th = new_expr(p, E_VAR, t->span);
        th->name = arena_strdup(p->arena, "this");
        th->slot = p->cur_this->slot;
        th->type = p->cur_this->type;
        return parse_postfix(p, th);
    }
    case T_KW_NULL: {
        advance(p);
        /* Type is deliberately unknown: the checker lets an unknown flow into
         * any pointer slot, and codegen materializes it as a literal 0. */
        return new_expr(p, E_NULL, t->span);
    }
    case T_KW_TRUE:
    case T_KW_FALSE: {
        advance(p);
        Expr *e = new_expr(p, E_BOOL, t->span);
        e->ival = t->kind == T_KW_TRUE;
        e->type = type_bool(&p->ty);
        return e;
    }
    case T_IDENT: {
        char *name = t->text;
        Span span = t->span;
        /* A local holding a function pointer is called through the pointer, so
         * this is decided before the ordinary function lookup, which would
         * report the name as undefined. */
        /* lookup_var, not lookup_var_local: a function pointer held in an
         * enclosing scope -- a parameter, most often -- is just as callable. */
        Var *fplocal = lookup_var(p, name);
        /* Inside a lambda, a name that resolves to a variable belonging to an
         * enclosing function is a capture. Done here, at the single point where
         * a name becomes a variable, so no other path can produce one. */
        capture_for(p, fplocal, span);
        /* The identifier is still the current token here, so the '(' that
         * follows it is one token ahead and whatever comes after that is two. */
        /* A local holding a function pointer, a bound method or a closure is
         * called through the value. type_is_callable covers all three: a closure
         * and a function pointer of the same signature are interchangeable, and
         * a call site should not have to know which one it was handed. */
        int is_fp_call = peek(p, 1)->kind == T_LPAREN && peek(p, 2)->kind != T_KW_THIS &&
                         fplocal != NULL && type_is_callable(fplocal->type);
        advance(p);
        if (is_fp_call) {
            Var *lv = fplocal;
            {
                advance(p); /* '(' */
                Expr **args = NULL;
                int n = 0, cap = 0;
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap == 0 ? 4 : cap * 2;
                            Expr **na =
                                arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            if (args != NULL)
                                memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                if (!match(p, T_RPAREN))
                    diag_error(cur(p)->span, "expected ')' after arguments");
                Type *ft = lv->type;
                if (n != ft->nparams) {
                    diag_error(span, "'%s' expects %d argument%s but got %d", name, ft->nparams,
                               ft->nparams == 1 ? "" : "s", n);
                } else {
                    for (int i = 0; i < n; i++) {
                        if (!is_unk(args[i]->type) && !type_equals(args[i]->type, ft->ptypes[i]))
                            diag_error(args[i]->span,
                                       "argument %d of '%s' expects '%s' but got '%s'", i + 1,
                                       name, type_name(&p->ty, ft->ptypes[i]),
                                       type_name(&p->ty, args[i]->type));
                    }
                }
                Expr *callee = new_expr(p, E_VAR, span);
                callee->name = name;
                callee->slot = lv->offset;
                callee->type = lv->type;
                if (is_kind(ft, TK_MPTR) &&
                    (is_kind(ft->ret, TK_STRUCT) || is_kind(ft->ret, TK_UNION)))
                    diag_error(span, "a method pointer cannot return a struct");
                Expr *ic = new_expr(p, E_ICALL, span);
                ic->lhs = callee;
                ic->args = args;
                ic->nargs = n;
                ic->type = ft->ret;
                return parse_postfix(p, ic);
            }
        }
        if (at(p, T_LPAREN)) {
            /* A bare variant name is a union constructor: `Circle(5)`. */
            UnionDef *owner = NULL;
            VariantDef *vd = type_find_variant(&p->ty, name, &owner);
            /* `Ok` and `Err` are the variants of every Result, so the name alone
             * does not say which one is meant: type_find_variant returns whichever
             * Result was interned first, and `Ok(true)` in a function returning
             * `Result<bool,string>` would build a `Result<int,string>`. The
             * expectation disambiguates, so the variant is re-pointed at the
             * expected Result's own before anything is checked against it. */
            Type *want = NULL;
            if (vd != NULL && type_is_result(type_find_union(&p->ty, owner->name))) {
                want = p->expect_result;
                if (want == NULL || !type_is_result(want)) {
                    diag_error(span,
                               "'%s' needs to know the other type: declare the variable's "
                               "type, or use it where a 'Result<T, E>' is expected",
                               name);
                    advance(p);
                    while (at(p, T_IDENT) || at(p, T_LPAREN) || at(p, T_RPAREN))
                        advance(p);
                    match(p, T_RPAREN);
                    Expr *bad = new_expr(p, E_INT, span);
                    bad->type = NULL;
                    return parse_postfix(p, bad);
                }
                int idx = strcmp(vd->name, "Ok") == 0 ? 0 : 1;
                if (want->udef->nvariants > idx) {
                    vd = &want->udef->variants[idx];
                    owner = want->udef;
                }
            }
            if (vd != NULL) {
                advance(p); /* '(' */
                int cap = 4, n = 0;
                Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap * 2;
                            Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                if (!match(p, T_RPAREN)) {
                    diag_error(cur(p)->span, "expected ')' after variant fields");
                }
                if (n != vd->nfields) {
                    diag_error(span, "'%s' expects %d field%s but got %d", name, vd->nfields,
                               vd->nfields == 1 ? "" : "s", n);
                } else {
                    for (int i = 0; i < n; i++) {
                        if (!is_unk(args[i]->type) &&
                            !type_equals(args[i]->type, vd->fields[i].type)) {
                            diag_error(args[i]->span, "field '%s' expects '%s' but got '%s'",
                                       vd->fields[i].name, type_name(&p->ty, vd->fields[i].type),
                                       type_name(&p->ty, args[i]->type));
                        }
                    }
                }
                Expr *e = new_expr(p, E_UNIONLIT, span);
                e->variant = vd;
                e->args = args;
                e->nargs = n;
                e->type = want != NULL ? want : type_find_union(&p->ty, owner->name);
                return parse_postfix(p, e);
            }
            return parse_postfix(p, parse_call(p, name, span));
        }
        Var *v = lookup_var(p, name);
        if (v == NULL) {
            /* A field-less variant can be written bare: `Empty`. */
            UnionDef *owner0 = NULL;
            VariantDef *vd0 = type_find_variant(&p->ty, name, &owner0);
            if (vd0 != NULL && vd0->nfields == 0) {
                Expr *eu = new_expr(p, E_UNIONLIT, span);
                eu->variant = vd0;
                eu->type = type_find_union(&p->ty, owner0->name);
                return parse_postfix(p, eu);
            }
        }
        if (v == NULL) {
            /* Inside a struct method, an unqualified field name resolves
             * to `this.<name>`. */
            if (p->cur_msd != NULL && p->cur_this != NULL) {
                Field *fld = struct_find_field(p->cur_msd, name);
                if (fld != NULL) {
                    Expr *fe = new_expr(p, E_FIELD, span);
                    fe->lhs = p->cur_this;
                    fe->name = name;
                    fe->field_off = fld->offset;
                    fe->type = fld->type;
                    return parse_postfix(p, fe);
                }
                /* A get-only property behaves like a field read. */
                StructMethod *pm = struct_find_method(p->cur_msd, name);
                if (pm != NULL && pm->nparams == 0) {
                    char *mang = arena_alloc(p->arena, strlen(p->cur_msd->name) + strlen(name) + 3);
                    snprintf(mang, strlen(p->cur_msd->name) + strlen(name) + 3, "%s__%s",
                             p->cur_msd->name, name);
                    Expr *call = new_expr(p, E_CALL, span);
                    call->name = mang;
                    Expr **a = arena_alloc_array(p->arena, 1, sizeof(Expr *));
                    a[0] = p->cur_this;
                    call->args = a;
                    call->nargs = 1;
                    call->type = pm->ret;
                    return call;
                }
            }
            {
                ConstDef *c = const_find(p, name);
                if (c != NULL)
                    return parse_postfix(p, const_expr(p, c, span));
            }
            suggest_in_scope(p, SK_VALUE, "variable", name);
            diag_error_code(span, "undefined_variable", "undefined variable '%s'", name);
            Expr *e = new_expr(p, E_VAR, span);
            e->name = name;
            e->type = NULL;
            return e;
        }
        Expr *e = new_expr(p, E_VAR, span);
        e->name = name;
        e->slot = v->offset;
        e->type = v->type;
        e->agg_param = v->agg_param;
        return parse_postfix(p, e);
    }
    case T_KW_NEW: {
        return parse_postfix(p, parse_new(p));
    }
    case T_LPAREN: {
        /* A lambda: a parameter list followed by `=>`. Checked before the cast
         * form, which also starts with a type keyword, and before the plain
         * grouped-expression form, which this is a refinement of. */
        if (lambda_ahead(p, p->pos) > 0) {
            Expr *lam = parse_lambda(p, t->span);
            return parse_postfix(p, lam);
        }
        /* `(float)x` and `(int)f`: a cast, which is how a value crosses between
         * the two numeric types in the direction that is never implicit. A '('
         * followed by anything that is not a type keyword is just a grouped
         * expression, so the two are told apart by looking at the next token. */
        if (is_kind(base_type_from_token(p, peek(p, 1)->kind), TK_INT) ||
            is_kind(base_type_from_token(p, peek(p, 1)->kind), TK_F64)) {
            Span cspan = cur(p)->span;
            Type *to = base_type_from_token(p, peek(p, 1)->kind);
            advance(p); /* '(' */
            advance(p); /* the type */
            if (!match(p, T_RPAREN))
                diag_error(cur(p)->span, "expected ')' after a cast type");
            /* parse_unary, not parse_expr: a cast binds like a unary operator, so
             * `(float)-x` and `-(float)x` both mean what they look like. */
            Expr *inner = parse_unary(p);
            Expr *c = make_cvt(p, to, inner, cspan);
            if (c == inner && !is_unk(inner->type) && !type_equals(inner->type, to))
                diag_error(cspan, "cannot cast '%s' to '%s'", type_name(&p->ty, inner->type),
                           type_name(&p->ty, to));
            return parse_postfix(p, c);
        }
        advance(p);
        Expr *inner = parse_expr(p);
        if (!match(p, T_RPAREN)) {
            diag_error(cur(p)->span, "expected ')' after expression");
        }
        /* Allow postfix (method calls, indexing) on a parenthesized value. */
        return parse_postfix(p, inner);
    }
    default:
        diag_error(t->span, "expected expression but found %s", token_kind_name(t->kind));
        advance(p);
        return new_expr(p, E_INT, t->span);
    }
}

static Expr *parse_unary(Parser *p) {
    Token *t = cur(p);
    if (t->kind == T_MINUS || t->kind == T_NOT || t->kind == T_TILDE) {
        advance(p);
        Expr *operand = parse_unary(p);
        /* Fold unary minus / logical-not / bitwise-not on literals. */
        if (t->kind == T_TILDE && operand->kind == E_INT) {
            Expr *f = new_expr(p, E_INT, t->span);
            f->ival = ~operand->ival;
            f->type = type_int(&p->ty);
            return f;
        }
        if (t->kind == T_MINUS && operand->kind == E_INT) {
            Expr *f = new_expr(p, E_INT, t->span);
            f->ival = -operand->ival;
            f->type = type_int(&p->ty);
            return f;
        }
        if (t->kind == T_NOT && operand->kind == E_BOOL) {
            Expr *f = new_expr(p, E_BOOL, t->span);
            f->ival = !operand->ival;
            f->type = type_bool(&p->ty);
            return f;
        }
        Expr *e = new_expr(p, E_UNARY, t->span);
        e->op = t->kind;
        e->lhs = operand;
        if (t->kind == T_MINUS || t->kind == T_TILDE) {
            const char *what = t->kind == T_MINUS ? "negate" : "apply '~' to";
            /* Negation works on a float -- it flips the sign bit, so -0.0 stays
             * distinct from 0.0 -- but bitwise not has no meaning for one. */
            int ok = t->kind == T_MINUS
                         ? (is_kind(operand->type, TK_INT) || is_kind(operand->type, TK_F64))
                         : is_kind(operand->type, TK_INT);
            if (!is_unk(operand->type) && !ok) {
                diag_error(t->span, "cannot %s '%s'", what, type_name(&p->ty, operand->type));
                e->type = NULL;
            } else if (is_kind(operand->type, TK_F64)) {
                e->type = type_f64(&p->ty);
            } else {
                e->type = type_int(&p->ty);
            }
        } else {
            if (!is_unk(operand->type) && !is_kind(operand->type, TK_BOOL)) {
                diag_error(t->span, "cannot apply '!' to '%s'", type_name(&p->ty, operand->type));
                e->type = NULL;
            } else {
                e->type = type_bool(&p->ty);
            }
        }
        return e;
    }
    if (t->kind == T_AMP) { /* &expr */
        advance(p);
        /* `&f` where f names a function is that function's address, typed by
         * its signature. This has to be caught before the operand is parsed as
         * an expression, because a bare function name is not a variable. */
        if (at(p, T_IDENT) && peek(p, 1)->kind != T_LPAREN) {
            char *fn_src = cur(p)->text;
            Sig *fs = find_sig(p, resolve_fn_sym(p, fn_src));
            if (fs != NULL) {
                Span fspan = cur(p)->span;
                char *fname = arena_strdup(p->arena, resolve_fn_sym(p, fn_src));
                advance(p);
                Expr *fe = new_expr(p, E_FNPTR, fspan);
                fe->name = fname;
                fe->is_extern = fs->is_extern || fs->is_export;
                Type **pts = NULL;
                if (fs->nparams > 0) {
                    pts = arena_alloc_array(p->arena, (size_t)fs->nparams, sizeof(Type *));
                    for (int i = 0; i < fs->nparams; i++)
                        pts[i] = fs->ptypes[i];
                }
                fe->type = type_fnptr(&p->ty, pts, fs->nparams, fs->ret);
                return fe;
            }
        }
        int saved_take = p->take_method_addr;
        p->take_method_addr = 1;
        Expr *operand = parse_unary(p);
        p->take_method_addr = saved_take;
        /* `&obj.M` is already the pointer; taking its address again would be
         * nonsense, and it is not an lvalue. */
        if (operand->kind == E_MPTR)
            return operand;
        Expr *e = new_expr(p, E_ADDR, t->span);
        e->lhs = operand;
        if (!is_lvalue(operand)) {
            if (!is_unk(operand->type)) {
                diag_error(t->span, "cannot take address of a temporary");
            }
            e->type = NULL;
        } else {
            e->type = type_ptr(&p->ty, operand->type);
        }
        return e;
    }
    if (t->kind == T_STAR) { /* *expr */
        advance(p);
        Expr *operand = parse_unary(p);
        Expr *e = new_expr(p, E_DEREF, t->span);
        e->lhs = operand;
        if (!is_kind(operand->type, TK_PTR)) {
            if (!is_unk(operand->type)) {
                diag_error(t->span, "cannot dereference %s", type_name(&p->ty, operand->type));
            }
            e->type = NULL;
        } else {
            e->type = operand->type->base;
        }
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_binary(Parser *p, int min_prec) {
    Expr *lhs = parse_unary(p);
    for (;;) {
        TokenKind op = cur(p)->kind;
        int prec = binop_prec(op);
        if (prec == 0 || prec < min_prec)
            break;
        Span span = cur(p)->span;
        advance(p);
        Expr *rhs = parse_binary(p, prec + 1);
        lhs = make_binary(p, op, lhs, rhs, span);
    }
    return lhs;
}

static int at_assign_op(TokenKind k) {
    return k == T_ASSIGN || k == T_PLUS_EQ || k == T_MINUS_EQ || k == T_STAR_EQ ||
           k == T_SLASH_EQ || k == T_PERCENT_EQ || k == T_AMP_EQ || k == T_PIPE_EQ ||
           k == T_CARET_EQ || k == T_SHL_EQ || k == T_SHR_EQ;
}

static TokenKind base_op(TokenKind k) {
    switch (k) {
    case T_PLUS_EQ:
        return T_PLUS;
    case T_MINUS_EQ:
        return T_MINUS;
    case T_STAR_EQ:
        return T_STAR;
    case T_SLASH_EQ:
        return T_SLASH;
    case T_PERCENT_EQ:
        return T_PERCENT;
    case T_AMP_EQ:
        return T_AMP;
    case T_PIPE_EQ:
        return T_PIPE;
    case T_CARET_EQ:
        return T_CARET;
    case T_SHL_EQ:
        return T_SHL;
    case T_SHR_EQ:
        return T_SHR;
    default:
        return T_ASSIGN;
    }
}

static Expr *parse_expr(Parser *p) {
    Expr *lhs = parse_binary(p, 1);
    TokenKind k = cur(p)->kind;
    if (at_assign_op(k)) {
        Span span = cur(p)->span;
        int compound = k != T_ASSIGN;
        TokenKind bop = base_op(k);
        advance(p);
        Expr *rhs = parse_expr(p);
        if (!is_lvalue(lhs)) {
            if (!is_unk(lhs->type)) {
                diag_error(span, "left side of assignment must be a variable");
            }
            lhs = new_expr(p, E_INT, lhs->span);
            lhs->type = type_int(&p->ty);
        }
        Expr *e = new_expr(p, E_ASSIGN, span);
        e->op = bop;
        e->compound = compound;
        e->lhs = lhs;
        e->rhs = rhs;
        if (compound) {
            /* A compound assignment runs the operator and stores back into the
             * left operand, so the right side is promoted to the left's type
             * rather than the whole expression being retyped. `f += 1` is
             * therefore float arithmetic, and `i += 1.5` is an error rather
             * than a silent truncation. */
            if (is_kind(lhs->type, TK_F64) && (bop == T_PLUS || bop == T_MINUS || bop == T_STAR ||
                                               bop == T_SLASH)) {
                if (!is_kind(rhs->type, TK_F64) && !is_unk(rhs->type) && !is_kind(rhs->type, TK_INT))
                    diag_error(span, "operator %s= requires 'float' or 'int' operands",
                               token_kind_name(bop));
                else
                    rhs = make_cvt(p, type_f64(&p->ty), rhs, span);
                e->rhs = rhs;
                e->type = lhs->type;
                return e;
            }
            if (!is_kind(lhs->type, TK_INT) || !is_kind(rhs->type, TK_INT)) {
                if (!is_unk(lhs->type) && !is_unk(rhs->type)) {
                    diag_error(span, "operator %s= requires 'int' operands",
                               token_kind_name(bop));
                    e->type = NULL;
                    return e;
                }
            }
            e->type = lhs->type;
        } else {
            /* The one implicit conversion: an int widens to a float, which
             * cannot lose a value. Everything else has to match exactly -- except
             * a concrete value going to an interface, which boxes it. */
            if (is_kind(lhs->type, TK_IFACE) && !is_kind(rhs->type, TK_IFACE) &&
                !is_unk(rhs->type)) {
                rhs = to_iface(p, rhs, lhs->type, span);
                e->rhs = rhs;
                e->type = lhs->type;
                return e;
            }
            if (type_widens_to(lhs->type, rhs->type) && !is_unk(lhs->type) &&
                !is_unk(rhs->type)) {
                rhs = make_cvt(p, lhs->type, rhs, span);
                e->rhs = rhs;
                e->type = lhs->type;
                return e;
            }
            if (!type_assignable(lhs->type, rhs->type) && !is_unk(lhs->type) &&
                !is_unk(rhs->type)) {
                /* Name the cast that would work, when there is one: "cannot
                 * assign float to int" is a true sentence but not much help. */
                if (is_kind(lhs->type, TK_INT) && is_kind(rhs->type, TK_F64))
                    diag_error(span,
                               "cannot assign 'float' to 'int' without losing precision; write "
                               "'(int)' if that is what you want",
                               type_name(&p->ty, rhs->type), type_name(&p->ty, lhs->type));
                else
                    diag_error(span, "cannot assign '%s' to '%s'", type_name(&p->ty, rhs->type),
                               type_name(&p->ty, lhs->type));
                e->type = NULL;
                return e;
            }
            e->type = lhs->type;
        }
        return e;
    }
    /* Conditional (ternary): `cond ? a : b`. */
    if (at(p, T_QUESTION)) {
        Span span = cur(p)->span;
        advance(p);
        Expr *cond = lhs;
        Expr *then_e = parse_expr(p);
        if (!match(p, T_COLON)) {
            diag_error(cur(p)->span, "expected ':' in conditional expression");
        }
        Expr *else_e = parse_expr(p);
        Expr *e = new_expr(p, E_TERNARY, span);
        e->lhs = cond;
        e->rhs = then_e;
        e->args = arena_alloc_array(p->arena, 1, sizeof(Expr *));
        e->args[0] = else_e;
        e->nargs = 1;
        if (!is_unk(cond->type) && !is_kind(cond->type, TK_BOOL)) {
            diag_error(span, "condition of '?:' must be 'bool' but got '%s'",
                       type_name(&p->ty, cond->type));
            e->type = NULL;
        } else if (!type_equals(then_e->type, else_e->type)) {
            if (!is_unk(then_e->type) && !is_unk(else_e->type)) {
                diag_error(span, "branches of '?:' have different types '%s' and '%s'",
                           type_name(&p->ty, then_e->type), type_name(&p->ty, else_e->type));
                e->type = NULL;
            } else {
                e->type = then_e->type;
            }
        } else {
            e->type = then_e->type;
        }
        return e;
    }
    return lhs;
}

/* ---- statement parsing ---- */

static void expect_semi(Parser *p) {
    if (!match(p, T_SEMI)) {
        diag_error(cur(p)->span, "expected ';' but found %s", token_kind_name(cur(p)->kind));
    }
}

/* Registers a compile-time constant. */
static void const_define(Parser *p, char *name, Type *ty, long long ival, int str_id) {
    for (int i = p->nconsts - 1; i >= 0; i--) {
        if (strcmp(p->consts[i].name, name) == 0) {
            diag_error(p->toks[p->pos].span, "const '%s' is already defined", name);
            return;
        }
    }
    if (p->nconsts == p->const_cap) {
        int ncap = p->const_cap ? p->const_cap * 2 : 8;
        ConstDef *nc = arena_alloc_array(p->arena, ncap, sizeof(ConstDef));
        if (p->nconsts > 0)
            memcpy(nc, p->consts, p->nconsts * sizeof(ConstDef));
        p->consts = nc;
        p->const_cap = ncap;
    }
    p->consts[p->nconsts].name = name;
    p->consts[p->nconsts].type = ty;
    p->consts[p->nconsts].ival = ival;
    p->consts[p->nconsts].str_id = str_id;
    p->nconsts++;
}

static ConstDef *const_find(Parser *p, const char *name) {
    for (int i = p->nconsts - 1; i >= 0; i--)
        if (strcmp(p->consts[i].name, name) == 0)
            return &p->consts[i];
    return NULL;
}

/* Materialises a const reference as a literal expression, so consts cost
 * nothing at run time and can be used in constant expressions. */
static Expr *const_expr(Parser *p, ConstDef *c, Span span) {
    Expr *e = new_expr(p, is_kind(c->type, TK_STRING) ? E_STRING
                                                      : (is_kind(c->type, TK_BOOL) ? E_BOOL : E_INT),
                       span);
    e->ival = c->ival;
    e->str_id = c->str_id;
    e->type = c->type;
    return e;
}

/* Folds an expression that is built only from literals, so `const` initializers
 * can be written as arithmetic rather than forcing a pre-computed number. */
static int const_fold_static(Parser *p, Expr *e, long long *out) {
    if (e == NULL)
        return 0;
    long long a, b;
    switch (e->kind) {
    case E_INT:
    case E_BOOL:
        *out = e->ival;
        return 1;
    case E_UNARY:
        if (!const_fold_static(p, e->lhs, &a))
            return 0;
        if (e->op == T_MINUS) {
            *out = -a;
            return 1;
        }
        if (e->op == T_TILDE) {
            *out = ~a;
            return 1;
        }
        if (e->op == T_NOT) {
            *out = !a;
            return 1;
        }
        return 0;
    case E_BINARY:
        if (!const_fold_static(p, e->lhs, &a) || !const_fold_static(p, e->rhs, &b))
            return 0;
        switch (e->op) {
        case T_PLUS: *out = a + b; return 1;
        case T_MINUS: *out = a - b; return 1;
        case T_STAR: *out = a * b; return 1;
        case T_SLASH:
            if (b == 0) return 0;
            *out = a / b;
            return 1;
        case T_PERCENT:
            if (b == 0) return 0;
            *out = a % b;
            return 1;
        case T_AMP: *out = a & b; return 1;
        case T_PIPE: *out = a | b; return 1;
        case T_CARET: *out = a ^ b; return 1;
        case T_SHL:
            if (b < 0 || b > 63) return 0;
            *out = a << b;
            return 1;
        case T_SHR:
            if (b < 0 || b > 63) return 0;
            *out = a >> b;
            return 1;
        case T_EQ: *out = a == b; return 1;
        case T_NE: *out = a != b; return 1;
        case T_LT: *out = a < b; return 1;
        case T_LE: *out = a <= b; return 1;
        case T_GT: *out = a > b; return 1;
        case T_GE: *out = a >= b; return 1;
        case T_AND: *out = a && b; return 1;
        case T_OR: *out = a || b; return 1;
        default: return 0;
        }
    case E_TERNARY:
        if (!const_fold_static(p, e->lhs, &a))
            return 0;
        if (a) {
            if (!const_fold_static(p, e->rhs, &b))
                return 0;
        } else {
            if (!const_fold_static(p, e->args[0], &b))
                return 0;
        }
        *out = b;
        return 1;
    default:
        return 0;
    }
}

/* `const T NAME = <compile-time constant>;`
 *
 * The initializer must fold to a constant; anything else is rejected so a
 * const can never quietly become a runtime value. Like functions and types,
 * a const is visible from its declaration onward, so it must be declared
 * before use. */
static Stmt *parse_const_decl(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* const */
    Type *ty = parse_type(p);
    if (ty == NULL) {
        diag_error(cur(p)->span, "const needs a type");
        while (!at(p, T_SEMI) && !at(p, T_EOF))
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, start);
    }
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected const name");
        while (!at(p, T_SEMI) && !at(p, T_EOF))
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, start);
    }
    char *name = cur(p)->text;
    Span name_span = cur(p)->span;
    advance(p);
    if (!match(p, T_ASSIGN)) {
        diag_error(cur(p)->span, "const '%s' needs an initializer", name);
        while (!at(p, T_SEMI) && !at(p, T_EOF))
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, start);
    }
    Expr *init = parse_expr(p);
    expect_semi(p);

    if (!is_unk(ty) && !is_unk(init->type) && !type_equals(ty, init->type)) {
        diag_error(name_span, "const '%s' is '%s' but the initializer is '%s'", name,
                   type_name(&p->ty, ty), type_name(&p->ty, init->type));
        return new_stmt(p, S_EXPR, start);
    }
    /* Fold the initializer; only constants are allowed. */
    if (init->kind == E_INT || init->kind == E_BOOL) {
        const_define(p, name, ty, init->ival, 0);
    } else if (init->kind == E_STRING) {
        const_define(p, name, ty, 0, init->str_id);
    } else if (init->kind == E_BINARY || init->kind == E_UNARY) {
        long long v;
        if (const_fold_static(p, init, &v)) {
            const_define(p, name, ty, v, 0);
        } else {
            diag_error(name_span, "const '%s' needs a compile-time constant initializer", name);
        }
    } else {
        diag_error(name_span, "const '%s' needs a compile-time constant initializer", name);
    }
    return new_stmt(p, S_EXPR, start);
}

static Stmt *parse_var_decl(Parser *p) {
    Span start = cur(p)->span;
    Type *declared = NULL;
    int infer = 0;
    if (match(p, T_KW_VAR)) {
        infer = 1;
    } else {
        declared = parse_type(p);
    }

    if (at(p, T_IDENT)) {
        char *name = cur(p)->text;
        advance(p);
        if (!match(p, T_ASSIGN)) {
            diag_error(cur(p)->span, "declaration of '%s' needs an initializer", name);
            expect_semi(p);
            Stmt *s = new_stmt(p, S_VAR, start);
            s->name = name;
            s->type = declared;
            return s;
        }
        /* A declared type tells `Ok(...)`/`Err(...)` what to build. */
        Type *saved_expect = p->expect_result;
        p->expect_result = type_is_result(declared) ? declared : NULL;
        Expr *init = parse_expr(p);
        p->expect_result = saved_expect;
        Type *t = declared;
        if (infer)
            t = init->type;
        if (!infer && is_kind(t, TK_IFACE))
            init = to_iface(p, init, t, start);
        if (t == NULL)
            /* A bare `null` carries no type, but `var p = null` is a pointer
             * declaration in every other language; default to int* so the
             * variable is assignable later. */
            t = init->kind == E_NULL ? type_ptr(&p->ty, type_int(&p->ty)) : type_int(&p->ty);
        int slot = declare_var(p, name, t);
        expect_semi(p);
        Stmt *s = new_stmt(p, S_VAR, start);
        s->name = name;
        s->type = t;
        s->slot = slot;
        s->init = init;
        return s;
    }

    diag_error(cur(p)->span, "expected variable name after declaration");
    expect_semi(p);
    return new_stmt(p, S_EXPR, start);
}

/* True when control cannot fall out of the bottom of `s`, so anything after it
 * in the same block is dead. An `if` counts only when it has an `else` and both
 * arms leave -- otherwise the untaken arm falls through. */
static int stmt_terminates(Stmt *s) {
    if (s == NULL)
        return 0;
    switch (s->kind) {
    case S_RETURN:
    case S_BREAK:
    case S_CONTINUE:
        return 1;
    case S_BLOCK:
        /* A block leaves only if its last statement does. */
        return s->nitems > 0 && stmt_terminates(s->items[s->nitems - 1]);
    case S_IF:
        return s->orelse != NULL && stmt_terminates(s->body) && stmt_terminates(s->orelse);
    default:
        return 0;
    }
}

static Stmt *parse_block(Parser *p) {
    Span start = cur(p)->span;
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open a block");
        return new_stmt(p, S_BLOCK, start);
    }
    scope_push(p);
    int cap = 8, n = 0;
    Stmt **items = arena_alloc_array(p->arena, (size_t)cap, sizeof(Stmt *));
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        /* A statement after one that always leaves the block can never run.
         * Say so once, at the first dead statement, and keep parsing: the rest
         * still has to type-check so the errors below are the real ones. */
        if (n > 0 && stmt_terminates(items[n - 1]))
            diag_warn(W_UNREACHABLE, cur(p)->span, "this statement can never run");
        items[n++] = parse_stmt(p);
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close block");
    }
    scope_pop(p);
    Stmt *s = new_stmt(p, S_BLOCK, start);
    s->items = items;
    s->nitems = n;
    return s;
}

    /* ---- closures ----
 *
 * A lambda is not a distinct kind of function in the backend. It is hoisted to a
 * top-level function with a hidden first parameter holding the captured
 * environment, and the expression in source position becomes a pointer to a
 * { code, env } cell -- the same cell shape a bound method already uses, which is
 * why the runtime needed nothing new beyond a one-word box.
 *
 * Captured variables are moved into heap cells. The alternative, copying them
 * into the environment, gives a closure that mutates a captured variable a
 * private copy: a counter would climb inside the closure and stay put outside
 * it. With boxing, both sides reach one cell, so a captured variable means what it
 * looks like it means.
 *
 * Two lists are involved, and they are not the same list. The function-wide one
 * records every variable any lambda captured, because that is what decides which
 * frame slots hold boxes. The per-lambda one records what *this* lambda captured,
 * in the order the body first mentioned each variable, because the environment
 * is laid out in that order and is therefore dense.
 */

/* Looks for a parameter list followed by `=>` at `i`, returning the index just
 * past the `=>`, or -1. Bracket-balanced, so a call like `f((a), b)` is not
 * mistaken for a lambda. */
static int lambda_ahead(Parser *p, int i) {
    if (i >= p->ntoks || p->toks[i].kind != T_LPAREN)
        return -1;
    int depth = 0;
    for (; i < p->ntoks; i++) {
        TokenKind k = p->toks[i].kind;
        if (k == T_LPAREN) {
            depth++;
        } else if (k == T_RPAREN) {
            depth--;
            if (depth == 0)
                break;
        } else if (k == T_SEMI || k == T_LBRACE) {
            return -1; /* a declaration, not an expression */
        }
    }
    if (i + 1 < p->ntoks && p->toks[i + 1].kind == T_FATARROW)
        return i + 2;
    return -1;
}

/* Notes that the body of the current lambda reads `v`.
 *
 * Whether a reference is a capture is decided by ownership rather than by
 * scope depth: a variable is a capture exactly when it was declared by some
 * other function. That is robust against nested blocks and shadowing inside the
 * lambda, which a scope-depth test would get wrong. */
static void capture_for(Parser *p, Var *v, Span span) {
    if (!p->lam_active || v == NULL || v->owner == p->lam_owner)
        return;
    if (!v->captured) {
        if (p->ncaptured >= MAX_CAPTURES) {
            diag_error(span, "a closure cannot capture more than %d variables", MAX_CAPTURES);
            return;
        }
        v->captured = 1;
        p->captured[p->ncaptured++] = v;
    }
    /* Once per lambda. A body that mentions a variable three times still
     * captures it once -- the environment holds one entry per variable, not one
     * per mention -- and two lambdas over the same variable each get their own
     * entry, since their layouts are independent. */
    for (int i = 0; i < p->nlamcaps; i++) {
        if (p->lam_caps[i] == v)
            return;
    }
    if (p->nlamcaps >= MAX_CAPTURES)
        return;
    p->lam_caps[p->nlamcaps++] = v;
}

/* The value type of a block-bodied lambda: the type of its `return`, or `int` if
 * the block never returns a value. */
/* Gather every `return` in a lambda body. A lambda's return type is inferred
 * from what its body returns rather than written down, so unlike a declared
 * function there is nothing for the usual check to compare a `return` against
 * until the whole body has been read. The returns are collected here and checked
 * by check_lambda_returns once the type is known. */
static void collect_returns(Stmt *s, Stmt ***out, int *n, int *cap, Arena *arena) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_LEAVE:
        return; /* generated by the inliner, never inside a lambda */
    case S_RETURN:
        if (*n == *cap) {
            int ncap = *cap ? *cap * 2 : 8;
            Stmt **bigger = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
            /* Copy before growing the count. Dropping the old entries would make
             * a body with more than eight returns silently check only some. */
            if (*n > 0)
                memcpy(bigger, *out, (size_t)*n * sizeof(Stmt *));
            *out = bigger;
            *cap = ncap;
        }
        (*out)[(*n)++] = s;
        break;
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            collect_returns(s->items[i], out, n, cap, arena);
        break;
    case S_IF:
    case S_WHILE:
        collect_returns(s->body, out, n, cap, arena);
        collect_returns(s->orelse, out, n, cap, arena);
        break;
    case S_FOR:
        collect_returns(s->for_init, out, n, cap, arena);
        collect_returns(s->body, out, n, cap, arena);
        break;
    case S_VAR:
    case S_EXPR:
    case S_BREAK:
    case S_CONTINUE:
    case S_FUNC:
    case S_STRUCT:
    case S_UNION:
        break;
    }
}

/* Unify the types of a lambda body's returns into the single type the lambda
 * returns. A body that returns two different types cannot be called through one
 * signature, so the first is taken as the lambda's type and the disagreement is
 * reported rather than dropped. `*agrees` says whether that happened: when the
 * body disagrees with itself, the later per-return check has nothing useful to
 * add, and reporting every return again would bury the one real mistake. */
static Type *lambda_block_ret(Parser *p, Stmt **returns, int nret, int *agrees) {
    Type *ret = NULL;
    *agrees = 1;
    for (int i = 0; i < nret; i++) {
        Type *rt = returns[i]->expr != NULL ? returns[i]->expr->type : NULL;
        if (rt == NULL || is_unk(rt))
            continue;
        if (ret == NULL) {
            ret = rt;
        } else if (!type_equals(ret, rt)) {
            diag_error(returns[i]->span, "lambda returns '%s' here but '%s' elsewhere",
                       type_name(&p->ty, rt), type_name(&p->ty, ret));
            *agrees = 0;
        }
    }
    /* A body with no value-returning path has no type to infer, and int is the
     * long-standing fallback rather than a claim about the program. */
    return ret != NULL ? ret : type_int(&p->ty);
}

/* Now that the lambda's return type is known, hold each return in its body to
 * it. The wording matches the diagnostic a declared function produces, because
 * the mistake -- returning a type the signature does not promise -- is the same
 * one, and a lambda nested in a function that returns a different type used to
 * report the *enclosing* function's return type here. */
static void check_lambda_returns(Parser *p, Stmt **returns, int nret, Type *ret) {
    for (int i = 0; i < nret; i++) {
        Type *rt = returns[i]->expr != NULL ? returns[i]->expr->type : NULL;
        if (rt == NULL || is_unk(rt) || is_unk(ret))
            continue;
        if (!type_equals(rt, ret))
            diag_error(returns[i]->span, "cannot return '%s' from function returning '%s'",
                       type_name(&p->ty, rt), type_name(&p->ty, ret));
    }
}

static void rewrite_captures_expr(Parser *p, Expr *e, Var **caps, int ncaps, Type *env_ty,
                                     int env_slot);

/* Rewrites every reference to a captured variable in a freshly parsed lambda body
 * into `*env[i]`, with `i` the variable's position in *this* lambda's capture
 * list. The frame slot the name used to refer to holds a box pointer, and the
 * value now lives behind it, so the reference becomes a double indirection --
 * and an index and a dereference are both things the language already has. */
static void rewrite_captures_expr(Parser *p, Expr *e, Var **caps, int ncaps, Type *env_ty,
                                     int env_slot) {
    if (e == NULL)
        return;
    if (e->kind == E_VAR) {
        for (int i = 0; i < ncaps; i++) {
            if (caps[i]->offset != e->slot)
                continue;
            if (!type_equals(caps[i]->type, e->type))
                continue;
            Type *vt = e->type;
            char *nm = e->name;
            Span sp = e->span;
            Expr *envref = new_expr(p, E_VAR, sp);
            envref->name = "$env";
            envref->type = env_ty;
            envref->slot = env_slot;
            Expr *idx = new_expr(p, E_INT, sp);
            idx->ival = i;
            idx->type = type_int(&p->ty);
            Expr *elem = new_expr(p, E_INDEX, sp);
            elem->lhs = envref;
            elem->rhs = idx;
            elem->type = type_ptr(&p->ty, type_int(&p->ty));
            Expr *deref = new_expr(p, E_DEREF, sp);
            deref->lhs = elem;
            deref->type = vt;
            deref->name = nm; /* kept so a diagnostic can still name the variable */
            e->kind = E_DEREF;
            e->lhs = elem;
            e->type = vt;
            e->slot = 0;
            e->name = nm;
            return;
        }
    }
    rewrite_captures_expr(p, e->lhs, caps, ncaps, env_ty, env_slot);
    rewrite_captures_expr(p, e->rhs, caps, ncaps, env_ty, env_slot);
    rewrite_captures_expr(p, e->env, caps, ncaps, env_ty, env_slot);
    for (int i = 0; i < e->nargs; i++)
        rewrite_captures_expr(p, e->args[i], caps, ncaps, env_ty, env_slot);
    for (int i = 0; i < e->narms; i++)
        rewrite_captures_expr(p, e->arms[i].body, caps, ncaps, env_ty, env_slot);
}

static void rewrite_captures_stmt(Parser *p, Stmt *s, Var **caps, int ncaps, Type *env_ty,
                                      int env_slot) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            rewrite_captures_stmt(p, s->items[i], caps, ncaps, env_ty, env_slot);
        return;
    case S_FOR:
        rewrite_captures_stmt(p, s->for_init, caps, ncaps, env_ty, env_slot);
        rewrite_captures_stmt(p, s->body, caps, ncaps, env_ty, env_slot);
        return;
    case S_IF:
    case S_WHILE:
        rewrite_captures_stmt(p, s->body, caps, ncaps, env_ty, env_slot);
        rewrite_captures_stmt(p, s->orelse, caps, ncaps, env_ty, env_slot);
        return;
    case S_EXPR:
    case S_RETURN:
        rewrite_captures_expr(p, s->expr, caps, ncaps, env_ty, env_slot);
        return;
    case S_VAR:
        rewrite_captures_expr(p, s->init, caps, ncaps, env_ty, env_slot);
        return;
    default:
        return;
    }
}

/* Parses `(params) => body`.
 *
 * Only one level of nesting is supported. A lambda inside a lambda would have to
 * forward its own captures through the frame in between, which is a different
 * mechanism from the one here, so it is refused rather than half-implemented. */
static Expr *parse_lambda(Parser *p, Span span) {
    if (p->lam_active) {
        diag_error(span, "a lambda cannot be defined inside another lambda");
        advance(p); /* the '(' */
        while (!at(p, T_FATARROW) && !at(p, T_EOF) && !at(p, T_SEMI) && !at(p, T_LBRACE))
            advance(p);
        match(p, T_FATARROW);
        if (at(p, T_LBRACE))
            parse_block(p);
        else
            parse_expr(p);
        Expr *e = new_expr(p, E_INT, span);
        e->type = type_int(&p->ty);
        return e;
    }

    /* State the lambda's own parsing must not leak into the enclosing function:
     * its return type, its frame allocator, and its `this`. */
    Type *saved_ret = p->cur_ret;
    int saved_offset = p->next_offset;
    StructDef *saved_msd = p->cur_msd;
    Expr *saved_this = p->cur_this;
    int saved_take = p->take_method_addr;
    int saved_nlamcaps = p->nlamcaps;
    int lam_id = p->lambda_counter++;

    /* The environment is the hidden first parameter, typed `int*` purely as an
     * opaque carrier: the body only ever indexes it. */
    Type *env_ty = type_ptr(&p->ty, type_int(&p->ty));
    int lam_owner = ++p->func_counter;
    int saved_owner = p->cur_owner;
    p->cur_owner = lam_owner;
    p->lam_owner = lam_owner;
    p->lam_active = 1;
    p->nlamcaps = 0;
    /* The hoisted function has a frame of its own, so its locals start from zero
     * rather than continuing the enclosing function's allocation. Without this a
     * lambda parameter would land on top of a local in the frame that encloses
     * the lambda. */
    p->next_offset = 0;

    Type *ptypes[Z_MAX_ARGS];
    const char *pnames[Z_MAX_ARGS];
    int poffset[Z_MAX_ARGS];
    int np = 0;

    advance(p); /* '(' */
    scope_push(p);
    /* The environment is declared first, before the body exists, because the
     * body is rewritten to index it and so needs to know which frame slot it
     * landed in. Frame order is irrelevant; only the recorded offsets matter. */
    int env_slot = declare_var(p, "$env", env_ty);
    mark_synth(p, "$env");
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (np + 1 >= Z_MAX_ARGS) {
                diag_error(span, "a lambda may take at most %d parameters", Z_MAX_ARGS - 1);
                break;
            }
            Type *pt = parse_type(p);
            if (pt == NULL || !at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected a parameter type and name in a lambda");
                break;
            }
            char *pn = cur(p)->text;
            advance(p);
            poffset[np] = declare_var(p, pn, pt);
            pnames[np] = pn;
            ptypes[np++] = pt;
            if (!match(p, T_COMMA))
                break;
        }
    }
    match(p, T_RPAREN);
    match(p, T_FATARROW);

    /* A diagnostic inside the body points at a span in the lambda, which is
     * text the reader wrote but cannot see in the shape of the expression that
     * contains it. Naming where the lambda starts is what makes the span
     * findable. */
    diag_push_ctx(DIAG_CTX_LAMBDA, "lambda", span);

    /* The body: a single expression, or a block the user wrote themselves. */
    Stmt *body_block;
    Type *ret;
    Stmt **returns = NULL;
    int nreturns = 0, retcap = 0;
    if (at(p, T_LBRACE)) {
        /* Nothing to check a `return` against yet: the lambda's type comes out
         * of its body. Clearing cur_ret keeps the S_RETURN check from holding
         * each return to the *enclosing* function's return type, which for
         * `() => { n = n + 1; return n; }` inside a function returning something
         * else was the whole diagnostic. */
        p->cur_ret = NULL;
        body_block = parse_block(p);
        collect_returns(body_block, &returns, &nreturns, &retcap, p->arena);
        int agrees = 1;
        ret = lambda_block_ret(p, returns, nreturns, &agrees);
        if (agrees)
            check_lambda_returns(p, returns, nreturns, ret);
    } else {
        Expr *value = parse_expr(p);
        ret = is_unk(value->type) ? type_int(&p->ty) : value->type;
        Stmt *ret_st = new_stmt(p, S_RETURN, span);
        ret_st->expr = value;
        Stmt **items = arena_alloc_array(p->arena, 1, sizeof(Stmt *));
        items[0] = ret_st;
        body_block = new_stmt(p, S_BLOCK, span);
        body_block->items = items;
        body_block->nitems = 1;
    }

    /* Whatever the body reached for outside becomes this lambda's environment,
     * in the order the body first mentioned it. */
    Var *caps[MAX_CAPTURES];
    int ncaps = p->nlamcaps;
    for (int i = 0; i < ncaps; i++)
        caps[i] = p->lam_caps[i];
    if (ncaps > 0)
        rewrite_captures_stmt(p, body_block, caps, ncaps, env_ty, env_slot);

    /* The hoisted function. `$lam` cannot be written in Z source, so the name
     * cannot collide with anything the user declared. */
    char nm[32];
    snprintf(nm, sizeof nm, "$lam%d", lam_id);
    Stmt *fn = new_stmt(p, S_FUNC, span);
    fn->fname = arena_strdup(p->arena, nm);
    fn->src_fname = NULL; /* a lambda is anonymous: `$lam0` is not a source name */
    fn->ret_type = ret;
    fn->fbody = body_block;
    fn->vis_start = 1; /* the hidden environment is not a parameter a reader wrote */

    /* The environment is parameter 0, and the declared parameters follow it. */
    Expr **params = arena_alloc_array(p->arena, (size_t)(np + 1), sizeof(Expr *));
    for (int i = 0; i < np; i++) {
        Expr *pe = new_expr(p, E_VAR, span);
        pe->name = arena_strdup(p->arena, pnames[i]);
        pe->type = ptypes[i];
        pe->slot = poffset[i];
        params[i + 1] = pe;
    }
    Expr *envp = new_expr(p, E_VAR, span);
    envp->name = "$env";
    envp->type = env_ty;
    envp->slot = env_slot;
    params[0] = envp;
    fn->params = params;
    fn->nparams = np + 1;
    fn->locals_bytes = p->next_offset;
    add_pending(p, fn);

    scope_pop(p);
    p->cur_owner = saved_owner;
    p->lam_active = 0;
    p->nlamcaps = saved_nlamcaps;
    p->cur_ret = saved_ret;
    p->next_offset = saved_offset;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
    p->take_method_addr = saved_take;

    /* The closure value: a { code, env } cell. Each argument is a captured
     * variable of the *enclosing* frame, read as the box pointer it holds. */
    Expr *cl = new_expr(p, E_CLOSURE, span);
    cl->name = arena_strdup(p->arena, nm);
    if (ncaps > 0) {
        Expr **elems = arena_alloc_array(p->arena, (size_t)ncaps, sizeof(Expr *));
        for (int i = 0; i < ncaps; i++) {
            Expr *bp = new_expr(p, E_VAR, span);
            bp->name = caps[i]->name;
            bp->type = type_ptr(&p->ty, type_int(&p->ty));
            bp->slot = caps[i]->offset;
            bp->boxed = 1;
            elems[i] = bp;
        }
        cl->args = elems;
        cl->nargs = ncaps;
    }
    /* The parameter types are copied into the arena. A Type keeps the *pointer*
     * it is given rather than the contents, and `ptypes` is a stack array that
     * dies the moment this function returns -- so a closure whose type pointed
     * straight at it would read whatever the next call left behind. The first
     * lambda in a file usually still works and the second does not, which is a
     * particularly confusing way for this to fail. */
    Type **stored = arena_alloc_array(p->arena, (size_t)(np > 0 ? np : 1), sizeof(Type *));
    for (int i = 0; i < np; i++)
        stored[i] = ptypes[i];
    cl->type = type_closure(&p->ty, stored, np, ret);
    diag_pop_ctx();
    return cl;
}




static Stmt *parse_func(Parser *p, int nested) {
    int fn_tok_start = p->pos;
    Span start = cur(p)->span;
    /* Each function owns an id, so a lambda inside it can tell its own locals
     * from the ones it captured. */
    int fn_owner = ++p->func_counter;
    int saved_owner = p->cur_owner;
    p->cur_owner = fn_owner;

    /* A function starts a fresh capture list, and the enclosing one is put back
     * on the way out. A top-level program is the enclosing context for a
     * function declared after some top-level statements, and a plain
     * `p->ncaptured = 0` on the way out discarded the closures that had already
     * captured those statements -- so their declarations were never boxed, and
     * the first call that allocated anything walked a frame slot nobody filled.
     * The list is a fixed-size array, so copying it is cheap. */
    Var saved_captured[MAX_CAPTURES];
    int saved_ncaptured = p->ncaptured;
    if (saved_ncaptured > 0)
        memcpy(saved_captured, p->captured, (size_t)saved_ncaptured * sizeof(Var));
    p->ncaptured = 0;

    /* Look ahead for a generic parameter list `name<T,...>(` and pre-bind the
     * type parameters so the return type and parameter types can reference them
     * even though the list is written after the name. */
    int ntparams = 0;
    char **tparams = NULL;
    int saved_ntbind = p->ntbind;

    /* An `extern` declaration names a symbol implemented in C. The body is
     * optional (and forbidden for extern): the declaration exists so calls
     * type-check and so codegen emits the symbol unmangled. */
    int is_extern = 0;
    if (at(p, T_KW_EXTERN)) {
        is_extern = 1;
        advance(p);
    }
    /* `export` is the mirror of `extern`: the body is here, but the symbol
     * keeps the name as written and is made .globl so C can call it. */
    int is_export = 0;
    if (at(p, T_KW_EXPORT)) {
        is_export = 1;
        advance(p);
    }
    {
        int j = skip_type_tokens(p, p->pos);
        if (j < p->ntoks && p->toks[j].kind == T_IDENT) {
            int k = j + 1;
            if (k < p->ntoks && p->toks[k].kind == T_LT && k + 1 < p->ntoks &&
                p->toks[k + 1].kind == T_IDENT) {
                int e = skip_generic_params(p, k);
                int cap = 4;
                tparams = arena_alloc_array(p->arena, (size_t)cap, sizeof(char *));
                int m = k + 1;
                while (m < e) {
                    if (p->toks[m].kind == T_IDENT) {
                        if (ntparams == cap) {
                            cap *= 2;
                            char **g = arena_alloc_array(p->arena, (size_t)cap, sizeof(char *));
                            memcpy(g, tparams, (size_t)ntparams * sizeof(char *));
                            tparams = g;
                        }
                        tparams[ntparams] = p->toks[m].text;
                        if (!is_bound(p, p->toks[m].text))
                            bind_type(p, p->toks[m].text, type_new_param(&p->ty, p->toks[m].text));
                        ntparams++;
                    }
                    m++;
                }
            }
        }
    }

    Type *ret = parse_type(p);
    if (ret == NULL) {
        if (cur(p)->kind == T_IDENT)
            suggest_in_scope(p, SK_TYPE, "type", cur(p)->text);
        diag_error(cur(p)->span, "unknown type '%s'",
                   cur(p)->kind == T_IDENT ? cur(p)->text : token_kind_name(cur(p)->kind));
    }
    char *name = cur(p)->text;
    advance(p); /* name */

    /* Consume the generic parameter list if present (already bound above). */
    if (ntparams > 0 && at(p, T_LT))
        p->pos = skip_generic_params(p, p->pos);
    match(p, T_LPAREN);

    /* A function written inside another one is emitted under a symbol built from
     * the enclosing function's id, so two `helper` declarations in two different
     * bodies do not collide. Everything the reader sees -- diagnostics, the
     * mapping from the written name -- keeps using the name as written. */
    char *src_name = name;
    char symbuf[64];
    if (nested) {
        snprintf(symbuf, sizeof symbuf, "$fn%d_%s", saved_owner, src_name);
        name = arena_strdup(p->arena, symbuf);
    }
    /* `extern` and `export` name a symbol deliberately, and a local has no
     * outside to be visible to. */
    if (nested && (is_extern || is_export))
        diag_error(start, "a function declared inside another function cannot be '%s'",
                   is_extern ? "extern" : "export");
    if (nested && ntparams > 0)
        diag_error(start, "'%s' cannot be a generic function here", src_name);

    Scope *saved_scope = p->scope;
    int saved_slot = p->next_offset;
    Type *saved_ret = p->cur_ret;
    LocalFn *saved_local_fns = p->local_fns;
    scope_push(p);
    p->scope->is_fn_body = 1;
    p->next_offset = 0;
    p->cur_ret = ret;

    int pcap = 4, pn = 0;
    Expr **params = arena_alloc_array(p->arena, (size_t)pcap, sizeof(Expr *));
    int is_ext = 0;
    if (at(p, T_KW_THIS)) {
        is_ext = 1; /* extension method: the first param is the receiver */
        advance(p);
    }
    if (!at(p, T_RPAREN)) {
        for (;;) {
            Type *pt = parse_type(p);
            if (pt == NULL) {
                diag_error(cur(p)->span, "expected parameter type but found %s",
                           token_kind_name(cur(p)->kind));
                break;
            }
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected parameter name");
                break;
            }
            char *pname = cur(p)->text;
            Span pspan = cur(p)->span;
            advance(p);
            if (pn == pcap) {
                int ncap = pcap * 2;
                Expr **npar = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(npar, params, (size_t)pn * sizeof(Expr *));
                params = npar;
                pcap = ncap;
            }
            Expr *pe = new_expr(p, E_VAR, pspan);
            pe->name = pname;
            pe->type = pt;
            pe->slot = declare_var(p, pname, pt);
            if (is_kind(pt, TK_STRUCT) || is_kind(pt, TK_UNION)) {
                pe->agg_param = 1;
                Var *pv = lookup_var_local(p, pname);
                if (pv)
                    pv->agg_param = 1;
            }
            params[pn++] = pe;
            if (!match(p, T_COMMA))
                break;
        }
    }
    match(p, T_RPAREN);

    /* A struct-returning function uses the SysV hidden-pointer convention: a
     * hidden first parameter holds the address of the caller's result buffer. */
    int ret_is_aggregate = is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION);
    /* The hidden result pointer is a parameter as far as the frame is concerned,
     * and the argument-setup arrays hold one entry per parameter. Accepting 16
     * written parameters here would make the 17th spill out of them, so a
     * struct-returning function is one shorter. */
    int max_params = ret_is_aggregate ? Z_MAX_ARGS - 1 : Z_MAX_ARGS;
    if (pn > max_params) {
        diag_error(start, "'%s' takes %d parameters but the limit is %d%s", src_name, pn,
                   max_params,
                   ret_is_aggregate ? " (a struct-returning function spends one on the result"
                                          " buffer)"
                                    : "");
    }
    if (ret_is_aggregate) {
        Expr **np = arena_alloc_array(p->arena, (size_t)(pn + 1), sizeof(Expr *));
        memcpy(np + 1, params, (size_t)pn * sizeof(Expr *));
        Expr *hidden = new_expr(p, E_VAR, start);
        hidden->name = arena_strdup(p->arena, "$ret");
        hidden->type = type_ptr(&p->ty, ret);
        hidden->slot = declare_var(p, "$ret", hidden->type);
        /* The hidden result buffer is written by the return sequence, not named
         * in source, so it is never "used" by name. */
        mark_synth(p, "$ret");
        np[0] = hidden;
        params = np;
        pn++;
    }

    /* A nested function gets no pre-scan -- that only walks top-level tokens --
     * so its signature is filed here, under the mangled symbol, before the body
     * is read. Doing it first is what lets the body call itself. */
    if (nested) {
        int ivis = ret_is_aggregate ? 1 : 0;
        int invis = pn - ivis;
        Type **cpt = arena_alloc_array(p->arena, (size_t)(invis > 0 ? invis : 1), sizeof(Type *));
        for (int i = 0; i < invis; i++)
            cpt[i] = params[ivis + i]->type;
        add_sig(p, name, ret, cpt, invis, is_ext);
        /* Pushed onto the list the enclosing function is using, not a fresh one:
         * the body has to resolve its own name, and a fresh list would discard
         * the mapping on the way out of parse_func. The mapping outlives this
         * parse_func call so calls that follow the declaration still find it. */
        LocalFn *lf = arena_alloc(p->arena, sizeof *lf);
        lf->name = src_name;
        lf->sym = name;
        lf->next = p->local_fns;
        p->local_fns = lf;
    }

    Stmt *fn = new_stmt(p, S_FUNC, start);
    fn->fname = name;
    fn->src_fname = src_name;
    fn->ret_type = ret;
    fn->params = params;
    fn->nparams = pn;
    /* Anything diagnosed from here on is inside this function, and says so. A
     * body-less declaration raises nothing, so the frame is pushed after the
     * declaration-only case and popped on every way out. */
    diag_push_ctx(DIAG_CTX_FUNC, src_name, start);
    /* A struct-returning function's parameter 0 is the hidden result buffer, so
     * the first one a reader can match to the source is 1. */
    fn->vis_start = ret_is_aggregate ? 1 : 0;
    fn->is_ext = is_ext;
    fn->is_extern = is_extern;
    fn->is_export = is_export;
    if (is_export && at(p, T_SEMI))
        diag_error(start, "'export' function '%s' needs a body; use 'extern' to declare a C "
                          "function",
                   src_name);
    if (at(p, T_SEMI) || is_extern) {
        /* Declaration only. Nothing is emitted; the pre-scan already recorded
         * the signature. */
        if (nested && !is_extern)
            diag_error(start, "a function declared inside another function needs a body; "
                              "a body-less declaration only makes sense at the top level");
        if (is_extern && at(p, T_LBRACE))
            diag_error(start, "'extern' function '%s' cannot have a body", src_name);
        else
            expect_semi(p);
        /* There is no body, so nothing in this scope can ever be referenced.
         * Claiming the parameters are unused would be reporting on C code we
         * have not seen. */
        for (Var *v = p->scope->vars; v != NULL; v = v->next)
            v->is_synth = 1;
        fn->fbody = NULL;
        fn->is_extern = is_extern;
        fn->locals_bytes = 0;
        p->ntbind = saved_ntbind;
        scope_pop(p);
        diag_pop_ctx();
        return fn;
    }
    if (match(p, T_FATARROW)) {
        /* Expression-bodied function: `int f(int a) => expr;` desugars to
         * `{ return expr; }`. */
        Expr *e = parse_expr(p);
        if (!is_unk(e->type) && !is_unk(ret) && !type_equals(e->type, ret)) {
            diag_error(start, "cannot return '%s' from function returning '%s'",
                       type_name(&p->ty, e->type), type_name(&p->ty, ret));
        }
        Stmt *ret_st = new_stmt(p, S_RETURN, start);
        ret_st->expr = e;
        Stmt **items = arena_alloc_array(p->arena, 1, sizeof(Stmt *));
        items[0] = ret_st;
        Stmt *body = new_stmt(p, S_BLOCK, start);
        body->items = items;
        body->nitems = 1;
        fn->fbody = body;
        expect_semi(p);
    } else {
        fn->fbody = parse_block(p);
    }
    fn->locals_bytes = p->next_offset;

    if (ntparams > 0 && !p->in_instantiate) {
        /* Record the template for later monomorphization. The placeholder-typed
         * function itself is validated here but not emitted; concrete
         * instantiations are re-parsed and emitted instead. During an
         * instantiation re-parse we must NOT re-register (that would grow the
         * template array and invalidate caller's `g` pointer). */
        int fn_tok_end = p->pos;
        Generic g;
        memset(&g, 0, sizeof g);
        g.name = name;
        g.ntparams = ntparams;
        g.tparams = tparams;
        g.tok_start = fn_tok_start;
        g.tok_end = fn_tok_end;
        g.is_ext = is_ext;
        g.nparams = fn->nparams;
        g.vis_start = (is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION)) ? 1 : 0;
        g.nvisible = fn->nparams - g.vis_start;
        g.ret_type = ret;
        Type **pt = arena_alloc_array(p->arena, (size_t)fn->nparams, sizeof(Type *));
        for (int i = 0; i < fn->nparams; i++)
            pt[i] = fn->params[i]->type;
        g.ptypes = pt;
        add_generic(p, g);
        fn->is_generic_template = 1;
    }

    p->ntbind = saved_ntbind;
    scope_pop(p);
    p->scope = saved_scope;
    p->next_offset = saved_slot;
    p->cur_ret = saved_ret;
    /* The mapping a nested function pushed belongs to the *enclosing* function:
     * its scope runs to the end of that function, so calls after the declaration
     * still find it. Only leaving the scope that owns it clears the list, and
     * that is this function's own `saved_local_fns` when the function being left
     * is a top-level one. A nested function's exit must not clear it. */
    if (!nested)
        p->local_fns = saved_local_fns;

    /* Every variable a lambda in this body captured is now known, so the slots
     * that hold boxes can be marked before codegen ever sees the tree. Done
     * after the body is complete and the scope is gone, so the marks are the
     * final word on which slots are indirect. */
    /* A function that returns a lambda has a closure, not a bare code address,
     * and the two are called differently: a closure carries an environment in the
     * second word of its cell and the call site has to pass it. The static type
     * has to say so, or a caller cannot generate the right call.
     *
     * A `fn` and a `closure` are different types and are called differently, so
     * a function that hands one back has to say which it is. Declaring `fn` and
     * returning a lambda is an error naming the right spelling. */
    if (is_kind(fn->ret_type, TK_FNPTR) && body_returns_closure(fn->fbody)) {
        diag_error(fn->span,
                   "'%s' returns a lambda, so its return type must be "
                   "'closure(...) -> ...' rather than 'fn(...) -> ...'",
                   fn->fname);
    }

    if (p->ncaptured > 0) {
        mark_boxed_stmt(p, fn->fbody);
        /* A captured parameter is a capture like any other: its slot holds a box,
         * so the prologue has to box the incoming value rather than store it
         * straight into the frame. mark_boxed_stmt does not see parameters --
         * they are not in the body -- so they are marked here. */
        for (int i = fn->vis_start; i < fn->nparams; i++) {
            for (int k = 0; k < p->ncaptured; k++) {
                if (p->captured[k]->offset == fn->params[i]->slot) {
                    fn->params[i]->boxed = 1;
                    break;
                }
            }
        }
    }
    /* Closed-form loops, once the body is complete: the pass reads variables, so
     * it cannot run before the whole function has been parsed. */
    if (fn->fbody != NULL)
        close_form_stmts(p, &fn->fbody, 1);

    if (saved_ncaptured > 0)
        memcpy(p->captured, saved_captured, (size_t)saved_ncaptured * sizeof(Var));
    p->ncaptured = saved_ncaptured;
    p->cur_owner = saved_owner;
    diag_pop_ctx();

    if (strcmp(name, "main") == 0)
        fn->is_entry = 1;

    /* A nested function is not emitted where it was written: it is queued with
     * the other hoisted functions and emitted alongside them, under its mangled
     * symbol. Its declaration site produces no code of its own. */
    if (nested)
        add_pending(p, fn);
    return fn;
}

/* interface Name { ret m(params); ... }
 *
 * A declaration only: the body is semicolon-terminated signatures, and which
 * types satisfy it is decided per use rather than declared. That is what lets a
 * struct implement an interface without naming it -- a struct has no vtable of
 * its own, so a declared `struct S : I` would have nowhere to put the methods.
 *
 * The order of the methods is the contract. A call resolves a name to an index
 * here, once, and every implementing type lays its itab out in this same order.
 */
static Stmt *parse_interface_decl(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* 'interface' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected interface name after 'interface'");
        return new_stmt(p, S_EXPR, start);
    }
    char *name = cur(p)->text;
    advance(p);
    IfaceDef *id = type_define_iface(&p->ty, name);
    /* A pre-scan already built the method list, so that a signature naming this
     * interface -- and a call resolving one of its methods -- could work above the
     * declaration. The members are about to be read again, so start over rather
     * than append. */
    if (id != NULL && id->prescanned) {
        id->prescanned = 0;
        id->methods = NULL;
        id->nmethods = 0;
    }
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open interface body");
        return new_stmt(p, S_EXPR, start);
    }
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        Type *ret = parse_type(p);
        if (ret == NULL) {
            diag_error(cur(p)->span, "expected a return type in interface body");
            break;
        }
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected a method name in interface body");
            break;
        }
        char *mname = cur(p)->text;
        advance(p);
        if (!match(p, T_LPAREN)) {
            diag_error(cur(p)->span, "expected '(' after method name");
            break;
        }
        Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
        int np = 0;
        if (!at(p, T_RPAREN)) {
            for (;;) {
                if (np >= Z_MAX_ARGS) {
                    diag_error(cur(p)->span, "'%s' takes too many parameters", mname);
                    break;
                }
                Type *pt = parse_type(p);
                if (pt == NULL || !at(p, T_IDENT)) {
                    diag_error(cur(p)->span, "expected a parameter type and name");
                    break;
                }
                advance(p); /* parameter name; an interface has no body to read */
                ptypes[np++] = pt;
                if (!match(p, T_COMMA))
                    break;
            }
        }
        match(p, T_RPAREN);
        if (iface_find_method(id, mname) != NULL) {
            diag_error(start, "interface '%s' already requires '%s'", name, mname);
        } else {
            iface_add_method(&p->ty, id, mname, type_fnptr(&p->ty, ptypes, np, ret));
        }
        if (!match(p, T_SEMI))
            break;
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close interface body");
    }
    return new_stmt(p, S_BLOCK, start);
}

/* foreach (T x in coll) body  ->  a hidden index-based while loop. */
static Stmt *parse_foreach(Parser *p, Span start) {
    advance(p); /* foreach */
    match(p, T_LPAREN);
    Type *et = NULL;
    int infer = 0;
    if (match(p, T_KW_VAR)) {
        infer = 1;
    } else {
        et = parse_type(p);
        /* `foreach (v in xs)` with a bare identifier: parse_type found no type
         * (it did not consume `v`), so treat the variable as inferred rather
         * than leaving a NULL element type. */
        if (et == NULL)
            infer = 1;
    }
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected loop variable name");
        return new_stmt(p, S_BLOCK, start);
    }
    char *name = cur(p)->text;
    advance(p);
    if (!match(p, T_KW_IN)) {
        diag_error(cur(p)->span, "expected 'in' in foreach");
    }
    Expr *coll = parse_expr(p);
    match(p, T_RPAREN);

    /* Only an array. A pointer has no length -- there is nothing for the
     * loop's bound to compare against -- and reading `ptr.length` off one
     * yields the pointer's own address, which used to turn a `foreach` over a
     * `T*` into an unbounded walk into unrelated memory. To walk part of an
     * array, use an explicit index loop. */
    if (!is_kind(coll->type, TK_ARRAY)) {
        if (!is_unk(coll->type)) {
            if (is_kind(coll->type, TK_PTR))
                diag_error(start,
                           "cannot foreach over '%s': a pointer has no length, so there is no "
                           "bound to iterate to; use an index loop over the array instead",
                           type_name(&p->ty, coll->type));
            else
                diag_error(start, "foreach requires an array, got %s",
                           type_name(&p->ty, coll->type));
        }
        /* The body is skipped rather than parsed: nothing in it can be
         * meaningful once the collection is rejected, and parsing it would bury
         * the one real diagnostic under a pile of undefined-name errors. */
        match(p, T_SEMI);
        skip_braced_block(p);
        return new_stmt(p, S_BLOCK, start);
    }

    /* Loop variables live in their own scope. */
    scope_push(p);
    char hname[32];
    int loop_id = p->foreach_counter++;
    /* __arr/__fi are this desugaring's own bookkeeping and are never named in
     * source, so they are exempt from -Wunused-local. */
    snprintf(hname, sizeof hname, "__arr%d", loop_id);
    int arr_slot = declare_var(p, hname, coll->type);
    mark_synth(p, hname);
    snprintf(hname, sizeof hname, "__fi%d", loop_id);
    int idx_slot = declare_var(p, hname, type_int(&p->ty));
    mark_synth(p, hname);
    int elem_slot = declare_var(p, name, infer ? coll->type->base : et);

    /* Build the desugared loop by hand (normal parser helpers would redeclare
     * names in the wrong scope). The loop variables stay in scope while the
     * body is parsed so the element name resolves. */
    Type *int_ty = type_int(&p->ty);

    Expr *arr_ref = new_expr(p, E_VAR, start);
    arr_ref->name = "coll";
    arr_ref->slot = arr_slot;
    arr_ref->type = coll->type;

    Expr *idx_ref = new_expr(p, E_VAR, start);
    idx_ref->name = "idx";
    idx_ref->slot = idx_slot;
    idx_ref->type = int_ty;

    /* coll = <collection> */
    Stmt *arr_init = new_stmt(p, S_VAR, start);
    arr_init->name = "coll";
    arr_init->type = coll->type;
    arr_init->slot = arr_slot;
    arr_init->init = coll;

    /* init index = 0 */
    Expr *zero = new_expr(p, E_INT, start);
    zero->ival = 0;
    zero->type = int_ty;
    Stmt *idx_init = new_stmt(p, S_VAR, start);
    idx_init->name = "idx";
    idx_init->type = int_ty;
    idx_init->slot = idx_slot;
    idx_init->init = zero;

    /* cond: idx < coll.length */
    Expr *len = new_expr(p, E_FIELD, start);
    len->lhs = arr_ref;
    len->name = "length";
    len->type = int_ty;
    Expr *cond = make_binary(p, T_LT, idx_ref, len, start);

    /* element = coll[idx] */
    Expr *idx_ref2 = new_expr(p, E_VAR, start);
    idx_ref2->slot = idx_slot;
    idx_ref2->type = int_ty;
    Expr *elem = new_expr(p, E_INDEX, start);
    elem->lhs = arr_ref;
    elem->rhs = idx_ref2;
    elem->type = coll->type->base;
    Stmt *elem_init = new_stmt(p, S_VAR, start);
    elem_init->name = name;
    elem_init->type = elem->type;
    elem_init->slot = elem_slot;
    elem_init->init = elem;

    /* idx = idx + 1 */
    Expr *one = new_expr(p, E_INT, start);
    one->ival = 1;
    one->type = int_ty;
    Expr *idx_ref3 = new_expr(p, E_VAR, start);
    idx_ref3->slot = idx_slot;
    idx_ref3->type = int_ty;
    Expr *asg = new_expr(p, E_ASSIGN, start);
    asg->op = T_PLUS;
    asg->compound = 1;
    asg->lhs = idx_ref3;
    asg->rhs = one; /* idx = idx + 1 */
    asg->type = int_ty;
    Stmt *step = new_stmt(p, S_EXPR, start);
    step->expr = asg;

    p->loop_depth++;
    Stmt *body = parse_stmt(p);
    p->loop_depth--;
    scope_pop(p);

    Stmt **inner = arena_alloc_array(p->arena, 3, sizeof(Stmt *));
    inner[0] = elem_init;
    inner[1] = step;
    inner[2] = body;
    Stmt *inner_block = new_stmt(p, S_BLOCK, start);
    inner_block->items = inner;
    inner_block->nitems = 3;

    Stmt *loop = new_stmt(p, S_WHILE, start);
    loop->cond = cond;
    loop->body = inner_block;

    Stmt **outer = arena_alloc_array(p->arena, 3, sizeof(Stmt *));
    outer[0] = arr_init;
    outer[1] = idx_init;
    outer[2] = loop;
    Stmt *wrap = new_stmt(p, S_BLOCK, start);
    wrap->items = outer;
    wrap->nitems = 3;
    return wrap;
}

/* Parses a method declared inside a struct body. The receiver `this` is a
 * hidden first parameter (a pointer to the struct); the emitted function is
 * mangled as `Struct__method` and registered on the struct. Returns the S_FUNC
 * AST node for later code generation. */
static Stmt *parse_method(Parser *p, StructDef *sd, const char *sname, Type *ret, const char *mname,
                          Span start, int is_virtual, int is_override) {
    advance(p); /* '(' */
    Type *stype = type_find_struct(&p->ty, sname);
    Type *this_ty = stype ? type_ptr(&p->ty, stype) : NULL;

    Scope *saved_scope = p->scope;
    int saved_off = p->next_offset;
    Type *saved_ret = p->cur_ret;
    StructDef *saved_msd = p->cur_msd;
    Expr *saved_this = p->cur_this;
    scope_push(p);
    p->next_offset = 0;
    p->cur_ret = ret;

    /* Qualified with the receiver, because a bare method name in a message
     * ("has no method 'lenght'") does not say which type's method list was
     * searched, and there may be ten. */
    char ctxlabel[256];
    snprintf(ctxlabel, sizeof ctxlabel, "%s.%s", sname, mname);
    diag_push_ctx(DIAG_CTX_METHOD, ctxlabel, start);

    int pcap = 4, pn = 0;
    Expr **params = arena_alloc_array(p->arena, (size_t)pcap, sizeof(Expr *));
    Expr *this_var = new_expr(p, E_VAR, start);
    this_var->name = arena_strdup(p->arena, "this");
    this_var->type = this_ty;
    this_var->slot = declare_var(p, "this", this_ty);
    /* The receiver is reached through every unqualified field and method name,
     * so a method that never writes `this` is still using it. */
    mark_synth(p, "this");
    params[pn++] = this_var;
    p->cur_msd = sd;
    p->cur_this = this_var;

    Type **ptypes = NULL;
    int nexplicit = 0;
    if (!at(p, T_RPAREN)) {
        for (;;) {
            Type *pt = parse_type(p);
            if (pt == NULL || !at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected parameter type and name");
                break;
            }
            char *pname = cur(p)->text;
            Span ps = cur(p)->span;
            advance(p);
            if (pn == pcap) {
                int ncap = pcap * 2;
                Expr **npar = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(npar, params, (size_t)pn * sizeof(Expr *));
                params = npar;
                pcap = ncap;
            }
            Expr *pe = new_expr(p, E_VAR, ps);
            pe->name = pname;
            pe->type = pt;
            pe->slot = declare_var(p, pname, pt);
            if (is_kind(pt, TK_STRUCT) || is_kind(pt, TK_UNION)) {
                pe->agg_param = 1;
                Var *pv = lookup_var_local(p, pname);
                if (pv)
                    pv->agg_param = 1;
            }
            params[pn++] = pe;
            nexplicit++;
            if (!match(p, T_COMMA))
                break;
        }
    }
    match(p, T_RPAREN);

    {
        int agg = is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION);
        /* A method's receiver is a parameter in the frame like any other, and an
         * aggregate result adds the hidden result pointer on top, so both come
         * out of the same budget the argument-setup arrays are sized by. */
        int max_params = Z_MAX_ARGS - 1 - (agg ? 1 : 0);
        if (nexplicit > max_params) {
            diag_error(start, "method '%s' takes %d parameters but the limit is %d%s", mname,
                       nexplicit, max_params,
                       agg ? " (a struct-returning method spends one on the result buffer)" : "");
        }
    }

    /* Struct-returning methods also use the hidden-pointer convention; the
     * hidden buffer pointer precedes `this` as the first argument. */
    if (is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION)) {
        Expr **np = arena_alloc_array(p->arena, (size_t)(pn + 1), sizeof(Expr *));
        memcpy(np + 1, params, (size_t)pn * sizeof(Expr *));
        Expr *hidden = new_expr(p, E_VAR, start);
        hidden->name = arena_strdup(p->arena, "$ret");
        hidden->type = type_ptr(&p->ty, ret);
        hidden->slot = declare_var(p, "$ret", hidden->type);
        /* The hidden result buffer is written by the return sequence, not named
         * in source, so it is never "used" by name. */
        mark_synth(p, "$ret");
        np[0] = hidden;
        params = np;
        pn++;
    }

    char *mangled = arena_alloc(p->arena, strlen(sname) + strlen(mname) + 3);
    snprintf(mangled, strlen(sname) + strlen(mname) + 3, "%s__%s", sname, mname);

    Stmt *fn = new_stmt(p, S_FUNC, start);
    fn->fname = mangled;
    fn->src_fname = arena_strdup(p->arena, mname);
    fn->ret_type = ret;
    fn->params = params;
    fn->nparams = pn;
    /* A method's parameter 0 is the receiver, and a struct-returning one pushes
     * the hidden result buffer in front of that, so neither is a parameter a
     * reader can match to the source. */
    fn->vis_start = params[0] != NULL && strcmp(params[0]->name, "$ret") == 0 ? 2 : 1;
    if (match(p, T_FATARROW)) {
        Expr *e = parse_expr(p);
        Stmt *ret_st = new_stmt(p, S_RETURN, start);
        ret_st->expr = e;
        Stmt **items = arena_alloc_array(p->arena, 1, sizeof(Stmt *));
        items[0] = ret_st;
        Stmt *body = new_stmt(p, S_BLOCK, start);
        body->items = items;
        body->nitems = 1;
        fn->fbody = body;
        expect_semi(p);
    } else {
        fn->fbody = parse_block(p);
    }
    fn->locals_bytes = p->next_offset;

    if (nexplicit > 0) {
        ptypes = arena_alloc_array(p->arena, (size_t)nexplicit, sizeof(Type *));
        /* params[0] is `this` (or the hidden $ret buffer if sret), so the
         * i-th explicit parameter is at params[i + 1 + regoff]. */
        int regoff = is_kind(ret, TK_STRUCT) ? 1 : 0;
        for (int i = 0; i < nexplicit; i++)
            ptypes[i] = params[i + 1 + regoff]->type;
    }
    /* A pre-scan may already have registered this method's signature so that
     * code above the declaration could see the type has the method -- which is
     * how a struct declared later can still satisfy an interface. Fill that entry
     * in rather than adding a second one, or the method would be emitted twice
     * and found twice. */
    StructMethod *m = struct_find_method(sd, mname);
    if (m == NULL || m->body != NULL) {
        m = arena_alloc(p->arena, sizeof *m);
        m->name = arena_strdup(p->arena, mname);
        m->vtable_index = -1;
        struct_add_method(&p->ty, sd, m);
    }
    m->ret = ret;
    m->ptypes = ptypes;
    m->nparams = nexplicit;
    m->body = fn;
    m->is_virtual = is_virtual;
    m->is_override = is_override;
    m->vtable_index = -1;

    scope_pop(p);
    p->scope = saved_scope;
    p->next_offset = saved_off;
    p->cur_ret = saved_ret;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
    diag_pop_ctx();
    return fn;
}

/* struct Name { field; ret method(params) {..} ret Prop { get; set; } } */
/* enum Name { Variant(fields), Variant2, ... } */
static Stmt *parse_enum_decl(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* 'enum' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected enum name after 'enum'");
        return new_stmt(p, S_EXPR, start);
    }
    char *name = cur(p)->text;
    advance(p);
    UnionDef *ud = type_define_union(&p->ty, name);
    if (ud == NULL) {
        diag_error(start, "enum '%s' is already defined", name);
    }
    /* A pre-scan already built the variant list, so that a `match` above this
     * declaration could check exhaustiveness. Rebuild it rather than appending:
     * the variants are about to be read again. */
    if (ud != NULL && ud->prescanned) {
        ud->prescanned = 0;
        ud->variants = NULL;
        ud->nvariants = 0;
        ud->size = 0;
        ud->align = 0;
    }
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open enum body");
        return new_stmt(p, S_EXPR, start);
    }
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected variant name");
            break;
        }
        char *vname = cur(p)->text;
        advance(p);
        Type *ftypes[16];
        const char *fnames[16];
        int nf = 0;
        if (match(p, T_LPAREN)) {
            while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
                Type *ft = parse_type(p);
                if (ft == NULL || !at(p, T_IDENT)) {
                    diag_error(cur(p)->span, "expected variant field type and name");
                    break;
                }
                fnames[nf] = cur(p)->text;
                advance(p);
                if (nf < 16)
                    ftypes[nf] = ft;
                nf++;
                if (!match(p, T_COMMA))
                    break;
            }
            match(p, T_RPAREN);
        }
        if (ud != NULL)
            union_add_variant(&p->ty, ud, vname, ftypes, fnames, nf);
        if (!match(p, T_COMMA))
            break;
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close enum body");
    }
    if (ud != NULL)
        union_finish(ud);
    Stmt *s = new_stmt(p, S_UNION, start);
    s->udef = ud;
    return s;
}

/* match subject { Variant(a, b) => body, _ => body } (exhaustive). */
static Expr *parse_match(Parser *p, Span start) {
    advance(p); /* 'match' */
    Expr *subject = parse_expr(p);
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' after match subject");
        return new_expr(p, E_INT, start);
    }
    Type *ut = subject->type;
    UnionDef *ud = is_kind(ut, TK_UNION) ? ut->udef : NULL;
    if (ud == NULL && !is_unk(ut)) {
        diag_error(start, "match requires a union value but got '%s'", type_name(&p->ty, ut));
    }

    int cap = 4, n = 0;
    MatchArm *arms = arena_alloc_array(p->arena, (size_t)cap, sizeof(MatchArm));
    int covered = 0;
    int wildcard = 0;
    /* Which variants an arm named, so the exhaustiveness diagnostic can list the
     * ones still missing. "not exhaustive: 4 variants but 2 covered" makes the
     * reader diff two lists themselves; naming the two tells them what to add. */
    int nslots = (ud != NULL && ud->nvariants > 0) ? ud->nvariants : 1;
    char *hit = arena_alloc_array(p->arena, (size_t)nslots, 1);
    memset(hit, 0, (size_t)nslots);
    Type *restype = NULL;
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        VariantDef *v = NULL;
        if (at(p, T_IDENT) && strcmp(cur(p)->text, "_") == 0) {
            advance(p);
            wildcard = 1;
        } else if (at(p, T_IDENT)) {
            char *vname = cur(p)->text;
            advance(p);
            if (ud != NULL) {
                for (int i = 0; i < ud->nvariants; i++) {
                    if (strcmp(ud->variants[i].name, vname) == 0) {
                        v = &ud->variants[i];
                        break;
                    }
                }
            }
            if (v == NULL) {
                if (ud != NULL) {
                    const char *vc[64];
                    int nvc = ud->nvariants < 64 ? ud->nvariants : 64;
                    for (int i = 0; i < nvc; i++)
                        vc[i] = ud->variants[i].name;
                    diag_suggest_note("variant", vname, vc, nvc);
                }
                diag_error_code(start, "no_such_variant", "'%s' is not a variant of '%s'", vname,
                                ud ? ud->name : "this match");
                v = NULL;
            } else {
                covered++;
                hit[v - ud->variants] = 1;
            }
        } else {
            diag_error(cur(p)->span, "expected a variant name or '_' in match arm");
            break;
        }

        /* Each arm gets its own scope for its payload bindings. They are only
         * frame slots as far as codegen is concerned, so the scope is purely a
         * name-resolution device -- and without it two arms binding `v` collide,
         * which matters for Result because matching one is two arms whose
         * bindings naturally want the same names. */
        scope_push(p);
        int bcap = 4, bn = 0;
        int *slots = NULL;
        if (match(p, T_LPAREN)) {
            slots = arena_alloc_array(p->arena, (size_t)bcap, sizeof(int));
            while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
                if (!at(p, T_IDENT)) {
                    diag_error(cur(p)->span, "expected a binding name");
                    break;
                }
                /* bound name: the payload field at the same index */
                int fidx = bn;
                Type *ft = (v != NULL && fidx < v->nfields) ? v->fields[fidx].type : NULL;
                char *bname = cur(p)->text;
                advance(p);
                if (bn == bcap) {
                    int ncap = bcap * 2;
                    int *ns = arena_alloc_array(p->arena, (size_t)ncap, sizeof(int));
                    memcpy(ns, slots, (size_t)bn * sizeof(int));
                    slots = ns;
                    bcap = ncap;
                }
                if (ft == NULL)
                    ft = type_int(&p->ty);
                slots[bn] = declare_var(p, bname, ft);
                /* `_` is a binding that is deliberately not read. It still needs
                 * a slot -- the arm's payload is copied into one either way -- but
                 * it is not a name, so it is not a use and must not be warned
                 * about. This is how an arm discards the payload it does not
                 * need, which with a per-arm scope is now the common case. */
                if (strcmp(bname, "_") == 0)
                    mark_synth(p, bname);
                bn++;
                if (!match(p, T_COMMA))
                    break;
            }
            match(p, T_RPAREN);
            if (v != NULL && bn != v->nfields && !is_unk(ut)) {
                diag_error(start, "variant '%s' expects %d binding%s but got %d", v->name,
                           v->nfields, v->nfields == 1 ? "" : "s", bn);
            }
        } else if (v != NULL && v->nfields > 0 && !is_unk(ut)) {
            diag_error(start, "variant '%s' expects %d binding%s", v->name, v->nfields,
                       v->nfields == 1 ? "" : "s");
        }

        if (!match(p, T_FATARROW)) {
            diag_error(cur(p)->span, "expected '=>' in match arm");
        }
        Expr *body = parse_expr(p);
        scope_pop(p);
        if (restype == NULL)
            restype = body->type;
        else if (!is_unk(restype) && !is_unk(body->type) && !type_equals(restype, body->type)) {
            diag_error(start, "match arms have different types '%s' and '%s'",
                       type_name(&p->ty, restype), type_name(&p->ty, body->type));
        }
        if (n == cap) {
            int ncap = cap * 2;
            MatchArm *na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(MatchArm));
            memcpy(na, arms, (size_t)n * sizeof(MatchArm));
            arms = na;
            cap = ncap;
        }
        arms[n].variant = v;
        arms[n].bind_slots = slots;
        arms[n].nbind = bn;
        arms[n].body = body;
        n++;
        if (!match(p, T_COMMA))
            break;
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close match");
    }
    /* Exhaustiveness: every variant must be covered (or a `_` wildcard). */
    if (ud != NULL && !wildcard && covered < ud->nvariants) {
        /* One note naming each missing variant, so the list is where the reader
         * is already looking rather than in a second diagnostic they have to
         * match up by hand. */
        for (int i = 0; i < ud->nvariants; i++) {
            if (!hit[i])
                diag_note("'%s' is not handled; add '%s%s' or a '_' arm", ud->variants[i].name,
                          ud->variants[i].name,
                          ud->variants[i].nfields > 0 ? "(...)" : "");
        }
        diag_error_code(start, "non_exhaustive_match",
                        "match does not handle every variant of '%s': %d of %d covered", ud->name,
                        covered, ud->nvariants);
    }

    Expr *e = new_expr(p, E_MATCH, start);
    e->lhs = subject;
    e->arms = arms;
    e->narms = n;
    e->type = restype;
    return e;
}

static Stmt *parse_struct_decl(Parser *p, int is_class) {
    Span start = cur(p)->span;
    advance(p); /* 'struct' or 'class' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected type name after '%s'", is_class ? "class" : "struct");
        return new_stmt(p, S_EXPR, start);
    }
    char *name = cur(p)->text;
    advance(p);
    StructDef *sd = type_define_struct(&p->ty, name);
    if (sd == NULL) {
        diag_error(start, "%s '%s' is already defined", is_class ? "class" : "struct", name);
    }
    /* A pre-scan already gave this declaration its fields, so that code above it
     * in the file could use it. Start the field list over: the members are about
     * to be read again, and appending to the pre-scanned list would double every
     * field and make every offset wrong. */
    if (sd != NULL && sd->prescanned) {
        sd->prescanned = 0;
        sd->fields = NULL;
        sd->nfields = 0;
        sd->props = NULL;
        sd->nprops = 0;
        sd->prop_cap = 0;
        sd->size = 0;
        sd->align = 1;
    }
    if (sd != NULL && is_class) {
        sd->is_class = 1;
        /* `class C : B` — resolve the base and reserve the vptr + inherited
         * fields so this class's fields append after them. */
        StructDef *base = NULL;
        if (match(p, T_COLON)) {
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected base class name after ':'");
            } else {
                Type *bt = type_find_struct(&p->ty, cur(p)->text);
                base = bt ? bt->sdef : NULL;
                if (base == NULL || !base->is_class) {
                    diag_error(cur(p)->span, "unknown or non-class base '%s'", cur(p)->text);
                } else if (base == sd) {
                    diag_error(cur(p)->span, "class '%s' cannot inherit from itself", name);
                    base = NULL;
                }
                advance(p);
            }
        }
        sd->base = base;
        sd->align = 8;
        sd->size = base ? base->size : 8; /* vptr (8 bytes) [+ base fields] */
    }
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open %s body", is_class ? "class" : "struct");
        return new_stmt(p, S_EXPR, start);
    }
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        int mvirtual = 0, moverride = 0;
        if (at(p, T_KW_VIRTUAL)) {
            mvirtual = 1;
            advance(p);
        } else if (at(p, T_KW_OVERRIDE)) {
            moverride = 1;
            mvirtual = 1;
            advance(p);
        }
        Type *mt = parse_type(p);
        if (mt == NULL) {
            diag_error(cur(p)->span, "expected member type but found %s",
                       token_kind_name(cur(p)->kind));
            break;
        }
        if (is_class && at(p, T_LPAREN) && is_kind(mt, TK_STRUCT) && mt->sdef == sd) {
            /* Constructor: `C(params) { ... }` (a method named after the class).
             * It returns void (not the class type), so no hidden sret param. */
            parse_method(p, sd, name, type_void(&p->ty), name, cur(p)->span, 0, 0);
            continue;
        }
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected member name");
            break;
        }
        char *mname = cur(p)->text;
        Span mspan = cur(p)->span;
        advance(p);
        if (at(p, T_LPAREN)) {
            /* method */
            parse_method(p, sd, name, mt, mname, mspan, mvirtual, moverride);
        } else if (at(p, T_SEMI)) {
            /* plain field */
            advance(p);
            if (sd != NULL)
                struct_add_field(&p->ty, sd, mname, mt);
        } else if (at(p, T_LBRACE)) {
            /* auto-property: `int X { get; set; }` -> backing field + accessor
             * methods named `X`. */
            advance(p); /* '{' */
            int has_get = 0;
            while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                if (at(p, T_IDENT) && strcmp(cur(p)->text, "get") == 0) {
                    has_get = 1;
                    advance(p);
                    match(p, T_SEMI);
                } else if (at(p, T_IDENT) && strcmp(cur(p)->text, "set") == 0) {
                    advance(p);
                    match(p, T_SEMI);
                } else {
                    diag_error(cur(p)->span, "expected 'get' or 'set' in property");
                    break;
                }
            }
            if (!match(p, T_RBRACE)) {
                diag_error(cur(p)->span, "expected '}' to close property");
            }
            if (!has_get) {
                diag_error(mspan, "property '%s' needs at least a getter", mname);
            }
            if (sd != NULL) {
                /* The backing field IS the property storage; `p.X` reads/writes
                 * it directly, equivalent to a trivial getter/setter. */
                struct_add_prop_field(&p->ty, sd, mname, mt);
            }
        } else if (at(p, T_FATARROW)) {
            /* Expression-bodied property: get-only, backed by a `get` method. */
            advance(p);
            /* parse body in a method context */
            Scope *ss = p->scope;
            int so = p->next_offset;
            Type *sr = p->cur_ret;
            StructDef *sm = p->cur_msd;
            Expr *st = p->cur_this;
            scope_push(p);
            p->next_offset = 0;
            p->cur_ret = mt;
            Type *sty = type_find_struct(&p->ty, name);
            Expr *tv = new_expr(p, E_VAR, mspan);
            tv->name = arena_strdup(p->arena, "this");
            tv->type = sty ? type_ptr(&p->ty, sty) : NULL;
            tv->slot = declare_var(p, "this", tv->type);
            mark_synth(p, "this");
            p->cur_msd = sd;
            p->cur_this = tv;
            Expr *e = parse_expr(p);
            Stmt *ret_st = new_stmt(p, S_RETURN, mspan);
            ret_st->expr = e;
            Stmt **items = arena_alloc_array(p->arena, 1, sizeof(Stmt *));
            items[0] = ret_st;
            Stmt *body = new_stmt(p, S_BLOCK, mspan);
            body->items = items;
            body->nitems = 1;
            char *gname = arena_alloc(p->arena, strlen(name) + strlen(mname) + 8);
            snprintf(gname, strlen(name) + strlen(mname) + 8, "%s__get_%s", name, mname);
            Stmt *fn = new_stmt(p, S_FUNC, mspan);
            fn->fname = gname;
            fn->ret_type = mt;
            Expr **gparams = arena_alloc_array(p->arena, 1, sizeof(Expr *));
            gparams[0] = tv;
            fn->params = gparams;
            fn->nparams = 1;
            fn->fbody = body;
            fn->locals_bytes = p->next_offset;
            StructMethod *m = arena_alloc(p->arena, sizeof *m);
            m->name = arena_strdup(p->arena, mname);
            m->ret = mt;
            m->ptypes = NULL;
            m->nparams = 0;
            m->body = fn;
            struct_add_method(&p->ty, sd, m);
            scope_pop(p);
            p->scope = ss;
            p->next_offset = so;
            p->cur_ret = sr;
            p->cur_msd = sm;
            p->cur_this = st;
            expect_semi(p);
        } else {
            diag_error(cur(p)->span, "expected ';', '(', or '=>' after member name");
            break;
        }
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close struct body");
    }
    if (sd != NULL)
        struct_finish(sd);
    if (sd != NULL && is_class) {
        if (class_finish_vtable(&p->ty, sd) != 0) {
            diag_error(start, "class '%s' has an 'override' with no matching base virtual method",
                       name);
        }
    }
    Stmt *s = new_stmt(p, S_STRUCT, start);
    s->sdef = sd;
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    Token *t = cur(p);

    /* A function declared inside another function, written where a statement
     * goes. `at_func_decl` requires a type, then a name, then '(', so this
     * cannot swallow `print(x);` (no leading type), `var v = 1;` (`var` is not a
     * type token), or `P p = ...;` (an '=' where the '(' would be). It also
     * tolerates a leading `extern`/`export`, which parse_func then rejects. */
    if (at_func_decl(p)) {
        parse_func(p, 1);
        /* The declaration emits nothing where it stands; the function is emitted
         * as a hoisted one, under a symbol of its own. An empty block is the
         * honest shape for a statement that produces no code. */
        return new_stmt(p, S_BLOCK, t->span);
    }

    switch (t->kind) {
    case T_KW_IF: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        Expr *cond = parse_expr(p);
        if (!is_kind(cond->type, TK_BOOL) && !is_unk(cond->type)) {
            diag_error(start, "condition must be 'bool' but got '%s'",
                       type_name(&p->ty, cond->type));
        }
        match(p, T_RPAREN);
        Stmt *s = new_stmt(p, S_IF, start);
        s->cond = cond;
        s->body = parse_stmt(p);
        if (match(p, T_KW_ELSE))
            s->orelse = parse_stmt(p);
        return s;
    }
    case T_KW_WHILE: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        Expr *cond = parse_expr(p);
        if (!is_kind(cond->type, TK_BOOL) && !is_unk(cond->type)) {
            diag_error(start, "condition must be 'bool' but got '%s'",
                       type_name(&p->ty, cond->type));
        }
        match(p, T_RPAREN);
        Stmt *s = new_stmt(p, S_WHILE, start);
        s->cond = cond;
        p->loop_depth++;
        s->body = parse_stmt(p);
        p->loop_depth--;
        return s;
    }
    case T_KW_FOR: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        Stmt *init = NULL;
        if (!at(p, T_SEMI)) {
            if (at(p, T_KW_CONST)) {
                init = parse_const_decl(p); /* consumes its ';' */
            } else if (at(p, T_KW_VAR) || is_type_token(cur(p)->kind)) {
                init = parse_var_decl(p); /* consumes its ';' */
            } else {
                Expr *e = parse_expr(p);
                init = new_stmt(p, S_EXPR, start);
                init->expr = e;
                match(p, T_SEMI);
            }
        } else {
            advance(p);
        }
        Expr *cond = NULL;
        if (!at(p, T_SEMI))
            cond = parse_expr(p);
        match(p, T_SEMI);
        Expr *step = NULL;
        if (!at(p, T_RPAREN))
            step = parse_expr(p);
        match(p, T_RPAREN);
        Stmt *s = new_stmt(p, S_FOR, start);
        s->for_init = init;
        s->cond = cond;
        s->for_step = step;
        p->loop_depth++;
        s->body = parse_stmt(p);
        p->loop_depth--;
        return s;
    }
    case T_KW_FOREACH:
        return parse_foreach(p, t->span);
    case T_KW_RETURN: {
        Span start = t->span;
        advance(p);
        Stmt *s = new_stmt(p, S_RETURN, start);
        if (at(p, T_SEMI)) {
            s->expr = NULL;
        } else {
            /* The function's return type tells `Ok(...)`/`Err(...)` what to
             * build, which is what makes `return Ok(3);` work unannotated. */
            Type *saved_expect = p->expect_result;
            p->expect_result = type_is_result(p->cur_ret) ? p->cur_ret : NULL;
            s->expr = parse_expr(p);
            p->expect_result = saved_expect;
            if (is_kind(p->cur_ret, TK_IFACE))
                s->expr = to_iface(p, s->expr, p->cur_ret, start);
            if (!is_unk(s->expr->type) && !is_unk(p->cur_ret) &&
                !type_equals(s->expr->type, p->cur_ret)) {
                diag_error(start, "cannot return '%s' from function returning '%s'",
                           type_name(&p->ty, s->expr->type), type_name(&p->ty, p->cur_ret));
            }
        }
        expect_semi(p);
        return s;
    }
    case T_LBRACE:
        return parse_block(p);
    case T_KW_VAR:
        return parse_var_decl(p);
    case T_KW_CONST:
        return parse_const_decl(p);
    /* A type declared inside a function. It is queued with the other hoisted
     * declarations rather than emitted where it stands: a class's vtable is
     * emitted by walking the top-level items, so a local class that stayed in
     * this body would have no vtable and a virtual call through it would jump
     * through a null slot. The declaration itself produces no code, so the
     * statement it parsed to is an empty block. */
    case T_KW_INTERFACE: {
        Stmt *s = parse_interface_decl(p);
        add_pending(p, s);
        return new_stmt(p, S_BLOCK, t->span);
    }
    case T_KW_CLASS:
    case T_KW_STRUCT:
    case T_KW_ENUM: {
        Stmt *s = at(p, T_KW_ENUM) ? parse_enum_decl(p)
                                    : parse_struct_decl(p, at(p, T_KW_CLASS));
        add_pending(p, s);
        return new_stmt(p, S_BLOCK, t->span);
    }
    case T_KW_BREAK: {
        Span bs = t->span;
        advance(p);
        expect_semi(p);
        if (p->loop_depth == 0)
            diag_error(bs, "'break' is only valid inside a loop");
        return new_stmt(p, S_BREAK, bs);
    }
    case T_KW_CONTINUE: {
        Span cs = t->span;
        advance(p);
        expect_semi(p);
        if (p->loop_depth == 0)
            diag_error(cs, "'continue' is only valid inside a loop");
        return new_stmt(p, S_CONTINUE, cs);
    }
    default:
        break;
    }

    /* An explicitly typed local: `Type name = expr;`.
     *
     * This used to be reachable only for the keyword types. A named type -- the
     * spelling a struct is declared and used with -- never got here: the code
     * below treated any leading identifier as an unknown type and reported it
     * without a lookup, so `P r = new P();` could not be written at all, and
     * `P* q = null;` fell through to the expression parser and read as a
     * variable named `P`. Both are fixed by resolving the name first. */
    if (at_typed_decl(p) && base_type_or_name(p, cur(p)) != NULL)
        return parse_var_decl(p);

    if (is_type_token(cur(p)->kind)) {
        return parse_var_decl(p);
    }
    if (t->kind == T_IDENT && at_typed_decl(p)) {
        /* The name is in declaration position but is not a type. Say which one,
         * then skip the declaration so the rest of the block still parses. */
        suggest_in_scope(p, SK_TYPE, "type", t->text);
        diag_error_code(t->span, "unknown_type", "unknown type '%s'", t->text);
        int guard = 0;
        while (!at(p, T_SEMI) && !at(p, T_EOF) && guard++ < 256)
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, t->span);
    }

    Span start = t->span;
    Expr *e = parse_expr(p);
    expect_semi(p);
    Stmt *s = new_stmt(p, S_EXPR, start);
    s->expr = e;
    return s;
}

Stmt *parse_program(Arena *arena, Token *toks, int ntoks, StringTable *strings,
                    int opt_level) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.toks = toks;
    p.ntoks = ntoks;
    typectx_init(&p.ty, arena);
    p.strings = strings;
    p.cur_ret = NULL;
    p.opt_level = opt_level;

    prescan_struct_names(&p);
    prescan_struct_fields(&p);
    prescan_signatures(&p);

    Stmt *program = new_stmt(&p, S_BLOCK, cur(&p)->span);
    scope_push(&p);
    p.next_offset = 0;

    int cap = 8, n = 0;
    Stmt **items = arena_alloc_array(arena, (size_t)cap, sizeof(Stmt *));
    int has_main = 0;
    int top_stmts = 0;

    while (!at(&p, T_EOF)) {
        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        if (at_func_decl(&p)) {
            Stmt *fn = parse_func(&p, 0);
            if (fn->is_entry)
                has_main = 1;
            items[n++] = fn;
        } else if (at(&p, T_KW_CLASS)) {
            items[n++] = parse_struct_decl(&p, 1); /* a class decl */
        } else if (at(&p, T_KW_STRUCT)) {
            items[n++] = parse_struct_decl(&p, 0); /* a type decl, not a statement */
        } else if (at(&p, T_KW_INTERFACE)) {
            items[n++] = parse_interface_decl(&p);
        } else if (at(&p, T_KW_ENUM)) {
            items[n++] = parse_enum_decl(&p); /* a type decl, not a statement */
        } else if (at(&p, T_KW_CONST)) {
            /* A top-level const is a declaration, not a top-level statement, so
             * it may sit alongside an explicit main(). It folds to a literal
             * and emits no code. */
            items[n++] = parse_const_decl(&p);
        } else {
            items[n++] = parse_stmt(&p);
            top_stmts++;
        }
    }

    /* A lambda marks the captured declarations in the body of the function that
     * encloses it, and it does that by walking an S_BLOCK. Top-level statements
     * are appended to `items` one at a time and only gathered into a block below,
     * after the whole file is read, so nothing walked them: a `var` a lambda
     * captured kept no box, and the closure's environment read the raw frame slot
     * that the initializer was supposed to fill. No parse_func runs for a
     * top-level program, so p->captured is still the whole file's list here. */
    for (int i = 0; i < n; i++)
        mark_boxed_stmt(&p, items[i]);

    /* Append monomorphized generic instances so codegen emits them. */
    for (int i = 0; i < p.npending; i++) {
        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        items[n++] = p.pending[i];
    }

    if (has_main && top_stmts > 0) {
        diag_error(items[0]->span, "cannot combine top-level statements with a 'main' function");
    }

    if (!has_main) {
        Stmt **body_items = arena_alloc_array(arena, (size_t)(n > 0 ? n : 1), sizeof(Stmt *));
        int bi = 0;
        for (int i = 0; i < n; i++) {
            if (items[i]->kind != S_FUNC)
                body_items[bi++] = items[i];
        }
        Stmt *body = new_stmt(&p, S_BLOCK, items[0] ? items[0]->span : cur(&p)->span);
        body->items = body_items;
        body->nitems = bi;

        Stmt *entry = new_stmt(&p, S_FUNC, body->span);
        entry->fname = arena_strdup(arena, "main");
        entry->ret_type = type_void(&p.ty);
        entry->fbody = body;
        entry->is_entry = 1;
        entry->locals_bytes = p.next_offset;

        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        items[n++] = entry;
    }

    program->items = items;
    program->nitems = n;
    /* Inlining runs over the finished program, so a call is inlinable no matter
     * which order the two functions were written in, and the synthesized entry is
     * a function like any other as far as it is concerned. */
    inline_program(&p, program);
    scope_pop(&p);
    return program;
}

/* ---- closed-form induction variables ----
 *
 * A counted loop whose body only accumulates loop-invariant amounts has a value
 * computable without running the loop:
 *
 *     var sum = 0; var i = 0;
 *     while (i < 20000000) { sum = sum + 82; i = i + 1; }
 *
 * adds 82 twenty million times, which is `sum = 82 * 20000000` written once. The
 * loop disappears. The body of that loop is invariant, so this is the limit of
 * what loop-invariant code motion can do -- it hoists the pieces, and this
 * notices there is nothing left to run.
 *
 * It runs as a tree rewrite in the parser rather than as a code-generation
 * trick, so everything downstream -- the register allocator, constant folding,
 * the strength reducer -- sees the finished statement and optimizes it for free.
 * In the example the result is a product of two constants, which the folder then
 * evaluates outright.
 *
 * The pattern is deliberately narrow. Every condition below is there because
 * getting it wrong is a wrong answer, not a missed speedup:
 *
 *   - the step is `i = i + C` or `i -= C` with C a nonzero constant;
 *   - the condition compares that same variable against a bound with `<`, `<=`,
 *     `>` or `>=`, and the bound does not change in the body;
 *   - the body is a straight-line run of `x = x + E`, `x = x - E`, `x += E`,
 *     `x -= E` and plain `x = E`, where x is a scalar local that is not the
 *     induction variable;
 *   - no contribution reads the induction variable, the accumulators, or the
 *     trip count, so each iteration really does add the same amount;
 *   - nothing in the body calls, allocates, or branches, and there is no `break`
 *     or `continue` -- an early exit means the trip count is not this one.
 *
 * Overflow behaves as it did: the closed form computes the same total the loop
 * would have, so a loop that overflowed was already outside what Z defines. */

#define CLOSE_FORM_MAX_ACC 8
#define CLOSE_FORM_MAX_VARS 16

typedef struct {
    int slot;
    char *name;
    Type *type;
    Expr *amount; /* the signed contribution of one iteration */
    /* The statement's own left-hand side, reused as the assignment target rather
     * than looked up again: this pass runs after the function's scope has been
     * popped, so a lookup by name would find nothing. */
    Expr *target;
} CloseFormAcc;

/* 1 when `e` reads any slot in `variant`, which is what stops it being
 * loop-invariant. A local's address counts as a read: the body could write
 * through it. */
static int expr_reads_any(Expr *e, const int *variant, int nvariant) {
    if (e == NULL)
        return 0;
    if (e->kind == E_VAR) {
        for (int i = 0; i < nvariant; i++)
            if (variant[i] == e->slot)
                return 1;
        return 0;
    }
    if (expr_reads_any(e->lhs, variant, nvariant) || expr_reads_any(e->rhs, variant, nvariant))
        return 1;
    for (int i = 0; i < e->nargs; i++)
        if (expr_reads_any(e->args[i], variant, nvariant))
            return 1;
    if (expr_reads_any(e->env, variant, nvariant))
        return 1;
    return 0;
}

/* Flattens an additive chain into the sum of everything it adds to the
 * accumulator, discarding the accumulator itself.
 *
 *     sum = sum + a*b + c*d - (a + c)
 *
 * parses as a left-leaning tree of `+` and `-` rooted at `sum`, and the terms are
 * a*b, c*d and -(a+c). Only the root has to be the variable: an interior `+` is a
 * term in its own right, and `a + c` is just an expression that happens to add.
 * Keeping the terms signed and summing them is what lets the loop be solved --
 * the amount per iteration is a constant, and three separately-hoisted
 * subexpressions would not say so.
 *
 * Returns 0 if any term reads the accumulator, which would make the contribution
 * differ between iterations. `*out` accumulates the running sum and starts NULL. */
static int flatten_accum(Parser *p, Expr *e, int slot, int neg, Span span, Expr **out,
                         int *found_base) {
    if (e == NULL)
        return 0;
    if (e->kind == E_VAR) {
        /* The accumulator itself contributes nothing -- that is the base the
         * chain is rooted at. A *different* variable here is an ordinary term:
         * `sum + a` has `a` in exactly this position. A stray read of the
         * accumulator further along is caught below as a term that reads it,
         * which is what makes `x = x * E` and `x = x + x` both fail. */
        if (e->slot == slot) {
            *found_base = 1;
            return 1;
        }
    }
    if (e->kind == E_BINARY && (e->op == T_PLUS || e->op == T_MINUS)) {
        int rneg = neg;
        if (e->op == T_MINUS)
            rneg = !neg;
        if (!flatten_accum(p, e->lhs, slot, neg, span, out, found_base))
            return 0;
        return flatten_accum(p, e->rhs, slot, rneg, span, out, found_base);
    }
    /* A term. It must be an integer, or the algebra below does not hold: `s = s +
     * "x"` looks exactly like an accumulate, and multiplying a string by the trip
     * count is not a thing. And it must not read the accumulator, or the amount
     * would depend on how many times the loop has already run. */
    if (!is_kind(e->type, TK_INT) || expr_reads_any(e, &slot, 1))
        return 0;
    Expr *term = e;
    if (neg) {
        Expr *zero = new_expr(p, E_INT, span);
        zero->type = type_int(&p->ty);
        term = make_binary(p, T_MINUS, zero, e, span);
    }
    *out = *out == NULL ? term : make_binary(p, T_PLUS, *out, term, span);
    return 1;
}

/* Matches one accumulate-into-a-local statement and reduces it to the signed
 * amount it adds per iteration.
 *
 *   `x += E` / `x = x + E`  -> +E
 *   `x -= E` / `x = x - E`  -> -E
 *
 * Returns 0 for anything else, including a compound form that is not additive
 * (`x = x * E` would need a power, `x = x / E` a logarithm) and a plain `x = E`,
 * whose result does not depend on the trip count at all. */
static int close_form_match(Parser *p, Stmt *st, CloseFormAcc *acc) {
    if (st == NULL || st->kind != S_EXPR || st->expr == NULL || st->expr->kind != E_ASSIGN)
{ fprintf(stderr, "MATCH fail 1\n"); return 0; }
    Expr *a = st->expr;
    if (a->lhs == NULL || a->lhs->kind != E_VAR || a->lhs->slot <= 0)
{ fprintf(stderr, "MATCH fail 2\n"); return 0; }
    if (is_kind(a->lhs->type, TK_STRUCT) || is_kind(a->lhs->type, TK_UNION) ||
        is_kind(a->lhs->type, TK_ARRAY) || is_kind(a->lhs->type, TK_PTR) ||
        is_kind(a->lhs->type, TK_F64))
{ fprintf(stderr, "MATCH fail 3\n"); return 0; }
    Expr *amount = NULL;
    if (a->compound) {
        if (a->op != T_PLUS && a->op != T_MINUS)
{ fprintf(stderr, "MATCH fail 4\n"); return 0; }
        if (expr_reads_any(a->rhs, &a->lhs->slot, 1))
{ fprintf(stderr, "MATCH fail 5\n"); return 0; }
        if (a->op == T_MINUS) {
            Expr *zero = new_expr(p, E_INT, st->span);
            zero->type = type_int(&p->ty);
            amount = make_binary(p, T_MINUS, zero, a->rhs, st->span);
        } else {
            amount = a->rhs;
        }
    } else {
        /* `x = <chain rooted at x>` and nothing else. A self-update that is not
         * additive has the same shape, so the flatten below is what rejects it:
         * `x = x * E` reaches it as a term that reads x. */
        int found_base = 0;
        if (!flatten_accum(p, a->rhs, a->lhs->slot, 0, st->span, &amount, &found_base) ||
            !found_base)
            return 0; /* not rooted at the accumulator: a plain set, not an update */
        if (amount == NULL)
            return 0; /* `x = x`: nothing accumulates */
    }
    acc->slot = a->lhs->slot;
    acc->name = a->lhs->name;
    acc->type = a->lhs->type;
    acc->amount = amount;
    acc->target = a->lhs;
    return 1;
}

/* The induction variable, its stride, and the bound, from a loop's step and
 * condition. Returns 1 on a match. */
static int close_form_trip(Parser *p, Stmt *loop, Expr *step_in, int *ivar, long long *stride,
                           Expr **bound, int *strict) {
    /* A `for` carries its step in the header; a `while` carries it as the last
     * statement of its body, which the caller peels off and passes in. */
    Expr *step = step_in != NULL ? step_in : loop->for_step;
    if (step == NULL || step->kind != E_ASSIGN)
        return 0;
    if (step->lhs == NULL || step->lhs->kind != E_VAR)
        return 0;
    int slot = step->lhs->slot;
    if (slot <= 0 || step->rhs == NULL)
        return 0;
    /* Four spellings of the same step, all a self-update by a constant:
     *
     *     i = i + C    i = i - C    i += C    i -= C
     *
     * The compound forms carry the operator on the assignment node and the bare
     * amount on the right; the spelled-out forms carry it on a binary node whose
     * left has to be the same variable, since `i = j + 1` walks a different one
     * and its trip count is not a stride. `i = i` is a zero stride and is
     * rejected below, along with a non-constant amount. */
    long long c = 0;
    if (step->compound) {
        if (step->op != T_PLUS && step->op != T_MINUS)
            return 0;
        long long d;
        if (!const_fold_static(p, step->rhs, &d))
            return 0;
        c = step->op == T_MINUS ? -d : d;
    } else if (step->rhs->kind == E_VAR) {
        if (step->rhs->slot != slot || !const_fold_static(p, step->rhs, &c))
            return 0;
    } else if (step->rhs->kind == E_BINARY && step->rhs->lhs != NULL &&
               step->rhs->lhs->kind == E_VAR && step->rhs->lhs->slot == slot &&
               (step->rhs->op == T_PLUS || step->rhs->op == T_MINUS)) {
        long long d;
        if (!const_fold_static(p, step->rhs->rhs, &d))
            return 0;
        c = step->rhs->op == T_MINUS ? -d : d;
    } else {
        return 0;
    }
    if (c == 0)
        return 0; /* a zero stride never terminates */
    Expr *cond = loop->cond;
    if (cond == NULL || cond->kind != E_BINARY)
        return 0;
    Expr *cv = NULL;
    Expr *cb = NULL;
    switch (cond->op) {
    case T_LT:
    case T_LE:
        cv = cond->lhs;
        cb = cond->rhs;
        *strict = cond->op == T_LT;
        break;
    case T_GT:
    case T_GE:
        cv = cond->lhs;
        cb = cond->rhs;
        *strict = cond->op == T_GT;
        c = -c; /* a downward loop, so the stride is negative */
        break;
    default:
        return 0;
    }
    if (cv == NULL || cb == NULL || cv->kind != E_VAR || cv->slot != slot)
        return 0;
    *ivar = slot;
    *stride = c;
    *bound = cb;
    return 1;
}

/* Applies close_form_loop to every loop in a function body, innermost first, so
 * that a closed form computed for an inner loop is visible to an enclosing one's
 * analysis. An inner loop that collapses leaves an ordinary block of assignments
 * behind, which the enclosing pass then sees as accumulate statements and can
 * close over in turn. */
static void close_form_stmts(Parser *p, Stmt **items, int n) {
    for (int i = 0; i < n; i++) {
        Stmt *s = items[i];
        if (s == NULL)
            continue;
        switch (s->kind) {
        case S_BLOCK:
            close_form_stmts(p, s->items, s->nitems);
            continue;
        case S_FOR:
        case S_WHILE:
            close_form_stmts(p, s->body != NULL && s->body->kind == S_BLOCK ? s->body->items
                                                                            : NULL,
                             s->body != NULL && s->body->kind == S_BLOCK ? s->body->nitems : 0);
            break;
        case S_IF:
            close_form_stmts(p, s->body != NULL && s->body->kind == S_BLOCK ? s->body->items
                                                                            : NULL,
                             s->body != NULL && s->body->kind == S_BLOCK ? s->body->nitems : 0);
            break;
        default:
            continue;
        }
        Stmt *replaced = close_form_loop(p, s, items, i);
        if (replaced != s)
            items[i] = replaced;
    }
}

/* Rewrites a counted accumulate loop into its closed form, or returns it
 * unchanged. */
static Stmt *close_form_loop(Parser *p, Stmt *loop, Stmt **siblings, int sib_index) {
    if (p->opt_level < 2)
return loop;
    if (loop->kind != S_FOR && loop->kind != S_WHILE)
return loop;
    /* A `while` keeps its step in the body, so peel it off and count only what is
     * left as the body's accumulators. The trip count does not include the step,
     * which runs one extra time -- as it does in the original loop. */
    Stmt *wbody = loop->body;
    int peel = 0;
    Expr *step_expr = NULL;
    if (loop->kind == S_WHILE) {
        if (wbody == NULL || wbody->kind != S_BLOCK || wbody->nitems < 1)
return loop;
        Stmt *last = wbody->items[wbody->nitems - 1];
        if (last->kind != S_EXPR || last->expr == NULL || last->expr->kind != E_ASSIGN)
return loop;
        step_expr = last->expr;
        peel = 1;
    }
    int ivar;
    long long stride;
    Expr *bound = NULL;
    int strict = 0;
    if (!close_form_trip(p, loop, step_expr, &ivar, &stride, &bound, &strict))
return loop;
    long long limit;
    if (!const_fold_static(p, bound, &limit))
return loop;

    Stmt *body = loop->body;
    if (body == NULL)
return loop;
    /* Only a straight-line block. A nested loop, a branch or a `break` means the
     * trip count is not the one computed here. */
    if (body->kind != S_BLOCK)
return loop;
    int nbody = body->nitems - peel;

    /* Collect the accumulators, and the slots the body writes, so a later
     * contribution can be checked against them. */
    CloseFormAcc accs[CLOSE_FORM_MAX_ACC];
    int nacc = 0;
    int written[CLOSE_FORM_MAX_VARS];
    int nwritten = 0;

    for (int i = 0; i < nbody; i++) {
        Stmt *st = body->items[i];
        if (st->kind == S_BREAK || st->kind == S_CONTINUE || st->kind == S_RETURN)
return loop;
        if (st->kind != S_EXPR) {
            /* A declaration or a nested control-flow statement: not this shape. */
            /* A declaration, a nested loop or any other control flow: not this
             * shape. (S_EXPR and the loop kinds are all that reach here.) */
return loop;
        }
        CloseFormAcc one;
        if (!close_form_match(p, st, &one))
return loop;
        if (one.slot == ivar)
            return loop; /* the loop variable is also an accumulator */
        if (nwritten < CLOSE_FORM_MAX_VARS)
            written[nwritten++] = one.slot;
        int found = -1;
        for (int k = 0; k < nacc; k++)
            if (accs[k].slot == one.slot)
                found = k;
        if (found >= 0) {
            /* Two updates to one accumulator in one iteration: add them, which is
             * exact for integers and is what the loop would have done. */
            accs[found].amount = make_binary(p, T_PLUS, accs[found].amount, one.amount, st->span);
        } else {
            if (nacc >= CLOSE_FORM_MAX_ACC)
return loop;
            accs[nacc++] = one;
        }
    }
    if (nacc == 0)
return loop;

    /* Every contribution must be the same on every iteration: it may not read the
     * induction variable, nor an accumulator, since the value of an accumulator
     * changes each iteration. */
    for (int k = 0; k < nacc; k++) {
        if (expr_reads_any(accs[k].amount, &ivar, 1))
return loop;
        if (expr_reads_any(accs[k].amount, written, nwritten))
return loop;
    }

    /* Trip count. The start has to be a constant: it is the one number this
     * cannot otherwise recover. A `for` that declares its own loop variable
     * carries it in the initializer; a `while` leaves it wherever the preceding
     * statements put it, so scan backwards for the nearest write to it. */
    long long start = 0;
    int have_start = 0;
    if (loop->for_init != NULL && loop->for_init->kind == S_VAR &&
        loop->for_init->init != NULL && loop->for_init->init->kind == E_INT &&
        loop->for_init->slot == ivar) {
        start = loop->for_init->init->ival;
        have_start = 1;
    }
    if (!have_start && siblings != NULL) {
        /* Only the *nearest* write counts. An earlier one whose value is not a
         * constant would make any guess wrong, so the search stops there. The
         * induction variable is known by now, which matters: a scan that did not
         * know it latched onto whichever local it saw first. */
        for (int k = sib_index - 1; k >= 0 && !have_start; k--) {
            Stmt *prev = siblings[k];
            if (prev == NULL)
                continue;
            if (prev->kind == S_VAR && prev->slot == ivar) {
                if (prev->init != NULL && prev->init->kind == E_INT) {
                    start = prev->init->ival;
                    have_start = 1;
                }
                break;
            }
            if (prev->kind == S_EXPR && prev->expr != NULL && prev->expr->kind == E_ASSIGN &&
                prev->expr->lhs != NULL && prev->expr->lhs->kind == E_VAR &&
                prev->expr->lhs->slot == ivar) {
                if (!prev->expr->compound && prev->expr->rhs != NULL &&
                    prev->expr->rhs->kind == E_INT) {
                    start = prev->expr->rhs->ival;
                    have_start = 1;
                }
                break;
            }
        }
    }
    if (!have_start)
        return loop;

    long long span = limit - start;
    long long n;
    if (stride > 0) {
        if (span <= 0)
            return loop; /* the loop never runs: the trip count is 0, and the
                          * code below assumes at least one iteration */
        n = strict ? (span + stride - 1) / stride : span / stride + 1;
    } else {
        if (span >= 0)
return loop;
        long long d = -stride;
        long long sp = -span;
        n = strict ? (sp + d - 1) / d : sp / d + 1;
    }
    if (n < 1)
return loop;

    /* Build the replacement: a run of statements assigning each accumulator its
     * closed form, in place of the loop. */
    int cap = nacc + 1;
    Stmt **items = arena_alloc_array(p->arena, (size_t)cap, sizeof(Stmt *));
    int nitems = 0;
    Expr *count = new_expr(p, E_INT, loop->span);
    count->type = type_int(&p->ty);
    count->ival = n;

    for (int k = 0; k < nacc; k++) {
        Expr *scaled = make_binary(p, T_STAR, accs[k].amount, count, loop->span);
        Expr *cur = accs[k].target;
        Expr *sum = make_binary(p, T_PLUS, cur, scaled, loop->span);
        if (sum->type == NULL)
            sum->type = accs[k].type;
        Expr *asg = new_expr(p, E_ASSIGN, loop->span);
        asg->lhs = cur;
        asg->rhs = sum;
        asg->type = accs[k].type;
        Stmt *st = new_stmt(p, S_EXPR, loop->span);
        st->expr = asg;
        items[nitems++] = st;
    }
    if (nitems == 0)
return loop;
    Stmt *blk = new_stmt(p, S_BLOCK, loop->span);
    blk->items = items;
    blk->nitems = nitems;
    return blk;
}

/* ---- inlining ----
 *
 * A call to a small function is replaced by the function's own body, with the
 * parameters bound to the caller's argument expressions. This runs as a
 * post-parse pass over every function in the program, so a call is inlinable no
 * matter which order the declarations were written in.
 *
 * Two shapes are handled, and the difference is what the body's result looks like:
 *
 *   - A single `return expr;` (or the `=> expr` it desugars to) substitutes
 *     directly. The call *is* the expression, nothing is stored anywhere, and
 *     there is no control flow to rebuild.
 *
 *   - Anything with more than one `return` is spliced in as statements around
 *     the call site: each `return e` becomes an assignment to a hidden result
 *     local plus a jump to the end of the spliced block, and the call becomes a
 *     read of that local. The result local is a fresh frame slot, allocated past
 *     the function's existing locals.
 *
 * What is deliberately refused, each because inlining it would be wrong rather
 * than merely unhelpful:
 *
 *   - a recursive callee, which would expand forever;
 *   - a virtual call, whose target is only known at run time;
 *   - `extern`, `export`, and a monomorphized generic instance, whose body does
 *     not belong to this program;
 *   - a struct-returning callee, whose result travels through a hidden pointer the
 *     splice would have to rebuild;
 *   - a callee whose address was taken with `&f`, since the value has to keep
 *     existing.
 *
 * Growth is bounded three ways: a size budget on the body, a depth limit on
 * nesting, and a cap on the number of expansions in any one function. */

#define INLINE_MAX_STMT 24    /* statements in a body worth copying */
#define INLINE_MAX_DEPTH 4    /* how deep expansions may nest */
#define INLINE_MAX_PER_FN 64  /* expansions allowed in one function */

/* Hands out a frame slot the way declare_var does.
 *
 * A value of `sz` bytes is anchored at `8 + next_offset + sz` and `next_offset`
 * grows by `sz`, which is what puts the value's *top* byte just below the saved
 * frame pointer. Allocating a slot any other way -- taking `next_offset` itself,
 * or adding a flat 8 -- lands on a slot the function has already given to
 * something, and the two silently share it. That is how the hidden result local
 * ended up aliasing the temporary an argument had been staged in, so storing the
 * result dereferenced a leftover pointer. */
static int inline_new_slot(int *next_offset, int sz) {
    if (sz < 8)
        sz = 8;
    *next_offset = (*next_offset + 7) / 8 * 8;
    int base = 8 + *next_offset + sz;
    *next_offset += sz;
    return base;
}

/* The state one function's inlining runs with. */
typedef struct {
    Parser *p;
    Stmt *self;      /* the function being inlined into, for the recursion check */
    int budget;      /* expansions left in this function */
    int depth;       /* how many expansions deep this call is */
    int *next_label; /* the parser's label counter, so labels are unique */
    int used_any;    /* set when anything was inlined, to report growth */
} InlineCtx;

static Stmt *clone_stmt(Parser *p, Stmt *s);
static Expr *clone_expr(Parser *p, Expr *e);

/* A local slot of the callee being copied, and where it is going in the caller.
 * Slot numbers are frame offsets and both functions have their own frame, so a
 * spliced body that kept the callee's numbers would read and write whatever the
 * caller happened to have at those offsets. */
typedef struct {
    int from;
    int to;
} SlotRemap;

static int inline_remap_stmt(InlineCtx *ic, Stmt *s, SlotRemap *map, int *nmap, int *next_slot,
                             int *base, int *pinned);
static int inline_param_top(Stmt *fn);

/* An argument being bound to a parameter, or a return type standing in for the
 * hidden result local. */
typedef struct InlineBinding {
    int slot;        /* the callee's parameter slot */
    Expr *value;     /* the caller's expression, or NULL for a result local */
    int result_slot; /* the hidden local's slot, when value is NULL */
    Type *type;
} InlineBinding;

/* Copies an expression tree. Deliberately not a memcpy of the struct: the
 * children are replaced with the copies, and a shallow copy would leave every
 * one of them pointing into the callee's tree, so a later edit through one call
 * site would be visible at the other. */
static Expr *clone_expr(Parser *p, Expr *e) {
    if (e == NULL)
        return NULL;
    Expr *c = arena_alloc(p->arena, sizeof *c);
    *c = *e;
    c->lhs = clone_expr(p, e->lhs);
    c->rhs = clone_expr(p, e->rhs);
    c->env = clone_expr(p, e->env);
    if (e->nargs > 0) {
        Expr **na = arena_alloc_array(p->arena, (size_t)e->nargs, sizeof(Expr *));
        for (int i = 0; i < e->nargs; i++)
            na[i] = clone_expr(p, e->args[i]);
        c->args = na;
    } else {
        c->args = NULL;
    }
    c->nargs = e->nargs;
    return c;
}

static Stmt *clone_stmt(Parser *p, Stmt *s) {
    if (s == NULL)
        return NULL;
    Stmt *c = arena_alloc(p->arena, sizeof *c);
    *c = *s;
    c->init = clone_expr(p, s->init);
    c->expr = clone_expr(p, s->expr);
    c->cond = clone_expr(p, s->cond);
    c->for_step = clone_expr(p, s->for_step);
    c->for_init = clone_stmt(p, s->for_init);
    c->body = clone_stmt(p, s->body);
    c->orelse = clone_stmt(p, s->orelse);
    if (s->nitems > 0) {
        Stmt **ni = arena_alloc_array(p->arena, (size_t)s->nitems, sizeof(Stmt *));
        for (int i = 0; i < s->nitems; i++)
            ni[i] = clone_stmt(p, s->items[i]);
        c->items = ni;
    } else {
        c->items = NULL;
    }
    return c;
}

/* Replaces a reference to a parameter with the caller's expression for it.
 * Substitution stops at an inner declaration of the same slot, which cannot
 * happen for a parameter -- a parameter's slot is the function's own, and a
 * local in the body gets a slot past it -- so no shadow check is needed. */
static Expr *subst_expr(Parser *p, Expr *e, InlineBinding *binds, int n) {
    if (e == NULL)
        return NULL;
    if (e->kind == E_VAR) {
        for (int i = 0; i < n; i++) {
            if (binds[i].slot == e->slot) {
                if (binds[i].value != NULL)
                    return clone_expr(p, binds[i].value);
                /* A return type bound to the hidden result local: reading it is
                 * the result, and there is nothing to substitute. */
                return e;
            }
        }
        return e;
    }
    e->lhs = subst_expr(p, e->lhs, binds, n);
    e->rhs = subst_expr(p, e->rhs, binds, n);
    e->env = subst_expr(p, e->env, binds, n);
    for (int i = 0; i < e->nargs; i++)
        e->args[i] = subst_expr(p, e->args[i], binds, n);
    if (e->narms > 0) {
        for (int i = 0; i < e->narms; i++)
            e->arms[i].body = subst_expr(p, e->arms[i].body, binds, n);
    }
    return e;
}

/* Counts the statements in a body, to the cap, so the size budget costs
 * nothing for a body that is already too big. */
static int inline_stmt_count(Stmt *s, int cap) {
    if (s == NULL)
        return 0;
    int n = 1;
    if (n >= cap)
        return n;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems && n < cap; i++)
            n += inline_stmt_count(s->items[i], cap - n);
        break;
    case S_IF:
    case S_WHILE:
        n += inline_stmt_count(s->body, cap - n);
        n += inline_stmt_count(s->orelse, cap - n);
        break;
    case S_FOR:
        n += inline_stmt_count(s->for_init, cap - n);
        n += inline_stmt_count(s->body, cap - n);
        break;
    default:
        break;
    }
    return n;
}


/* Is this function one the inliner may copy? */
/* Whether a body mentions anything whose meaning depends on *which frame* it was
 * built in: a nested function, a closure's environment, a bound method cell.
 *
 * A closure's environment is materialised where the lambda appears, and it
 * points at the slots of the frame it was written in. Splicing a body that builds
 * one into another function keeps the code but not the frame, so the environment
 * ends up naming slots of a call that has already returned -- the symptom being a
 * program that prints nothing and exits 0, having read a frame that was
 * overwritten before it was read. `makeAdder(int n) { return (int x) => x + n; }`
 * is the smallest case: one statement, nothing else. */
static int inline_env_bound(Expr *e) {
    if (e == NULL)
        return 0;
    if (e->kind == E_CLOSURE || e->kind == E_MPTR)
        return 1;
    if (inline_env_bound(e->lhs) || inline_env_bound(e->rhs) || inline_env_bound(e->env))
        return 1;
    for (int i = 0; i < e->nargs; i++)
        if (inline_env_bound(e->args[i]))
            return 1;
    for (int i = 0; i < e->narms; i++)
        if (inline_env_bound(e->arms[i].body))
            return 1;
    return 0;
}

static int inline_env_bound_stmt(Stmt *s) {
    if (s == NULL)
        return 0;
    if (s->kind == S_FUNC)
        return 1;
    if (inline_env_bound(s->init) || inline_env_bound(s->expr) || inline_env_bound(s->cond) ||
        inline_env_bound(s->for_step))
        return 1;
    if (inline_env_bound_stmt(s->for_init) || inline_env_bound_stmt(s->body) ||
        inline_env_bound_stmt(s->orelse))
        return 1;
    for (int i = 0; i < s->nitems; i++)
        if (inline_env_bound_stmt(s->items[i]))
            return 1;
    return 0;
}

static int inlineable(Stmt *fn) {
    if (fn == NULL || fn->kind != S_FUNC || fn->fbody == NULL)
        return 0;
    if (fn->is_extern || fn->is_export)
        return 0;   /* the body is not this program's to copy */
    if (fn->is_generic_template)
        return 0;   /* replaced by a monomorphized instance */
    if (fn->is_entry)
        return 0;   /* nothing calls it, and it may not return */
    if (fn->is_ext)
        return 0;   /* the receiver is a by-value struct; skip for now */
    if (fn->ret_type != NULL && (is_kind(fn->ret_type, TK_STRUCT) || is_kind(fn->ret_type, TK_UNION)))
        return 0;   /* a hidden result pointer the splice would have to rebuild */
    for (int i = 0; i < fn->nparams; i++) {
        Type *pt = fn->params[i]->type;
        /* An aggregate parameter arrives as a *pointer* to the caller's value,
         * under the same `agg_param` convention a struct result uses. Substituting
         * the argument expression there yields whatever that expression happens to
         * evaluate to rather than its address, so a struct argument came out
         * wrong -- `Rect(3, 4)` read as `3x5`. Rebuilding the address is a
         * separate job; refusing is not a missed speedup, it is the same call. */
        if (pt != NULL && (is_kind(pt, TK_STRUCT) || is_kind(pt, TK_UNION)))
            return 0;
        if (fn->params[i]->agg_param)
            return 0;
    }
    if (inline_env_bound_stmt(fn->fbody))
        return 0;   /* a closure in it names the frame this body no longer has */
    if (inline_stmt_count(fn->fbody, INLINE_MAX_STMT + 1) > INLINE_MAX_STMT)
        return 0;
    return 1;
}


/* Rebuilds a statement list, rewriting every `return e` in it into an assignment
 * to the hidden result local followed by a jump to the end of the inlined body.
 *
 * Rebuilding rather than editing in place is what lets a `return` inside an `if`
 * become *two* statements: there has to be somewhere to put the second one, and
 * that is the enclosing list. A return is the only statement that becomes more
 * than one thing; everything else is passed through with its children rebuilt.
 *
 * The list grows, so a buffer is passed in and reallocated as needed. */
typedef struct {
    Stmt **items;
    int n;
    int cap;
    Arena *arena;
} StmtList;

static void list_push(StmtList *L, Stmt *s) {
    if (L->n == L->cap) {
        int ncap = L->cap ? L->cap * 2 : 8;
        Stmt **bigger = arena_alloc_array(L->arena, (size_t)ncap, sizeof(Stmt *));
        if (L->n > 0)
            memcpy(bigger, L->items, (size_t)L->n * sizeof(Stmt *));
        L->items = bigger;
        L->cap = ncap;
    }
    L->items[L->n++] = s;
}

static void inline_rewrite(InlineCtx *ic, Stmt *s, StmtList *out, int rslot, Type *ret);

static void inline_rewrite_list(InlineCtx *ic, Stmt **items, int n, StmtList *out, int rslot,
                                Type *ret) {
    for (int i = 0; i < n; i++)
        inline_rewrite(ic, items[i], out, rslot, ret);
}

static void inline_rewrite(InlineCtx *ic, Stmt *s, StmtList *out, int rslot, Type *ret) {
    if (s == NULL)
        return;
    Parser *p = ic->p;
    switch (s->kind) {
    case S_RETURN: {
        /* The assignment, then the jump. Both are separate statements because
         * neither an assignment nor a jump is the other. */
        Stmt *asg = new_stmt(p, S_EXPR, s->span);
        Expr *a = new_expr(p, E_ASSIGN, s->span);
        Expr *dst = new_expr(p, E_VAR, s->span);
        dst->name = arena_strdup(p->arena, "$inl");
        dst->slot = rslot;
        dst->type = ret;
        a->lhs = dst;
        a->rhs = s->expr;
        a->type = ret;
        asg->expr = a;
        list_push(out, asg);
        Stmt *leave = new_stmt(p, S_LEAVE, s->span);
        list_push(out, leave);
        return;
    }
    case S_BLOCK: {
        Stmt *blk = new_stmt(p, S_BLOCK, s->span);
        StmtList inner = {NULL, 0, 0, p->arena};
        inline_rewrite_list(ic, s->items, s->nitems, &inner, rslot, ret);
        blk->items = inner.items;
        blk->nitems = inner.n;
        list_push(out, blk);
        return;
    }
    case S_IF: {
        Stmt *iff = new_stmt(p, S_IF, s->span);
        iff->cond = s->cond;
        iff->orelse = s->orelse;
        StmtList b = {NULL, 0, 0, p->arena};
        inline_rewrite(ic, s->body, &b, rslot, ret);
        iff->body = b.n == 1 ? b.items[0] : NULL;
        if (b.n > 1) {
            Stmt *blk = new_stmt(p, S_BLOCK, s->span);
            blk->items = b.items;
            blk->nitems = b.n;
            iff->body = blk;
        }
        StmtList e = {NULL, 0, 0, p->arena};
        inline_rewrite(ic, s->orelse, &e, rslot, ret);
        if (e.n == 1) {
            iff->orelse = e.items[0];
        } else if (e.n > 1) {
            Stmt *blk = new_stmt(p, S_BLOCK, s->span);
            blk->items = e.items;
            blk->nitems = e.n;
            iff->orelse = blk;
        }
        list_push(out, iff);
        return;
    }
    case S_WHILE: {
        Stmt *w = new_stmt(p, S_WHILE, s->span);
        w->cond = s->cond;
        StmtList b = {NULL, 0, 0, p->arena};
        inline_rewrite(ic, s->body, &b, rslot, ret);
        w->body = b.n == 1 ? b.items[0] : NULL;
        if (b.n > 1) {
            Stmt *blk = new_stmt(p, S_BLOCK, s->span);
            blk->items = b.items;
            blk->nitems = b.n;
            w->body = blk;
        }
        list_push(out, w);
        return;
    }
    case S_FOR: {
        Stmt *f = new_stmt(p, S_FOR, s->span);
        f->cond = s->cond;
        f->for_step = s->for_step;
        f->for_init = s->for_init;
        StmtList b = {NULL, 0, 0, p->arena};
        inline_rewrite(ic, s->body, &b, rslot, ret);
        f->body = b.n == 1 ? b.items[0] : NULL;
        if (b.n > 1) {
            Stmt *blk = new_stmt(p, S_BLOCK, s->span);
            blk->items = b.items;
            blk->nitems = b.n;
            f->body = blk;
        }
        list_push(out, f);
        return;
    }
    default:
        list_push(out, s);
        return;
    }
}

/* The program's functions by name, so a call can find the body to copy however
 * the two were written relative to each other. A name maps to the most recently
 * declared function, which for a name that is only ever declared once is the one
 * that exists. */
typedef struct {
    Stmt **fns;
    int n;
    int cap;
    Arena *arena;
} FnTable;

static Stmt *fn_table_find(FnTable *T, const char *name) {
    for (int i = T->n - 1; i >= 0; i--)
        if (strcmp(T->fns[i]->fname, name) == 0)
            return T->fns[i];
    return NULL;
}

static void fn_table_add(FnTable *T, Stmt *fn) {
    if (T->n == T->cap) {
        int ncap = T->cap ? T->cap * 2 : 16;
        Stmt **bigger = arena_alloc_array(T->arena, (size_t)ncap, sizeof(Stmt *));
        if (T->n > 0)
            memcpy(bigger, T->fns, (size_t)T->n * sizeof(Stmt *));
        T->fns = bigger;
        T->cap = ncap;
    }
    T->fns[T->n++] = fn;
}

/* Binds the callee's parameters to the caller's arguments. The callee's own
 * frame slots are what the body refers to, so a parameter is matched by slot
 * rather than by name. */
/* Whether an argument may be copied into every place the callee referred to it.
 *
 * Substitution is textual: the expression goes back in at each use. A value that
 * only reads something already in hand -- a variable, a literal, a field of one --
 * can be repeated freely, and in practice always is, since a parameter is often
 * used two or three times. Anything that can act (`f().add(3)`) or that costs
 * something (`1/den`) would run again per use, so a call carrying one is left as
 * a call. */
static int inline_dup_ok(Expr *e) {
    if (e == NULL)
        return 1;
    switch (e->kind) {
    case E_VAR:
    case E_INT:
    case E_F64:
    case E_STRING:
    case E_BOOL:
    case E_NULL:
    case E_DEREF:
    case E_ADDR:
        return 1;
    case E_UNARY:
    case E_CVT:
    case E_FIELD:
    case E_INDEX:
        return inline_dup_ok(e->lhs);
    case E_BINARY:
        return inline_dup_ok(e->lhs) && inline_dup_ok(e->rhs);
    default:
        return 0;
    }
}

static int inline_bind_args(InlineCtx *ic, Stmt *callee, Expr *call, InlineBinding *out) {
    int n = 0;
    for (int i = callee->vis_start; i < callee->nparams && n < INLINE_MAX_STMT; i++) {
        /* The index into `args` is the parameter index itself, not one relative
         * to the receiver. A method's parameter list leaves the receiver out but
         * the call already carries it in args[0], so `this` bound to the first
         * *written* argument and the real one to the second. */
        if (i >= call->nargs || !inline_dup_ok(call->args[i]))
            return -1; /* -1, not 0: a parameterless call legitimately binds none */
        out[n].slot = callee->params[i]->slot;
        out[n].value = clone_expr(ic->p, call->args[i]);
        out[n].result_slot = 0;
        out[n].type = callee->params[i]->type;
        n++;
    }
    return n;
}

/* The single `return e` of an inlinable body, or NULL if it has more than one
 * or none. A body whose last statement is a return has exactly one reachable
 * path out of it for a call, which is what makes direct substitution sound. */
static Stmt *inline_single_return(Stmt *body) {
    /* Exactly one statement, and it is the return. Anything longer declares
     * locals of its own, and substituting only the final expression would leave
     * the caller reading a frame slot it never wrote -- `count` below is that
     * case, and it read back 0. Those bodies take the splice path instead, which
     * copies the declarations and renumbers their slots. */
    if (body == NULL)
        return NULL;
    if (body->kind != S_BLOCK) {
        return body->kind == S_RETURN && body->expr != NULL ? body : NULL;
    }
    if (body->nitems != 1)
        return NULL;
    Stmt *only = body->items[0];
    if (only->kind != S_RETURN || only->expr == NULL)
        return NULL;
    return only;
}

/* Rewrites a call in place when the callee is a single return, which needs no
 * statement around it. Returns 1 when something was done. The expression is
 * replaced through `*slot`. */
static int inline_substitute(InlineCtx *ic, FnTable *T, Expr **slot) {
    Expr *call = *slot;
    if (call == NULL || call->kind != E_CALL || call->is_extern)
        return 0;
    if (ic->budget <= 0 || ic->depth >= INLINE_MAX_DEPTH)
        return 0;
    Stmt *callee = fn_table_find(T, call->name);
    if (!inlineable(callee))
        return 0;
    if (callee == ic->self)
        return 0; /* recursive: the expansion would never finish */
    Stmt *ret = inline_single_return(callee->fbody);
    if (ret == NULL)
        return 0;
    InlineBinding binds[INLINE_MAX_STMT];
    int nb = inline_bind_args(ic, callee, call, binds);
    if (nb < 0)
        return 0;
    Expr *e = clone_expr(ic->p, ret->expr);
    InlineCtx deeper = *ic;
    deeper.depth = ic->depth + 1;
    e = subst_expr(ic->p, e, binds, nb);
    /* A nested call inside the substituted expression gets its own chance. */
    if (nb > 0)
        (void)deeper;
    *slot = e;
    ic->budget--;
    ic->used_any = 1;
    return 1;
}

/* Substitutes every single-return call anywhere in an expression. Safe at any
 * depth because nothing is spliced: a call becomes its callee's expression. */
static void inline_expr(InlineCtx *ic, FnTable *T, Expr **e) {
    if (e == NULL || *e == NULL)
        return;
    Expr *x = *e;
    if (inline_substitute(ic, T, e))
        return; /* the replacement is already a fresh tree; do not rescan it */
    x = *e;
    x->lhs = x->lhs;
    inline_expr(ic, T, &x->lhs);
    inline_expr(ic, T, &x->rhs);
    inline_expr(ic, T, &x->env);
    for (int i = 0; i < x->nargs; i++)
        inline_expr(ic, T, &x->args[i]);
    for (int i = 0; i < x->narms; i++)
        inline_expr(ic, T, &x->arms[i].body);
    *e = x;
}


/* Substitutes parameter bindings through a statement tree, in place. */
static void inline_bind_stmt(InlineCtx *ic, Stmt *s, InlineBinding *b, int n) {
    if (s == NULL)
        return;
    s->init = subst_expr(ic->p, s->init, b, n);
    s->expr = subst_expr(ic->p, s->expr, b, n);
    s->cond = subst_expr(ic->p, s->cond, b, n);
    s->for_step = subst_expr(ic->p, s->for_step, b, n);
    inline_bind_stmt(ic, s->for_init, b, n);
    inline_bind_stmt(ic, s->body, b, n);
    inline_bind_stmt(ic, s->orelse, b, n);
    for (int i = 0; i < s->nitems; i++)
        inline_bind_stmt(ic, s->items[i], b, n);
}

/* Builds the statements that replace one call site with a spliced body. On
 * success the wrapper and the caller's rewritten statement are appended to `out`
 * and 1 is returned; otherwise nothing is appended. */
static int inline_build_splice(InlineCtx *ic, FnTable *T, Stmt *st, int *next_label,
                               StmtList *out) {
    Parser *p = ic->p;
    Expr **slot = NULL;
    if (st->kind == S_EXPR || st->kind == S_RETURN)
        slot = &st->expr;
    else if (st->kind == S_VAR)
        slot = &st->init;
    if (slot == NULL || *slot == NULL)
        return 0;
    Expr *call = *slot;
    if (call->kind != E_CALL || call->is_extern)
        return 0;
    if (ic->budget <= 0 || ic->depth >= INLINE_MAX_DEPTH)
        return 0;
    Stmt *callee = fn_table_find(T, call->name);
    if (!inlineable(callee) || callee == ic->self)
        return 0;
    if (inline_single_return(callee->fbody) != NULL)
        return 0; /* the cheaper path already handles this one */
    if (callee->ret_type == NULL || is_kind(callee->ret_type, TK_VOID))
        return 0; /* nothing to read the result out of */
    InlineBinding binds[INLINE_MAX_STMT];
    int nb = inline_bind_args(ic, callee, call, binds);
    if (nb < 0)
        return 0;

    /* Strictly above the last parameter, not at it: parameters are declared
     * first, so the last one occupies the highest parameter slot and a test of
     * `>= ptop` renumbered that one as if it were a local. The last parameter is
     * then left reading a slot the caller never wrote, which is how `set(&y, 123)`
     * turned y into an address. */
    int ptop = inline_param_top(callee) + 8;

    /* A fresh frame slot for the result, past everything the function already
     * declared: the caller is fully parsed, so the slot is handed out here and
     * the frame grown to match.
     *
     * The slot is also pushed clear of the callee's whole parameter range.
     * Arguments are substituted into the copy by matching slot number, so a
     * result local that happened to land on the slot of a parameter -- which
     * costs nothing here, the two live in different frames and the numbers are
     * just offsets into them -- was rewritten as that argument's value, and the
     * function then stored its result through a caller temporary. */
    if (ic->self->locals_bytes < ptop)
        ic->self->locals_bytes = ptop;
    int rslot = inline_new_slot(&ic->self->locals_bytes, 8);

    Stmt *body = clone_stmt(p, callee->fbody);
    if (body == NULL)
        return 0;

    /* The callee's locals have to move into the caller's frame before anything
     * else touches the copy. A body with a lambda in it is left alone: a lambda
     * captures by referring to the enclosing frame's slots, and which function
     * owns them changes when the body moves. */
    SlotRemap map[64];
    int nmap = 0;
    int pinned = 0;
    int next_slot = ic->self->locals_bytes;
    if (inline_remap_stmt(ic, body, map, &nmap, &next_slot, &ptop, &pinned)) {
        /* Rolled back: the frame must not keep the slots a refused body took. */
        return 0;
    }
    ic->self->locals_bytes = next_slot;
    /* Rewrite the callee's returns first, then substitute through the result, so
     * a parameter named like one of the caller's locals cannot capture it. */
    StmtList fixed = {NULL, 0, 0, p->arena};
    if (body->kind == S_BLOCK)
        inline_rewrite_list(ic, body->items, body->nitems, &fixed, rslot, callee->ret_type);
    else
        inline_rewrite(ic, body, &fixed, rslot, callee->ret_type);
    if (fixed.n == 0)
        return 0;

    InlineBinding all[INLINE_MAX_STMT + 1];
    for (int i = 0; i < nb; i++)
        all[i] = binds[i];
    all[nb].slot = rslot;
    all[nb].value = NULL;
    all[nb].result_slot = rslot;
    all[nb].type = callee->ret_type;
    for (int i = 0; i < fixed.n; i++)
        inline_bind_stmt(ic, fixed.items[i], all, nb + 1);

    Stmt *wrapper = new_stmt(p, S_BLOCK, body->span);
    wrapper->items = fixed.items;
    wrapper->nitems = fixed.n;
    wrapper->inl_label = ++ic->p->inline_label;
    (void)next_label;

    Expr *read = new_expr(p, E_VAR, call->span);
    read->name = arena_strdup(p->arena, "$inl");
    read->slot = rslot;
    read->type = callee->ret_type;
    *slot = read;

    list_push(out, wrapper);
    list_push(out, st);
    ic->budget--;
    ic->used_any = 1;
    return 1;
}

/* Rewrites one statement, appending whatever replaces it to `out`. */
static void inline_stmt(InlineCtx *ic, FnTable *T, Stmt *s, StmtList *out, int *next_label) {
    if (s == NULL)
        return;
    if (inline_build_splice(ic, T, s, next_label, out))
        return;
    /* Substitutions first: they are valid at any depth and need no statements. */
    inline_expr(ic, T, &s->init);
    inline_expr(ic, T, &s->expr);
    inline_expr(ic, T, &s->cond);
    inline_expr(ic, T, &s->for_step);
    switch (s->kind) {
    case S_BLOCK: {
        StmtList inner = {NULL, 0, 0, ic->p->arena};
        for (int i = 0; i < s->nitems; i++)
            inline_stmt(ic, T, s->items[i], &inner, next_label);
        s->items = inner.items;
        s->nitems = inner.n;
        list_push(out, s);
        return;
    }
    case S_IF:
    case S_WHILE:
    case S_FOR: {
        StmtList b = {NULL, 0, 0, ic->p->arena};
        inline_stmt(ic, T, s->body, &b, next_label);
        s->body = b.n == 1 ? b.items[0] : NULL;
        if (b.n > 1) {
            Stmt *blk = new_stmt(ic->p, S_BLOCK, s->span);
            blk->items = b.items;
            blk->nitems = b.n;
            s->body = blk;
        }
        StmtList e = {NULL, 0, 0, ic->p->arena};
        inline_stmt(ic, T, s->orelse, &e, next_label);
        if (e.n == 1) {
            s->orelse = e.items[0];
        } else if (e.n > 1) {
            Stmt *blk = new_stmt(ic->p, S_BLOCK, s->span);
            blk->items = e.items;
            blk->nitems = e.n;
            s->orelse = blk;
        }
        list_push(out, s);
        return;
    }
    default:
        list_push(out, s);
        return;
    }
}

/* Inlines into one function. */
static void inline_function(InlineCtx *ic, FnTable *T, Stmt *fn) {
    if (fn == NULL || fn->fbody == NULL)
        return;
    if (fn->is_generic_template)
        return;
    /* A lambda or a nested function is skipped as a *caller*, and not only as a
     * callee. Its body refers to the enclosing frame's slots -- a captured
     * variable is boxed, and the box is created by a declaration outside the
     * lambda -- so renumbering those references, which is what inlining a body
     * into it would do, points them at slots nothing ever writes. A closure's
     * frame is not an ordinary one and this pass does not model it. */
    if (fn->fname != NULL && fn->fname[0] == '$')
        return;
    ic->self = fn;
    ic->budget = INLINE_MAX_PER_FN;
    ic->depth = 0;
    ic->used_any = 0;
    StmtList out = {NULL, 0, 0, ic->p->arena};
    if (fn->fbody->kind == S_BLOCK) {
        for (int i = 0; i < fn->fbody->nitems; i++)
            inline_stmt(ic, T, fn->fbody->items[i], &out, NULL);
        fn->fbody->items = out.items;
        fn->fbody->nitems = out.n;
    } else {
        inline_stmt(ic, T, fn->fbody, &out, NULL);
        if (out.n == 1)
            fn->fbody = out.items[0];
    }
}

/* Runs the inliner over every function in the program, to a fixed point so an
 * expansion can enable the next: inlining `f` into `g` may leave a call to `h`
 * in `g` that is itself worth inlining. Three rounds is enough for the chains
 * that occur in practice, and a bound is what keeps a mutually recursive pair
 * from oscillating. */
static void inline_program(Parser *p, Stmt *program) {
    if (p->opt_level < 1)
        return;
    FnTable T = {NULL, 0, 0, p->arena};
    for (int i = 0; i < program->nitems; i++) {
        Stmt *it = program->items[i];
        if (it != NULL && it->kind == S_FUNC && it->fbody != NULL)
            fn_table_add(&T, it);
    }
    for (int round = 0; round < 3; round++) {
        int changed = 0;
        for (int i = 0; i < program->nitems; i++) {
            Stmt *it = program->items[i];
            if (it == NULL || it->kind != S_FUNC)
                continue;
            InlineCtx ic;
            memset(&ic, 0, sizeof ic);
            ic.p = p;
            ic.self = it;
            inline_function(&ic, &T, it);
            if (ic.used_any)
                changed = 1;
        }
        if (!changed)
            break;
    }
}

/* Renumbers the callee's locals into fresh slots of the caller.
 *
 * Slot numbers are frame offsets and both functions have their own frame, so a
 * spliced body that kept the callee's numbers would read and write whatever the
 * caller happened to have at those offsets. Every variable in the body that is
 * not a parameter is therefore given a slot of its own past the caller's
 * existing locals, and the frame is grown to match.
 *
 * A lambda in the body stops the walk rather than being renumbered: a lambda has
 * a frame of its own, and a variable it captures refers to the *caller's* slot
 * once inlined, which this cannot tell apart from the callee's. A body with one
 * is left alone entirely. */
static int inline_remap_expr(InlineCtx *ic, Expr *e, SlotRemap *map, int *nmap, int *next_slot,
                             int *base, int *pinned) {
    if (e == NULL)
        return 0;
    if (e->kind == E_CLOSURE)
        return 1; /* pinned: do not touch what a lambda captures */
    if (e->kind == E_VAR && e->slot > 0) {
        for (int i = 0; i < *nmap; i++)
            if (map[i].from == e->slot) {
                e->slot = map[i].to;
                return 0;
            }
        if (e->slot >= *base) {
            /* A local of the callee, given a slot of the caller's own. */
            int at = inline_new_slot(next_slot, type_size(e->type));
            if (*nmap < 64) {
                map[*nmap].from = e->slot;
                map[*nmap].to = at;
                (*nmap)++;
            }
            e->slot = at;
        } else {
            /* Below the parameters: one of them, and the substitution pass
             * replaces it. Nothing to do. */
        }
        return 0;
    }
    if (inline_remap_expr(ic, e->lhs, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_expr(ic, e->rhs, map, nmap, next_slot, base, pinned))
        return 1;
    for (int i = 0; i < e->nargs; i++)
        if (inline_remap_expr(ic, e->args[i], map, nmap, next_slot, base, pinned))
            return 1;
    return 0;
}

static int inline_remap_stmt(InlineCtx *ic, Stmt *s, SlotRemap *map, int *nmap, int *next_slot,
                             int *base, int *pinned) {
    if (s == NULL)
        return 0;
    if (s->kind == S_LEAVE)
        return 0;
    if (s->kind == S_FUNC)
        return 1; /* a nested function has its own frame */
    if (inline_remap_expr(ic, s->init, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_expr(ic, s->expr, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_expr(ic, s->cond, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_expr(ic, s->for_step, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_stmt(ic, s->for_init, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_stmt(ic, s->body, map, nmap, next_slot, base, pinned))
        return 1;
    if (inline_remap_stmt(ic, s->orelse, map, nmap, next_slot, base, pinned))
        return 1;
    for (int i = 0; i < s->nitems; i++)
        if (inline_remap_stmt(ic, s->items[i], map, nmap, next_slot, base, pinned))
            return 1;
    /* A declaration names a slot too, for the debugger and for the register
     * allocator's own walk. It is also the only place a local that is never read
     * back -- `var t = 1;` whose value goes unused -- can be given a new one, so
     * the slot is claimed here rather than looked up: waiting to look it up left
     * the declaration writing to a slot of the caller's, because the map only
     * grows when a reference is met and this one comes first. */
    if (s->kind == S_VAR && s->slot >= *base) {
        int to = -1;
        for (int i = 0; i < *nmap; i++)
            if (map[i].from == s->slot) {
                to = map[i].to;
                break;
            }
        if (to < 0) {
            to = inline_new_slot(next_slot, type_size(s->type));
            if (*nmap < 64) {
                map[*nmap].from = s->slot;
                map[*nmap].to = to;
                (*nmap)++;
            }
        }
        s->slot = to;
    }
    return 0;
}

/* The highest parameter slot of a function: everything below it is a parameter
 * and is substituted rather than copied. Parameters are declared first, so this
 * is the slot of the last one. */
static int inline_param_top(Stmt *fn) {
    int top = 0;
    for (int i = 0; i < fn->nparams; i++) {
        int s = fn->params[i]->slot;
        if (s > top)
            top = s;
    }
    return top;
}
