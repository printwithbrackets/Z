#ifndef Z_AST_H
#define Z_AST_H

#include "arena.h"
#include "source.h"
#include "token.h"
#include "types.h"

typedef enum {
    E_INT,
    E_F64, /* a float literal; value in `dval` */
    E_CVT, /* an explicit or widening conversion of `lhs` to `type` */
    E_BOOL,
    E_NULL,      /* the null pointer literal; type is left unknown */
    E_INTRINSIC, /* a built-in: abs/min/max/clamp/sqrt, resolved at parse time */
    E_FNPTR,     /* &f: the address of a function, typed by its signature */
    E_ICALL,     /* f(args) where f is a function pointer */
    E_MPTR,      /* &obj.M: the address of a { code, receiver } binding cell */
    /* A lambda that captured something: a { code, env } cell. `lhs` is the
     * function's address and `env` the captured environment. */
    E_CLOSURE,
    E_STRING,
    /* s.length: a load of the string header's length field. Its own node rather
     * than an E_FIELD with an offset, because the field lives *before* the value
     * -- the string points at its bytes, not at its header -- and every other
     * field read in the code generator adds a positive offset. */
    E_STRLEN,
    /* s[a..b]: a half-open byte range of a string, as a new string. `rhs` is the
     * inclusive start, `env` the exclusive end. */
    E_SLICE,
    E_VAR,
    E_UNARY,
    E_BINARY,
    E_ASSIGN,
    /* Postfix `x++` / `x--`: the old value, and the variable incremented.
     *
     * Its own node rather than a rewrite to `x = x + 1`, because that rewrite
     * yields the *new* value and postfix increment is defined as yielding the old
     * one. It is not a corner case: `data[n++] = v` is how every growable
     * collection in the language is written, and with the rewrite it stored at
     * `data[n + 1]` and skipped `data[n]`. Desugaring an expression into
     * statements needs a temporary and a scope, so the node stays an expression
     * and the code generator emits the two orders. */
    E_POSTINC,
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
    /* `move x`: yields x's value and gives up the source's claim to it.
     *
     * The source is poisoned at the point of the move, so a later read of it is
     * a diagnostic at compile time where the parser can see it, and a null at
     * run time where it cannot. That is the whole point of spelling the transfer
     * out: without it, two variables name one allocation and one of them frees
     * it. */
    E_MOVE,
} ExprKind;

typedef struct Expr Expr;
typedef struct Stmt Stmt;

/* Does this expression name string bytes that something else owns? Defined in the
 * parser, where the ownership rules live, and read by the code generator when it
 * decides whether a value has to be released as a temporary. Declared here rather
 * than duplicated, because two copies of this answer that disagree would show up
 * as a use-after-free rather than as anything a compiler could complain about. */
int expr_is_borrowed_string(const Expr *e);

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
    int compound;     /* E_ASSIGN: nonzero for += -= *= %= */
    /* E_ASSIGN storing a string: the destination's previous value has to be
     * released, because overwriting the slot is the only other way a string
     * stops being owned by anyone. Set by the parser alongside the copy, so both
     * halves of "a string slot owns exactly one value" live in one decision. */
    int frees_old;
    /* Set on a fresh string value that a store or a `return` has taken over, so
     * the code generator does not also release it as a temporary. The two are the
     * same value and it has exactly one owner: the slot, or the caller. */
    int str_result_owned;
    int is_extern; /* E_CALL to an `extern` function: symbol used verbatim */
    /* E_BINARY/E_UNARY: nonzero once loop-invariant code motion has hoisted
     * this expression, holding the frame slot its value was computed into.
     * Zero means "not hoisted", which is safe because slot numbers start at 8. */
    int hoisted_slot;
    Expr *lhs;
    /* E_TRY: the destructors for every scope between the `?` and the function
     * that `?` propagates out of, built where the `?` was written. */
    Stmt *try_drops;
    Expr *rhs;
    Expr *env; /* E_CLOSURE: the captured environment */
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
    /* End of an inlined function body: jump to the label its wrapper block
     * carries. A `return` inside an inlined body becomes an assignment to the
     * caller's hidden result local followed by one of these, which is what lets
     * a function with several returns be inlined at all. */
    S_LEAVE,
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
    /* S_BLOCK: the scope this block introduced, so the destructor pass can find
     * the locals it owns. NULL on a block that shares its enclosing scope. */
    struct Scope *own_scope;
    /* S_RETURN: the scope the `return` was written in -- the innermost one it
     * is leaving. Its locals die first; the pass then walks up the parent chain
     * for the rest, stopping at the function's parameter scope. */
    struct Scope *ret_scope;
    /* S_BREAK/S_CONTINUE: the scope the jump must not destroy, which is the one
     * enclosing the loop. Everything from where the jump was written up to *but
     * not including* this goes, so the loop's own body scope is included -- a
     * `break` does leave it -- and whatever the loop was written inside survives. */
    struct Scope *jump_floor;
    /* S_RETURN: the destructors for every scope the `return` leaves. They run
     * *after* the returned value has been computed and copied, not before it,
     * because a returned local is the very thing those destructors destroy: the
     * copy on return (`own_string_copy`) reads it, and running a destructor
     * first freed the bytes the copy was about to read. Held here rather than
     * spliced in ahead of the statement so the code generator can order the two
     * correctly -- which is what `?` already does with `try_drops`. */
    Stmt *ret_drops;
    /* S_BLOCK: when nonzero, codegen emits this label after the block's items,
     * and it is what an S_LEAVE inside the block jumps to. Zero on every block
     * that is not an inlined body. */
    int inl_label;

    /* S_FUNC: the destructor pass put at least one destructor call in this
     * body. The inliner declines such a function: a spliced body would run its
     * teardown in the caller's frame, where the value it destroys is a frame
     * slot the inliner has renumbered, and the callee's own copy would then run
     * the same drops a second time at its own exit. */
    int owns_drops;

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
