#ifndef VELA_TYPES_H
#define VELA_TYPES_H

#include "arena.h"

/* Type kinds. Pointers, arrays, and structs are all built from a base type so
 * the checker and codegen can reason about them uniformly. */
typedef enum {
    TK_VOID,
    TK_INT,
    TK_BOOL,
    TK_STRING,
    TK_PTR,   /* *base */
    TK_ARRAY, /* base[len] */
    TK_STRUCT,
    TK_UNION,     /* tagged sum: a leading int tag + variant payloads */
    TK_FNPTR,     /* fn(params) -> ret: a function pointer, carries its signature */
    /* A bound method pointer, `&obj.M`. Eight bytes: a pointer to a
     * garbage-collected cell holding { code, receiver }. Keeping it a scalar
     * rather than a two-word aggregate means it lives in a register and is
     * passed like an int, so none of the struct copy machinery applies. */
    TK_MPTR,
    TK_TYPEPARAM, /* a generic type parameter (substituted during monomorphization) */
} TypeKind;

typedef struct Type Type;
typedef struct StructDef StructDef;
typedef struct UnionDef UnionDef;

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
    int is_override;   /* declared `override` (must override a base virtual) */
    int vtable_index;  /* slot in the class vtable when is_virtual */
} StructMethod;

struct StructDef {
    char *name;
    Field *fields;
    int nfields;
    int size;  /* total bytes, multiple of align */
    int align; /* max field alignment */
    int complete;
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
    StructDef *base;    /* base class, or NULL */
    int nvtable;        /* number of virtual slots (inherited + own) */
    char **vtable_impl; /* per-slot mangled implementation name for this class */
};

struct Type {
    TypeKind kind;
    Type *base;        /* TK_PTR / TK_ARRAY */
    int len;           /* TK_ARRAY: element count, -1 for dynamic */
    StructDef *sdef;   /* TK_STRUCT */
    UnionDef *udef;    /* TK_UNION */
    const char *pname; /* TK_TYPEPARAM */
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
    Type *t_string;
    StructDef **structs;
    int nstructs;
    int cap;
    UnionDef **unions;
    int nunion;
    int union_cap;
} TypeCtx;

void typectx_init(TypeCtx *ctx, Arena *arena);

Type *type_void(TypeCtx *ctx);
Type *type_int(TypeCtx *ctx);
Type *type_bool(TypeCtx *ctx);
Type *type_string(TypeCtx *ctx);
Type *type_ptr(TypeCtx *ctx, Type *base);
Type *type_array(TypeCtx *ctx, Type *elem, int len);

/* A function type. `ptypes` is borrowed, not copied. */
Type *type_fnptr(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret);

/* A bound method pointer with the same signature shape. */
Type *type_mptr(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret);

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
 * success, -1 if an `override` doesn't match a base virtual. */
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
};

/* Registers a method and a property backing field on a struct. */
void struct_add_method(TypeCtx *ctx, StructDef *sd, StructMethod *m);
StructMethod *struct_find_method(StructDef *sd, const char *name);
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

int type_size(Type *t);
int type_align(Type *t);
int type_equals(Type *a, Type *b);
int type_assignable(Type *dst, Type *src); /* allows derived-class* -> base-class* upcast */
int type_is_scalar(Type *t);               /* 8-byte value that fits in rax */

/* Renders a type for diagnostics, e.g. "int*", "int[]", "Point". */
const char *type_name(TypeCtx *ctx, Type *t);

#endif /* VELA_TYPES_H */
