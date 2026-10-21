#include "parser.h"

#include "diag.h"

#include "z_limits.h"

#include "surface.h"

/* How a diagnostic names the output builtin. A message that said `print` would be
 * naming a spelling no program can write, which is the one thing a diagnostic must
 * not do. The shipped surface spelling lives in surface.c's defaults; a project
 * that renames it also renames what the message says, because a project that
 * cannot write `Console.WriteLog` does not deserve to be told about it. */
#define PRINT_SURFACE "Console.WriteLog"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many variables one closure can capture. Each costs a word in the
 * environment and a word in the cell, so the bound is generous but real. */
#define MAX_CAPTURES 64

/* Declarations waiting to be appended to the program: the instances monomorphized
 * out of a template, which are produced part-way through parsing and emitted at
 * the end. */
typedef struct {
    Stmt **items;
    int count;
    int cap;
} PendingList;

/* A local variable binding in a lexical scope. */
typedef struct Var {
    char *name;
    Type *type;
    int offset;     /* byte offset from rbp within the function frame */
    int agg_param;  /* parameter of struct/union type: slot holds a pointer */
    int captured;   /* referenced by a closure: the slot holds a box pointer */
    int owner;      /* func_counter value of the function that declared it */
    int used;       /* referenced anywhere; drives -Wunused-local */
    int is_param;   /* a parameter rather than a local statement */
    int is_synth;   /* compiler-generated, or in a body we never see: not warnable */
    Span decl_span; /* where the name was written, for diagnostics */
    int needs_drop; /* the type owns something, so the scope must destroy it */
    /* Set once `move` has taken this variable's value. Reading it afterwards is
     * a diagnostic rather than a read of whatever the slot holds now, because
     * "use after move" and "read a null" are the same bug with two very
     * different amounts of help. */
    int is_moved;
    Span move_span; /* where the move was written, for the note */
    /* `lvar`: a local that outranks a plain `var` of the same name in the same
     * scope. The two can coexist there, so the redeclaration check below has to
     * compare tiers and not just names -- otherwise `var what = 2; lvar what = 1;`
     * in one body, which is the whole point of having both, would be rejected as
     * a duplicate. */
    int is_lvar;
    /* `gvar`: project storage rather than a frame slot, so `offset` above is
     * meaningless for it and the symbol comes from GlobalDef instead. */
    int is_global;
    struct Var *next;
} Var;

/* A `gvar`, `gint`, `gbool`, `gstring`, `gfloat` or `gFoo`: one declaration,
 * visible to every file in the project.
 *
 * Not a frame slot and not a compile-time constant. A constant initializer is
 * folded into `.data` at compile time; anything else is zeroed in `.bss` and
 * filled by a generated init function that runs before the entry point, which is
 * what lets `gvar x = new R(5);` mean the same thing here as it does in a
 * function. */
typedef struct GlobalDef {
    char *name;
    Type *type;
    Expr *init;        /* the initializer, or NULL for a declaration with none */
    Span decl_span;    /* where it was written, for diagnostics */
    const char *file;  /* the file that declared it */
    int init_is_const; /* the initializer is a literal: emit it into .data */
    /* The Var this global is referred to by, built on first use. See
     * global_var_ref: a use site has to be an E_VAR for codegen, and every pass
     * downstream of here keys off Var. */
    struct Var *ref;
    /* Assigned in the init function, and zero in .bss before it. */
    int needs_init;
    /* Position in the emitted table, which is declaration order. Stamped at
     * registration so a use site found during a later pass -- a function body
     * parsed after the declarations, which is the normal case -- can say which
     * global it means without searching for it again. */
    int gidx;
    struct GlobalDef *next;
} GlobalDef;

typedef struct Scope {
    Var *vars;
    /* Sibling link for the deferred unused-local report: every closed scope is
     * chained onto the parser so `report_unused_locals` can walk them after the
     * destructor pass has decided which values count as used. */
    struct Scope *warn_next;
    /* The locals in this scope that own something, in declaration order. Their
     * destructors run when the scope ends, so this is what the drop list is
     * built from. Held per scope rather than per function because the order is
     * per scope: a nested block's values go before the enclosing block's. */
    Var **drops;
    int ndrops;
    int drop_cap;
    struct Scope *parent;
    /* The outermost scope of a function body. Name lookup stops here: a variable
     * declared outside a function lives in a different frame, so naming it from
     * inside would read a slot holding something else entirely. */
    int is_fn_body;
} Scope;

/* How deep the open-scope stack may get. A block nested this far is already past
 * the parser's own recursion limit, so a program that reaches it is rejected
 * before it can overflow the stack rather than after. */
#define MAX_DROP_SCOPES 128

