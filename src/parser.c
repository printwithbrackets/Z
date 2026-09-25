#include "parser.h"

#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A local variable binding in a lexical scope. */
typedef struct Var {
    char *name;
    Type *type;
    int offset;    /* byte offset from rbp within the function frame */
    int agg_param; /* parameter of struct/union type: slot holds a pointer */
    struct Var *next;
} Var;

typedef struct Scope {
    Var *vars;
    struct Scope *parent;
} Scope;

/* A function signature discovered by the pre-scan, used to resolve and check
 * calls (including forward references). */
typedef struct {
    char *name;
    Type *ret;
    Type **ptypes;
    int nparams;
    int is_ext;     /* extension method: first param is the receiver */
    Type *ext_recv; /* receiver type of an extension method */
} Sig;

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
    Stmt **pending; /* concrete instances awaiting append to program items */
    int npending, pending_cap;
    int in_instantiate; /* set while re-parsing a template for an instance */
} Parser;

/* Forward declarations. */
static Expr *parse_expr(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_block(Parser *p);
static Stmt *parse_func(Parser *p);
static Type *parse_type_at(Parser *p, int i);
static Expr *parse_match(Parser *p, Span start);

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
    case T_KW_STRING:
        return type_string(&p->ty);
    case T_KW_VOID:
        return type_void(&p->ty);
    default:
        return NULL;
    }
}

static int is_type_token(TokenKind k) {
    return k == T_KW_INT || k == T_KW_BOOL || k == T_KW_STRING || k == T_KW_VOID;
}

/* Parses a type possibly followed by `*` stars and `[]` array suffixes.
 * Used in declarations, parameters, and return positions. */
