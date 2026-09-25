#ifndef VELA_AST_H
#define VELA_AST_H

#include "arena.h"
#include "source.h"
#include "token.h"
#include "types.h"

typedef enum {
    E_INT,
    E_BOOL,
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
    int str_id;       /* E_STRING */
    char *name;       /* E_VAR, E_CALL, E_FIELD (field name) */
    int slot;         /* E_VAR: local slot index (resolved during parse) */
    int agg_param;    /* E_VAR param of struct/union type: slot holds a pointer */
    int field_off;    /* E_FIELD: byte offset of the field */
    int vtable_index; /* E_VCALL: slot index into the receiver's vtable */
    TokenKind op;     /* E_UNARY, E_BINARY; base op for compound E_ASSIGN */
    int compound;     /* E_ASSIGN: nonzero for += -= *= /= %= */
    Expr *lhs;
    Expr *rhs;
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
    Type *ret_type;
    Stmt *fbody;
    Expr **params;
    int nparams;
    int locals_bytes;        /* total bytes of the function's frame locals (params + locals) */
    int is_entry;            /* the program's entry function */
    int is_ext;              /* extension method (first param is the receiver) */
    int is_generic_template; /* generic function placeholder: not emitted directly */

    /* S_STRUCT / S_UNION */
    StructDef *sdef;
    UnionDef *udef;
};

#endif /* VELA_AST_H */