/* How deep the recursive descent may go.
 *
 * Each level of nesting in the source -- a parenthesis, an index, a block --
 * costs the parser a few C stack frames, and a program that nests far enough
 * exhausts the stack before the parser reaches the end of the file. The limit
 * is a multiple of what a program plausibly nests, so a real program is
 * unaffected, and a hostile one gets a diagnostic instead of a signal.
 *
 * The number is deliberately not tied to the size of the input: a program with
 * a hundred thousand terms in one expression is not deeply nested and must
 * still compile. */
#define MAX_PARSE_DEPTH 256

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
    int is_ext; /* extension method: first param is the receiver */
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
    char *ns_prefix; /* lexical namespace at the declaration, restored to re-parse it */
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
    char *name;      /* qualified template name (`n.Vec`), used for instances */
    char *src_name;  /* name as written (`Vec`), what the template body spells */
    char *ns_prefix; /* lexical namespace at the declaration, restored to re-parse it */
    int is_class;
    int ntparams;
    char **tparams;
    int tok_start, tok_end; /* `class` .. the closing brace */
    char **inst_names;      /* mangled instance names, parallel to inst_types */
    Type ***inst_types;
    int ninstances;
} GenericType;

typedef struct {
    Arena *arena;
    Token *toks;
    int ntoks;
    int pos;
    /* A pointer, not a value. Several passes parse a type with a shallow copy of
     * the Parser whose only difference is the cursor -- the signature pre-scan,
     * `Result<...>` detection, skip_type_tokens. A Parser holding a TypeCtx by
     * value would give each of those copies its *own* struct table, so anything
     * that registered a type there was invisible to the real parse: a generic
     * type named in a field or a parameter was instantiated into the copy, and
     * the copy's struct was a second type with the same name. The two then
     * compared unequal, and the error named two identical types:
     * "cannot assign 'Map__string_bool*' to 'Map__string_bool*'". */
    TypeCtx *ty;

    Scope *scope;
    int next_offset; /* byte-offset allocator for the current function frame */

    Sig *sigs;
    int nsigs;
    int sig_cap;

    Type *cur_ret; /* return type of the function being parsed */
    int foreach_counter;
    int loop_depth; /* break/continue must sit inside a loop */
    /* `p->nopen` when each enclosing loop's body was entered, innermost last. A
     * `break` destroys the scopes from where it was written up to the loop's own
     * body scope, and this is where "the loop's own body scope" is recorded. */
    int loop_top[MAX_DROP_SCOPES];
    int nloops;
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
    GenericType *gtypes; /* registered generic class/struct templates */
    int ngtypes, gtype_cap;
    /* While a generic type's declaration is being re-parsed for one instance:
     * the instance's StructDef, and the source name the declaration was written
     * with. A constructor is spelled with the source name (`Vec() { ... }`), so
     * resolving that name has to land on the instance being built rather than
     * failing to resolve at all. */
    StructDef *inst_sdef;
    const char *inst_srcname;
    /* The mangled name the instance being built is registered under, which
     * instantiate_type interns *before* it parses the body. A method of a
     * declaration may name its own type again -- `new Vec<T>()` inside Vec's own
     * reversed() -- and with nothing to find, that is unbounded recursion. */
    const char *inst_name;
    ConstDef *consts;
    int nconsts, const_cap;

    /* Concrete instances awaiting append to the program items, behind a pointer
     * for the same reason `ty` is: several passes parse with a shallow copy of
     * the Parser whose only difference is the cursor, and an instance built
     * during one of those -- a generic type named in a field, which the
     * signature pre-scan resolves -- has to be queued on the *real* list. A copy
     * queued it into a private array, and the instance was then missing from the
     * program: `Outer<T>` holding an `Inner<T>`, where every method of the inner
     * was an undefined reference at link time and nothing had complained. */
    PendingList *pending;
    int in_instantiate; /* set while re-parsing a template for an instance */

    /* ---- closures ----
     *
     * See the closure section below. `captured` is every variable in the current
     * function that a lambda captured: their frame slots hold a box rather than
     * the value. `lam_caps` is per-lambda -- the variables *this* lambda
     * captured, in the order the body first mentioned them, which is the order
     * its environment is laid out in. */
    int lambda_counter;
    /* Labels for spliced inlined bodies, unique across the program.
     *
     * These are emitted as `.Li<n>` rather than `.L<n>`, because the code
     * generator has its own label counter for loops and branches and the two are
     * seeded independently. Sharing one namespace meant a loop inside a spliced
     * body and the end of another spliced body could land on the same number, and
     * the assembler stopped on the duplicate symbol -- so a generic function with
     * an inlinable call in it did not build at all. That was the collision; this
     * counter was only half of it.
     *
     * Not restored across a generic instantiation, where `lambda_counter` and
     * `func_counter` are. Those two want the same names for a second instance of
     * one template, because the instance replaces the template rather than
     * joining it. A spliced body is not like that: two instances are two separate
     * places in the assembly and each needs its own label, so this one only ever
     * goes up. */
    int inline_label;
    int func_counter; /* owner id stamped on each Var; see capture_for */
    struct Var *captured[MAX_CAPTURES];
    int ncaptured;
    struct Var *lam_caps[MAX_CAPTURES];
    int nlamcaps;
    int lam_owner;  /* func id of the lambda being parsed */
    int lam_active; /* nonzero while inside a lambda body */
    int cur_owner;  /* func id of the function whose frame is being built */

    /* The scopes currently open in the function being parsed, innermost last.
     * A `return` or a `?` has to destroy what every one of them owns, and it is
     * three blocks deep when it is parsed, so the scopes have to be reachable
     * from wherever the exit is. Reset at a function boundary -- a lambda body
     * is a separate function with its own frame, and its scopes are not the
     * enclosing function's. */
    Scope *open_scopes[MAX_DROP_SCOPES];
    int nopen;

    /* How deep the recursive descent currently is.
     *
     * The parser recurses on the shape of the input: a parenthesis, an index
     * and a nested block each step one level deeper, and there is no bound on
     * how deep a program may nest. A file with tens of thousands of open
     * parens therefore ran the C stack out and the compiler died on a signal
     * rather than reporting a problem with the program.
     *
     * The limit is well above anything a person writes, and it is a limit on
     * the *parser's* recursion rather than on the input's length, because a
     * long expression is fine and a deeply nested one is not. */
    int depth;

    /* Whether the depth limit has already been reported, so a program nested
     * past it produces one diagnostic rather than one per level. */
    int depth_reported;

    /* Whether the scope-stack limit has already been reported, for the same
     * reason. */
    int scope_limit_reported;

    /* Every scope that has been closed, newest first, for the deferred
     * unused-local report. See scope_pop. */
    Scope *popped_scopes;

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

    /* The dialect this program is written in: the names a project writes mapped
     * to the names the compiler knows. Consulted only where ordinary resolution
     * has already failed, so it renames the standard library and the builtins
     * without ever getting between a program and a name it declared itself. */
    struct Surface *surf;

    /* ---- gvar: project-wide variables ----
     *
     * `gvar` and the other `g`-prefixed declarations register here, and every
     * file compiled into the same program shares one list. A name resolves
     * against it only after the scope walk has failed, which is what makes
     * `lvar` > `var` > `gvar` fall out of the existing structure rather than
     * needing a separate priority rule. */
    GlobalDef *globals;
    int nglobals;
    /* Set while parsing an `lvar`, so declare_var can tell a tier change from a
     * genuine duplicate. See the check there. */
    int declaring_lvar;

    /* ---- namespaces: `namespace netlink { ... }` + `netlink.Lobby` ----
     *
     * `ns_prefix` is the lexical prefix for definitions ("netlink" inside that
     * block, "" everywhere else), so a declaration registers under
     * "netlink.Lobby" and an unqualified use beside it falls back to the same.
     * `ns_paths` is every namespace path ever declared -- blocks and `import`
     * aliases alike, since an alias arrives as synthesized `namespace` tokens --
     * which is what makes `netlink` in `netlink.Lobby` resolve as a qualifier
     * rather than reading as a variable no one declared. */
    char *ns_prefix;
    char **ns_paths;
    int nns, ns_cap;
} Parser;

