#ifndef Z_AST_H
#define Z_AST_H

#include "arena.h"
#include "source.h"
#include "token.h"
#include "types.h"

typedef enum {
    E_INT,
    E_F64,   /* a float literal; value in `dval` */
    E_CVT,   /* an explicit or widening conversion of `lhs` to `type` */
    E_BOOL,
    E_NULL,   /* the null pointer literal; type is left unknown */
    E_INTRINSIC, /* a built-in: abs/min/max/clamp/sqrt, resolved at parse time */
    E_FNPTR,     /* &f: the address of a function, typed by its signature */
    E_ICALL,     /* f(args) where f is a function pointer */
    E_MPTR,      /* &obj.M: the address of a { code, receiver } binding cell */
    /* A lambda that captured something: a { code, env } cell. `lhs` is the
     * function's address and `env` the captured environment. */
    E_CLOSURE,
    E_STRING,
    E_VAR,
    E_UNARY,
    E_BINARY,
    E_ASSIGN,
    E_CALL,
    E_INDEX,     /* a[i] */
    E_DEREF,     /* *p */
    E_ADDR,      /* &x */
    E_FIELD,     /* s.field (also a.length) */
    E_NEW,       /* new T[n] */
    E_STRUCTLIT, /* new Point(1, 2) */
    E_TERNARY,   /* cond ? a : b (lhs=cond, rhs=a, args[0]=b) */
    E_UNIONLIT,  /* Circle(5) */
    E_MATCH,     /* match e { V(x) => body, ... } */
    E_VCALL,     /* virtual method call: args[0]=receiver, vtable slot in e->vtable_index */
    E_NEWCLASS,  /* new C(args): allocate an object, set vptr, run ctor */
    E_HOIST,     /* loop-invariant subexpression hoisted to a frame slot (ival=slot) */
    /* `expr?` on a `Result<T,E>`: yields the Ok payload, and returns the error
     * from the enclosing function if the value turned out to be an Err. `lhs` is
     * the Result; `type` is T, the payload. See the Result section in the parser
     * for why this needs its own node rather than desugaring to statements. */
    E_TRY,
    /* A concrete value materialised as an interface value: a pointer to a
     * { itab, receiver } cell. `lhs` is the value, `type` is the interface.
     * `impl` is the implementing type and `idef` the interface, which together
     * name the itab the code generator has to emit. */
    E_IFACE,
} ExprKind;

typedef struct Expr Expr;
typedef struct Stmt Stmt;

/* One arm of a `match`: binds the variant's payload fields then runs body. */
typedef struct {
    VariantDef *variant; /* matched variant (NULL for a `_` wildcard) */
    int *bind_slots;     /* frame slots for bound payload names */
    int nbind;
    Expr *body;
} MatchArm;

struct Expr {
    ExprKind kind;
    Span span;
    Type *type;
    long long ival;   /* E_INT, E_BOOL */
    double dval;      /* E_F64 */
    int str_id;       /* E_STRING */
    char *name;       /* E_VAR, E_CALL, E_FIELD (field name) */
    int slot;         /* E_VAR: local slot index (resolved during parse) */
    int agg_param;    /* E_VAR param of struct/union type: slot holds a pointer */
    int field_off;    /* E_FIELD: byte offset of the field */
    int vtable_index; /* E_VCALL: slot index into the receiver's vtable */
    TokenKind op;     /* E_UNARY, E_BINARY; base op for compound E_ASSIGN */
    int compound;     /* E_ASSIGN: nonzero for += -= *= /= %= */
    int is_extern;    /* E_CALL to an `extern` function: symbol used verbatim */
    /* E_BINARY/E_UNARY: nonzero once loop-invariant code motion has hoisted
     * this expression, holding the frame slot its value was computed into.
     * Zero means "not hoisted", which is safe because slot numbers start at 8. */
    int hoisted_slot;
    Expr *lhs;
    Expr *rhs;
    Expr *env;      /* E_CLOSURE: the captured environment */
    /* E_IFACE: the type satisfying the interface, and the interface required. */
    StructDef *impl;
    IfaceDef *idef;
    /* Set on an E_VAR, or the S_VAR that declares it, when the variable is
     * captured by a closure. Its frame slot then holds a pointer to a heap cell
     * rather than the value, and every access goes through it. */
    int boxed;
    Expr **args;
    int nargs;
    MatchArm *arms; /* E_MATCH */
    int narms;
    VariantDef *variant; /* E_UNIONLIT */
};

typedef enum {
    S_VAR,
    S_EXPR,
    S_IF,
    S_WHILE,
    S_FOR,
    S_RETURN,
    S_BREAK,
    S_CONTINUE,
    S_BLOCK,
    S_FUNC,
    S_STRUCT,
    S_UNION,
} StmtKind;

struct Stmt {
    StmtKind kind;
    Span span;

    /* S_VAR */
    char *name;
    Type *type;
    int slot;
    /* 1 when this declaration names a variable a closure captured, so the slot
     * holds a box rather than the value. */
    int boxed;
    Expr *init;

    /* S_EXPR, S_RETURN */
    Expr *expr;

    /* S_IF, S_WHILE, S_FOR */
    Expr *cond;
    Stmt *body;
    Stmt *orelse;
    Stmt *for_init; /* S_FOR */
    Expr *for_step; /* S_FOR */

    /* S_BLOCK and the top-level program */
    Stmt **items;
    int nitems;
    /* LICM: loop-invariant subexpressions hoisted out of this loop, computed
     * once before the loop and read from a frame slot inside it. */
    int *hoist_slots;
    Expr **hoist_exprs;
    int nhoist;

    /* S_FUNC */
    char *fname;
    /* The name as the reader wrote it, for diagnostics and for DWARF. `fname` is
     * the emitted symbol, which for a hoisted lambda, a nested function or a
     * struct method is a mangled name with the source name buried in it; a
     * debugger showing `$fn3_helper` is not telling anyone anything they wrote. */
    char *src_fname;
    Type *ret_type;
    Stmt *fbody;
    Expr **params;
    int nparams;
    int locals_bytes;        /* total bytes of the function's frame locals (params + locals) */
    int is_entry;            /* the program's entry function */
    int is_extern;           /* declared `extern`: the symbol is C's, used verbatim */
    int is_export;           /* declared `export`: keep the name, make it .globl */
    int is_ext;              /* extension method (first param is the receiver) */
    int is_generic_template; /* generic function placeholder: not emitted directly */
    /* Index of the first parameter the user actually named. 0 for an ordinary
     * function; 1 when parameter 0 is `this` or the hidden struct-return
     * buffer, neither of which appears in the source. Debug info uses it to
     * describe only the parameters a reader can match to the declaration. */
    int vis_start;

    /* S_STRUCT / S_UNION */
    StructDef *sdef;
    UnionDef *udef;
};

#endif /* Z_AST_H */
