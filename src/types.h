#ifndef Z_TYPES_H
#define Z_TYPES_H

#include "arena.h"

/* Type kinds. Pointers, arrays, and structs are all built from a base type so
 * the checker and codegen can reason about them uniformly. */
typedef enum {
    TK_VOID,
    TK_INT,
    TK_BOOL,
    /* An IEEE-754 binary64. 8 bytes, like `int`, but it lives in an XMM
     * register rather than a general-purpose one, and it is not an integer:
     * division, comparison and conversion all differ. */
    TK_F64,
    TK_STRING,
    TK_PTR,   /* *base */
    TK_ARRAY, /* base[len] */
    TK_STRUCT,
    TK_UNION, /* tagged sum: a leading int tag + variant payloads */
    TK_FNPTR, /* fn(params) -> ret: a function pointer, carries its signature */
    /* A bound method pointer, `&obj.M`. Eight bytes: a pointer to a
     * garbage-collected cell holding { code, receiver }. Keeping it a scalar
     * rather than a two-word aggregate means it lives in a register and is
     * passed like an int, so none of the struct copy machinery applies. */
    TK_MPTR,
    /* A closure: a lambda that captured something. Where a `fn` value is a bare
     * code address, this is a pointer to a garbage-collected cell holding the
     * code address and a pointer to the captured environment -- the same
     * { code, receiver } shape a bound method uses, with the captured variables
     * in place of the receiver. Keeping it a single eight-byte scalar means
     * closures pass, store and return exactly like function pointers, and none
     * of the aggregate machinery applies. */
    TK_CLOSURE,
    /* An interface: a named set of method signatures. A value of interface type
     * is a pointer to a two-word cell -- { itab, receiver } -- so it is one
     * scalar and needs none of the aggregate machinery. The itab is a static
     * array of code pointers, one per method in declaration order; the receiver
     * is the object, or a pointer to a boxed copy when the implementer is a
     * struct. */
    TK_IFACE,
    TK_TYPEPARAM, /* a generic type parameter (substituted during monomorphization) */
    /* The `a` of a built-in signature: "whatever this is". A generic function
     * cannot ask what type it was instantiated with -- `typeof` answers, but
     * every branch after the test still has to type-check, and a branch that
     * calls int_to_string on a string does not -- so the two questions that
     * genuinely depend on the type, a value's text form and its hash, are asked
     * of the code generator instead, which does know.
     *
     * It only ever appears as a parameter type, and only for a built-in. There
     * is no way to write it, name it or declare a variable of it. */
    TK_ANY,
} TypeKind;

typedef struct Type Type;
typedef struct StructDef StructDef;
typedef struct UnionDef UnionDef;
typedef struct IfaceDef IfaceDef;

typedef struct {
    char *name;
    Type *type;
    int offset;  /* byte offset within the struct */
    int is_prop; /* 1 if this is an auto-property backing field */
} Field;

/* A method declared inside a struct/class, associated with it by name. */
typedef struct StructMethod {
    char *name;
    Type *ret;
    Type **ptypes;
    int nparams;
    struct Stmt *body; /* S_FUNC AST, first param is the receiver (this) */
    int is_virtual;    /* declared `virtual` or `override` (dynamic dispatch) */
    int vtable_index;  /* slot in the class vtable when is_virtual */
} StructMethod;

struct StructDef {
    char *name;
    Field *fields;
    int nfields;
    int size;  /* total bytes, multiple of align */
    int align; /* max field alignment */
    int complete;
    /* 1 when a pre-scan already filled in the fields, so parse_struct_decl knows
     * to start over rather than append to them. The pre-scan is what lets a
     * struct be used before it is declared; the real parse still rebuilds the
     * same layout when it gets there. */
    int prescanned;
    StructMethod **methods;
    int nmethods;
    int method_cap;
    /* Auto-properties (`int X { get; set; }`) get a hidden backing field. */
    Field *props;
    int nprops;
    int prop_cap;
    /* Classes (`class C : B`) are heap objects: an 8-byte vtable pointer is
     * stored first, fields follow, and virtual methods dispatch through the
     * object's vtable. */
    int is_class;
    int nvtable;        /* number of virtual slots */
    char **vtable_impl; /* per-slot mangled implementation name for this class */
};

struct Type {
    TypeKind kind;
    Type *base;        /* TK_PTR / TK_ARRAY */
    int len;           /* TK_ARRAY: element count, -1 for dynamic */
    StructDef *sdef;   /* TK_STRUCT */
    UnionDef *udef;    /* TK_UNION */
    const char *pname; /* TK_TYPEPARAM */
    IfaceDef *idef;    /* TK_IFACE */
    /* TK_FNPTR: the signature, so a call through the pointer can be checked
     * and the right number of arguments marshalled. */
    Type **ptypes;
    int nparams;
    Type *ret;
};