/* The internal name for `name`, or `name` itself. See the surface rules in
 * surface.h; the short version is that this is a fallback, not a claim. */
static const char *surf(Parser *p, const char *name) { return surface_name(p->surf, name); }

/* A method lookup that falls back to the dialect.
 *
 * Same rule as a type: the written name is tried first, so a project that has
 * declared its own `append` on its own type keeps that one, and only a name that
 * would otherwise be missing is looked up under its surface spelling. `v.push(x)`
 * on a `TextBuffer` therefore reaches the standard library's `append`, and
 * `v.push(x)` on a type of the project's own with a `push` method reaches the
 * project's. */
static StructMethod *find_method(Parser *p, StructDef *sd, const char *name) {
    StructMethod *m = struct_find_method(sd, name);
    if (m == NULL && p->surf != NULL)
        m = struct_find_method(sd, surf(p, name));
    return m;
}

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
        /* The condition and the step, for the same reason as the `if` and `while`
         * cases below: both are the loop's own expressions and both read and write
         * captured variables. The step was skipped outright. */
        mark_boxed_expr(p, s->cond);
        mark_boxed_expr(p, s->for_step);
        mark_boxed_stmt(p, s->body);
        return;
    case S_IF:
    case S_WHILE:
        /* The condition first. It reads the captured variables it names, so it
         * needs the marking as much as the body does -- and nothing marked it,
         * because this case walked the body and the else-arm and skipped the one
         * thing in between. A `while (d < 4)` over a captured `d` compared the
         * box pointer against 4, which is false, so the loop ran once. */
        mark_boxed_expr(p, s->cond);
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
static Stmt *build_drops_chain(Parser *p, Scope *inner, Span span);
static void close_form_stmts(Parser *p, Stmt **items, int n);
static void inline_program(Parser *p, Stmt *program);
static Stmt *close_form_loop(Parser *p, Stmt *loop, Stmt **siblings, int sib_index);
static Stmt *parse_block(Parser *p);
static Stmt *parse_func(Parser *p, int nested);
static Stmt *parse_struct_decl(Parser *p, int is_class);