/* Resolves a type-name token: builtins, or a declared struct by name. */
static Type *base_type_or_name(Parser *p, Token *t) {
    Type *b = base_type_from_token(p, t->kind);
    if (b != NULL)
        return b;
    if (t->kind == T_IDENT) {
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

static Type *parse_type(Parser *p) {
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

static void scope_pop(Parser *p) {
    if (p->scope != NULL)
        p->scope = p->scope->parent;
}

static Var *lookup_var_local(Parser *p, const char *name) {
    for (Var *v = p->scope->vars; v != NULL; v = v->next) {
        if (strcmp(v->name, name) == 0)
            return v;
    }
    return NULL;
}

static Var *lookup_var(Parser *p, const char *name) {
    for (Scope *s = p->scope; s != NULL; s = s->parent) {
        for (Var *v = s->vars; v != NULL; v = v->next) {
            if (strcmp(v->name, name) == 0)
                return v;
        }
    }
    return NULL;
}

static int align_up(int n, int a) {
    if (a <= 0)
        return n;
    return (n + a - 1) / a * a;
}

static int declare_var(Parser *p, const char *name, Type *type) {
    /* Shadowing an outer variable in a nested block is legal (as in C#); only
     * redeclaring within the same scope is an error. */
    if (lookup_var_local(p, name) != NULL) {
        diag_error(cur(p)->span, "variable '%s' is already declared in this scope", name);
        return -1;
    }
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
    v->next = p->scope->vars;
    p->scope->vars = v;
    return base;
}

/* ---- function signature table (pre-scan) ---- */

static void add_sig(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                    int is_ext) {
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

/* Given a token index that begins a type, returns the index just past it. */
static int skip_type_tokens(Parser *p, int i) {
    if (i >= p->ntoks)
        return i;
    if (!is_type_token(p->toks[i].kind) && p->toks[i].kind != T_IDENT)
        return i;
    i++;
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
    int j = skip_type_tokens(p, p->pos);
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

/* Scans top-level tokens for function signatures so calls resolve regardless
 * of declaration order. */
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
    case T_EQ:
    case T_NE:
        return 3;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        return 4;
    case T_PLUS:
    case T_MINUS:
        return 5;
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
        return 6;
    default:
        return 0;
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
                        diag_error(span, "operator '%s' expects '%s' but got '%s'",
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
                  is_kind(lhs->type, TK_BOOL);
        int rok = is_kind(rhs->type, TK_STRING) || is_kind(rhs->type, TK_INT) ||
                  is_kind(rhs->type, TK_BOOL);
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
        int lok = is_kind(lhs->type, TK_INT) || is_kind(lhs->type, TK_BOOL) || lparam;
        int rok = is_kind(rhs->type, TK_INT) || is_kind(rhs->type, TK_BOOL) || rparam;
        if (!lok || !rok || !type_equals(lhs->type, rhs->type)) {
            diag_error(span, "cannot compare '%s' with '%s'", type_name(&p->ty, lhs->type),
                       type_name(&p->ty, rhs->type));
            e->type = NULL;
            return e;
        }
        e->type = type_bool(&p->ty);
        return e;
    }

    if ((!is_kind(lhs->type, TK_INT) && !lparam) || (!is_kind(rhs->type, TK_INT) && !rparam)) {
        diag_error(span, "operator '%s' is not defined for '%s' and '%s'", token_kind_name(op),
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
        p->pending_cap = p->pending_cap ? p->pending_cap * 2 : 8;
        p->pending = arena_alloc_array(p->arena, (size_t)p->pending_cap, sizeof(Stmt *));
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
    Stmt *inst = parse_func(p);
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
        if (!is_kind(at0, TK_INT) && !is_kind(at0, TK_BOOL) && !is_kind(at0, TK_STRING)) {
            diag_error(span, "print expects 'int', 'bool' or 'string' but got %s",
                       type_name(&p->ty, at0));
        }
        e->type = type_void(&p->ty);
        return e;
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

    /* User function. */
    Sig *s = find_sig(p, name);
    if (s == NULL) {
        diag_error(span, "undefined function '%s'", name);
        e->type = NULL;
        return e;
    }
    if (s->nparams != n) {
        diag_error(span, "'%s' expects %d argument%s but got %d", name, s->nparams,
                   s->nparams == 1 ? "" : "s", n);
        e->type = NULL;
        return e;
    }
    for (int i = 0; i < n; i++) {
        if (!is_unk(args[i]->type) && !is_unk(s->ptypes[i]) &&
            !type_equals(args[i]->type, s->ptypes[i])) {
            diag_error(args[i]->span, "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                       type_name(&p->ty, s->ptypes[i]), type_name(&p->ty, args[i]->type));
        }
    }
    e->type = s->ret;
    return e;
}

/* Postfix suffixes: indexing, .length, and postfix ++/--. */
static Expr *parse_postfix(Parser *p, Expr *e) {
    for (;;) {
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
            /* method call: recv.Name(args) */
            Type *st = e->type;
            if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
                st = st->base;
            if (at(p, T_LPAREN) && is_kind(st, TK_STRUCT)) {
                StructMethod *m = struct_find_method(st->sdef, name);
                if (m == NULL) {
                    diag_error(span, "type %s has no method '%s'", type_name(&p->ty, st), name);
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
                        diag_error(span, "type %s has no field '%s'", type_name(&p->ty, st), name);
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
                        diag_error(span, "type %s has no field '%s'", type_name(&p->ty, e->type),
                                   name);
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
                    diag_error(span, "operator '%s' requires 'int'", token_kind_name(op));
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
        } else {
            break;
        }
    }
    return e;
}

/* Parses a base type followed by `*` stars but NOT `[]` (used by `new T[n]`,
 * where the brackets are the element count). */
static Type *parse_type_base(Parser *p) {
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
            diag_error(start, "unknown type '%s'", sname);
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
        if (*cur == '{') {
            int litlen = (int)(cur - seg);
            Expr *lit = make_str_expr(p, seg, litlen, span);
            acc = acc ? make_binary(p, T_PLUS, acc, lit, span) : lit;
            const char *e = cur + 1;
            int depth = 1;
            while (*e && depth > 0) {
                if (*e == '{')
                    depth++;
                else if (*e == '}')
                    depth--;
                if (depth > 0)
                    e++;
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
    Expr *tail = make_str_expr(p, seg, litlen, span);
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
        advance(p);
        if (at(p, T_LPAREN)) {
            /* A bare variant name is a union constructor: `Circle(5)`. */
            UnionDef *owner = NULL;
            VariantDef *vd = type_find_variant(&p->ty, name, &owner);
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
                e->type = type_find_union(&p->ty, owner->name);
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
            diag_error(span, "undefined variable '%s'", name);
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
    if (t->kind == T_MINUS || t->kind == T_NOT) {
        advance(p);
        Expr *operand = parse_unary(p);
        /* Fold unary minus / logical-not on literals. */
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
        if (t->kind == T_MINUS) {
            if (!is_unk(operand->type) && !is_kind(operand->type, TK_INT)) {
                diag_error(t->span, "cannot negate '%s'", type_name(&p->ty, operand->type));
                e->type = NULL;
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
        Expr *operand = parse_unary(p);
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
           k == T_SLASH_EQ || k == T_PERCENT_EQ;
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
            if (!is_kind(lhs->type, TK_INT) || !is_kind(rhs->type, TK_INT)) {
                if (!is_unk(lhs->type) && !is_unk(rhs->type)) {
                    diag_error(span, "operator '%s=' requires 'int' operands",
                               token_kind_name(bop));
                    e->type = NULL;
                    return e;
                }
            }
            e->type = lhs->type;
        } else {
            if (!type_assignable(lhs->type, rhs->type) && !is_unk(lhs->type) &&
                !is_unk(rhs->type)) {
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
        Expr *init = parse_expr(p);
        Type *t = declared;
        if (infer)
            t = init->type;
        if (t == NULL)
            t = type_int(&p->ty);
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

static Stmt *parse_func(Parser *p) {
    int fn_tok_start = p->pos;
    Span start = cur(p)->span;

    /* Look ahead for a generic parameter list `name<T,...>(` and pre-bind the
     * type parameters so the return type and parameter types can reference them
     * even though the list is written after the name. */
    int ntparams = 0;
    char **tparams = NULL;
    int saved_ntbind = p->ntbind;
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
        diag_error(cur(p)->span, "unknown type '%s'",
                   cur(p)->kind == T_IDENT ? cur(p)->text : token_kind_name(cur(p)->kind));
    }
    char *name = cur(p)->text;
    advance(p); /* name */

    /* Consume the generic parameter list if present (already bound above). */
    if (ntparams > 0 && at(p, T_LT))
        p->pos = skip_generic_params(p, p->pos);
    match(p, T_LPAREN);

    Scope *saved_scope = p->scope;
    int saved_slot = p->next_offset;
    Type *saved_ret = p->cur_ret;
    scope_push(p);
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
    if (is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION)) {
        Expr **np = arena_alloc_array(p->arena, (size_t)(pn + 1), sizeof(Expr *));
        memcpy(np + 1, params, (size_t)pn * sizeof(Expr *));
        Expr *hidden = new_expr(p, E_VAR, start);
        hidden->name = arena_strdup(p->arena, "$ret");
        hidden->type = type_ptr(&p->ty, ret);
        hidden->slot = declare_var(p, "$ret", hidden->type);
        np[0] = hidden;
        params = np;
        pn++;
    }

    Stmt *fn = new_stmt(p, S_FUNC, start);
    fn->fname = name;
    fn->ret_type = ret;
    fn->params = params;
    fn->nparams = pn;
    fn->is_ext = is_ext;
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

    if (strcmp(name, "main") == 0)
        fn->is_entry = 1;
    return fn;
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

    if (!is_kind(coll->type, TK_ARRAY) && !is_kind(coll->type, TK_PTR)) {
        if (!is_unk(coll->type)) {
            diag_error(start, "foreach requires an array or pointer, got %s",
                       type_name(&p->ty, coll->type));
        }
        Stmt *dummy = parse_stmt(p);
        return dummy;
    }

    /* Loop variables live in their own scope. */
    scope_push(p);
    char hname[32];
    snprintf(hname, sizeof hname, "__arr%d", p->foreach_counter);
    int arr_slot = declare_var(p, hname, coll->type);
    snprintf(hname, sizeof hname, "__fi%d", p->foreach_counter);
    p->foreach_counter++;
    int idx_slot = declare_var(p, hname, type_int(&p->ty));
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

    Stmt *body = parse_stmt(p);
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

    int pcap = 4, pn = 0;
    Expr **params = arena_alloc_array(p->arena, (size_t)pcap, sizeof(Expr *));
    Expr *this_var = new_expr(p, E_VAR, start);
    this_var->name = arena_strdup(p->arena, "this");
    this_var->type = this_ty;
    this_var->slot = declare_var(p, "this", this_ty);
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

    /* Struct-returning methods also use the hidden-pointer convention; the
     * hidden buffer pointer precedes `this` as the first argument. */
    if (is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION)) {
        Expr **np = arena_alloc_array(p->arena, (size_t)(pn + 1), sizeof(Expr *));
        memcpy(np + 1, params, (size_t)pn * sizeof(Expr *));
        Expr *hidden = new_expr(p, E_VAR, start);
        hidden->name = arena_strdup(p->arena, "$ret");
        hidden->type = type_ptr(&p->ty, ret);
        hidden->slot = declare_var(p, "$ret", hidden->type);
        np[0] = hidden;
        params = np;
        pn++;
    }

    char *mangled = arena_alloc(p->arena, strlen(sname) + strlen(mname) + 3);
    snprintf(mangled, strlen(sname) + strlen(mname) + 3, "%s__%s", sname, mname);

    Stmt *fn = new_stmt(p, S_FUNC, start);
    fn->fname = mangled;
    fn->ret_type = ret;
    fn->params = params;
    fn->nparams = pn;
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
    StructMethod *m = arena_alloc(p->arena, sizeof *m);
    m->name = arena_strdup(p->arena, mname);
    m->ret = ret;
    m->ptypes = ptypes;
    m->nparams = nexplicit;
    m->body = fn;
    m->is_virtual = is_virtual;
    m->is_override = is_override;
    m->vtable_index = -1;
    struct_add_method(&p->ty, sd, m);

    scope_pop(p);
    p->scope = saved_scope;
    p->next_offset = saved_off;
    p->cur_ret = saved_ret;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
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
                diag_error(start, "'%s' is not a variant of '%s'", vname,
                           ud ? ud->name : "this match");
                v = NULL;
            } else {
                covered++;
            }
        } else {
            diag_error(cur(p)->span, "expected a variant name or '_' in match arm");
            break;
        }

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
        diag_error(start, "match is not exhaustive: '%s' has %d variant%s but %d covered", ud->name,
                   ud->nvariants, ud->nvariants == 1 ? "" : "s", covered);
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
        s->body = parse_stmt(p);
        return s;
    }
    case T_KW_FOR: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        Stmt *init = NULL;
        if (!at(p, T_SEMI)) {
            if (at(p, T_KW_VAR) || is_type_token(cur(p)->kind)) {
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
        s->body = parse_stmt(p);
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
            s->expr = parse_expr(p);
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
    default:
        break;
    }

    if (is_type_token(cur(p)->kind)) {
        return parse_var_decl(p);
    }
    if (t->kind == T_IDENT && peek(p, 1)->kind == T_IDENT) {
        diag_error(t->span, "unknown type '%s'", t->text);
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

Stmt *parse_program(Arena *arena, Token *toks, int ntoks, StringTable *strings) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.toks = toks;
    p.ntoks = ntoks;
    typectx_init(&p.ty, arena);
    p.strings = strings;
    p.cur_ret = NULL;

    prescan_struct_names(&p);
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
            Stmt *fn = parse_func(&p);
            if (fn->is_entry)
                has_main = 1;
            items[n++] = fn;
        } else if (at(&p, T_KW_CLASS)) {
            items[n++] = parse_struct_decl(&p, 1); /* a class decl */
        } else if (at(&p, T_KW_STRUCT)) {
            items[n++] = parse_struct_decl(&p, 0); /* a type decl, not a statement */
        } else if (at(&p, T_KW_ENUM)) {
            items[n++] = parse_enum_decl(&p); /* a type decl, not a statement */
        } else {
            items[n++] = parse_stmt(&p);
            top_stmts++;
        }
    }

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
    scope_pop(&p);
    return program;
}