/* Owns all Type/StructDef objects for one compilation (arena-backed). */
typedef struct {
    Arena *arena;
    Type *t_void;
    Type *t_int;
    Type *t_bool;
    Type *t_f64;
    Type *t_string;
    Type *t_any;
    StructDef **structs;
    int nstructs;
    int cap;
    UnionDef **unions;
    int nunion;
    int union_cap;
    IfaceDef **ifaces;
    int niface;
    int iface_cap;
    /* `Result<T,E>` instantiations, interned on the pair so that two mentions of
     * the same one are the same Type and compare equal. */
    Type **results;
    int nresult;
    int result_cap;
} TypeCtx;

void typectx_init(TypeCtx *ctx, Arena *arena);

Type *type_void(TypeCtx *ctx);
Type *type_int(TypeCtx *ctx);
Type *type_bool(TypeCtx *ctx);
Type *type_f64(TypeCtx *ctx);
Type *type_string(TypeCtx *ctx);
/* The `a` of a built-in signature. See TK_ANY. */
Type *type_any(TypeCtx *ctx);
Type *type_ptr(TypeCtx *ctx, Type *base);
Type *type_array(TypeCtx *ctx, Type *elem, int len);

/* One method an interface requires, as a signature. `sig` is a TK_FNPTR, which
 * is exactly the shape a call through a pointer is checked against, so a call
 * through an interface and a call through a function pointer are checked the
 * same way. */
typedef struct {
    char *name;
    Type *sig;
} IfaceMethod;

/* An interface: a name, an ordered list of required methods, and a flag for
 * whether the type is sealed (nothing implements it) which nothing sets today
 * and which exists so the shape does not have to change later. */
struct IfaceDef {
    char *name;
    IfaceMethod *methods;
    int nmethods;
    int sealed;
    int prescanned; /* see StructDef.prescanned */
};

/* A function type. `ptypes` is borrowed, not copied. */
Type *type_fnptr(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret);

/* Interfaces. `type_define_iface` registers a name; `iface_add_method` appends a
 * required signature, in the order that becomes the itab layout; `type_iface`
 * builds the type. `iface_find_method` looks a method up by name, which is how
 * a call resolves to an itab slot. */
IfaceDef *type_define_iface(TypeCtx *ctx, const char *name);
Type *type_find_iface(TypeCtx *ctx, const char *name);
void iface_add_method(TypeCtx *ctx, IfaceDef *id, const char *name, Type *sig);
IfaceMethod *iface_find_method(IfaceDef *id, const char *name);
Type *type_iface(TypeCtx *ctx, IfaceDef *id);
/* 1 when `t` has every method `id` requires, with matching signatures. When it
 * does, `slots` (if given) receives the index of each required method in the
 * implementing type's own vtable, for a class. */
int iface_implemented_by(TypeCtx *ctx, IfaceDef *id, Type *t, int *slots);
/* 1 when this method has exactly the signature an interface requires. Separate
 * from iface_implemented_by so a diagnostic can say "it has no method 'X'" and
 * "'X' has the wrong signature" rather than one message for both. */
int iface_method_matches(StructMethod *m, Type *sig);

/* A bound method pointer with the same signature shape. */
Type *type_mptr(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret);

/* A closure over `nparams` parameters returning `ret`. `ptypes` is borrowed. */
Type *type_closure(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret);

/* Declares a struct type (not yet complete). Returns NULL if the name is
 * already in use. */
StructDef *type_define_struct(TypeCtx *ctx, const char *name);

/* Looks up a struct by name; NULL if unknown. */
Type *type_find_struct(TypeCtx *ctx, const char *name);

/* Appends a field to an in-progress struct and assigns its byte offset. */
void struct_add_field(TypeCtx *ctx, StructDef *sd, const char *name, Type *ftype);

/* Finalizes a struct's size (padded to alignment) and marks it complete. */
void struct_finish(StructDef *sd);

/* Finds a field by name; NULL if absent. */
Field *struct_find_field(StructDef *sd, const char *name);
/* Computes a class's vtable layout after methods are registered. Returns 0 on
 * success. */
int class_finish_vtable(TypeCtx *ctx, StructDef *sd);

/* One variant of a union: a name, a discriminant, and payload fields. */
typedef struct {
    char *name;
    Field *fields; /* payload fields (offset > 0, after the tag) */
    int nfields;
    int tag; /* discriminant value stored at offset 0 */
} VariantDef;

/* A tagged union (sum type / enum). */
struct UnionDef {
    char *name;
    VariantDef *variants;
    int nvariants;
    int size; /* total bytes including the leading int tag */
    int align;
    int complete;
    int prescanned; /* see StructDef.prescanned */
};