static GenericType *find_gtype(Parser *p, const char *name);

/* The field types and names of one enum variant, gathered while its tokens are
 * scanned.
 *
 * Grown rather than fixed. The two arrays used to be sixteen entries on the stack
 * with a guard that stopped the writes at sixteen and nothing else: the count
 * carried on past it and was handed to the union builder with that length, which
 * read the names and the type pointers out of whatever followed the arrays in the
 * frame. A variant with twenty fields walked off the end of both arrays and the
 * process died.
 *
 * The second of the two sites that build a variant had it slightly worse: the
 * name was written before the guard, so a seventeenth field was already a store
 * out of bounds rather than only a read.
 *
 * There is no reason for a limit here. The builder copies into arena storage of
 * its own, so the fixed thing was only this scratch space, and a variant with
 * twenty fields is a big variant rather than a wrong one. A limit at sixteen
 * would have been a number with nothing behind it. */
typedef struct {
    Type **ftypes;
    const char **fnames;
    int n;
    int cap;
} VariantFields;

static void variant_fields_add(Parser *p, VariantFields *vf, Type *ft, const char *name) {
    if (vf->n == vf->cap) {
        int ncap = vf->cap == 0 ? 8 : vf->cap * 2;
        Type **nt = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Type *));
        const char **nn = arena_alloc_array(p->arena, (size_t)ncap, sizeof(char *));
        if (vf->ftypes != NULL) {
            memcpy(nt, vf->ftypes, (size_t)vf->n * sizeof(Type *));
            memcpy(nn, vf->fnames, (size_t)vf->n * sizeof(char *));
        }
        vf->ftypes = nt;
        vf->fnames = nn;
        vf->cap = ncap;
    }
    vf->ftypes[vf->n] = ft;
    vf->fnames[vf->n] = name;
    vf->n++;
}
static StructDef *resolve_gtype(Parser *p, GenericType *g, Type **args, int nargs, Span span);
static void split_shift_closer(Parser *p);
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
static const char *ns_here(Parser *p);
static char *ns_join(Parser *p, const char *prefix, const char *name);
static void ns_declare(Parser *p, const char *path);
static int ns_known(Parser *p, const char *path);
static void *ns_lookup(Parser *p, const char *name, void *(*look)(Parser *, const char *));
static void *look_sig(Parser *p, const char *n);
static void *look_generic(Parser *p, const char *n);
static void *look_gtype(Parser *p, const char *n);
static void *look_const(Parser *p, const char *n);
static void *look_global(Parser *p, const char *n);
static void *look_struct(Parser *p, const char *n);
static void *look_iface(Parser *p, const char *n);
static void *look_union(Parser *p, const char *n);
static const char *ns_short_name(const char *internal);
static Type *ns_lookup_plain_type(Parser *p, const char *internal);
static Expr *const_expr(Parser *p, ConstDef *c, Span span);
static int const_fold_static(Parser *p, Expr *e, long long *out);

/* The parser is split across the files in src/parser and included here in order, so
 * the concatenation is the single translation unit parser.c used to be.
 * The split is a map, not a change: see the section banners in each part. */
#include "parser/parser_01_tokens_types.inc"
#include "parser/parser_02_prescan_signatures.inc"
#include "parser/parser_03_expressions.inc"
#include "parser/parser_04_generics.inc"
#include "parser/parser_05_names_and_operators.inc"
#include "parser/parser_06_statements.inc"
#include "parser/parser_07_destructor_pass.inc"
#include "parser/parser_08_closures.inc"
#include "parser/parser_09_functions.inc"
#include "parser/parser_10_interfaces_loops_methods.inc"
#include "parser/parser_11_types_and_program.inc"
#include "parser/parser_12_closed_form.inc"
#include "parser/parser_13_inlining.inc"