/* Registers a method and a property backing field on a struct. */
void struct_add_method(TypeCtx *ctx, StructDef *sd, StructMethod *m);
StructMethod *struct_find_method(StructDef *sd, const char *name);

/* The destructor of a type, or NULL. Registered under the mangled name, since
 * `~` cannot appear in an assembler symbol. */
StructMethod *struct_find_dtor(StructDef *sd);

/* True when a value of this type owns something and must be destroyed when its
 * scope ends.
 *
 * A type owns if it declares a destructor, or if any field of it does -- the
 * default teardown destroys each owning field, so `struct Pair { Vec<int> a; }`
 * is owning without saying so. It is computed from the type's own declaration
 * rather than declared, because a flag a programmer can set is a flag that can
 * be set wrong, and getting it wrong here is a double free. */
int type_needs_drop(Type *t);
StructDef *struct_method_owner(StructDef *sd, const char *name);
void struct_add_prop(TypeCtx *ctx, StructDef *sd, const char *name, Type *type, int offset);
void struct_add_prop_field(TypeCtx *ctx, StructDef *sd, const char *name, Type *type);

/* Union (sum type) construction and lookup. */
UnionDef *type_define_union(TypeCtx *ctx, const char *name);
Type *type_find_union(TypeCtx *ctx, const char *name);
/* Finds a union variant by (globally unique) variant name. */
VariantDef *type_find_variant(TypeCtx *ctx, const char *name, UnionDef **owner);
UnionDef *type_find_variant_owner(TypeCtx *ctx, const char *name);

/* A fresh type-parameter placeholder (identity-compared; substituted at
 * instantiation time). */
Type *type_new_param(TypeCtx *ctx, const char *name);
/* Adds a variant with the given payload fields (types already resolved) and
 * assigns its tag; the caller finishes layout with union_finish. */
void union_add_variant(TypeCtx *ctx, UnionDef *ud, const char *name, Type **ftypes,
                       const char **fnames, int nfields);
void union_finish(UnionDef *ud);

/* `Result<T,E>`, as a two-variant union: Ok carries T, Err carries E.
 *
 * It is a union rather than a new type kind so that everything the union already
 * does comes for free -- layout, construction, `match` with payload binding,
 * exhaustiveness checking, and the struct-return calling convention.
 *
 * The layout is deliberately uniform: tag at offset 0, one payload word at
 * offset 8, sixteen bytes whatever T and E are. Both parameters must be one-word
 * types for that to hold. Uniformity is what makes `?` cheap: the error value
 * returned early has the same bytes as the one the caller expects, so it is a
 * copy rather than a conversion.
 *
 * Returns NULL if either parameter is not a one-word type, which the caller
 * reports; the restriction is in the header comment here rather than buried in
 * the parser. */
Type *type_result(TypeCtx *ctx, Type *ok, Type *err);
/* 1 for a `Result<T,E>` type, however it was spelled. */
int type_is_result(Type *t);
/* The T of a Result, or NULL. */
Type *type_result_ok(Type *t);
/* The E of a Result, or NULL. */
Type *type_result_err(Type *t);

int type_size(Type *t);
int type_align(Type *t);
int type_equals(Type *a, Type *b);
int type_assignable(Type *dst, Type *src); /* equality, or anything to an interface */
int type_is_scalar(Type *t);               /* 8-byte value that fits in rax */

/* 1 for the IEEE-754 types, which travel in XMM registers and answer to
 * different instructions than everything else. The optimizer branches on this
 * constantly, so it is worth one predicate rather than a comparison at each
 * site. */
int type_is_float(Type *t);

/* The type an arithmetic or comparison operand takes part as. A `float` operand
 * makes the whole expression a `float`; an `int` operand leaves it an `int`.
 * The conversion is then materialized by the parser as an explicit node rather
 * than being implicit in the code generator, so there is exactly one place that
 * decides how a widening happens. */
Type *type_arith_result(TypeCtx *ctx, Type *a, Type *b);

/* 1 for a value that can be called: a plain function pointer, a bound method,
 * or a closure. Callers should not switch on the kind themselves, since a
 * closure and a function pointer are interchangeable at every use that is not
 * an assignment. */
int type_is_callable(Type *t);

/* 1 when `src` may be assigned to `dst` without an explicit cast. Numeric
 * widening is the one conversion Z performs on its own, and it only ever goes
 * from `int` to `float`: every value an `int` can hold is a `float` exactly, so
 * the conversion cannot lose anything. The reverse truncates toward zero and is
 * therefore never implicit, matching C# and every other language that has both
 * an integer and a floating-point type. */
int type_widens_to(Type *dst, Type *src);

/* Renders a type for diagnostics, e.g. "int*", "int[]", "Point". */
const char *type_name(TypeCtx *ctx, Type *t);

#endif /* Z_TYPES_H */
