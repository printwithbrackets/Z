#include "types.h"

#include <stdio.h>
#include <string.h>

void typectx_init(TypeCtx *ctx, Arena *arena) {
    memset(ctx, 0, sizeof *ctx);
    ctx->arena = arena;
    ctx->t_void = arena_alloc(arena, sizeof(Type));
    ctx->t_void->kind = TK_VOID;
    ctx->t_int = arena_alloc(arena, sizeof(Type));
    ctx->t_int->kind = TK_INT;
    ctx->t_bool = arena_alloc(arena, sizeof(Type));
    ctx->t_bool->kind = TK_BOOL;
    ctx->t_f64 = arena_alloc(arena, sizeof(Type));
    ctx->t_f64->kind = TK_F64;
    ctx->t_string = arena_alloc(arena, sizeof(Type));
    ctx->t_string->kind = TK_STRING;
    ctx->t_any = arena_alloc(arena, sizeof(Type));
    ctx->t_any->kind = TK_ANY;
}

Type *type_void(TypeCtx *ctx) { return ctx->t_void; }
Type *type_int(TypeCtx *ctx) { return ctx->t_int; }
Type *type_bool(TypeCtx *ctx) { return ctx->t_bool; }
Type *type_f64(TypeCtx *ctx) { return ctx->t_f64; }
Type *type_string(TypeCtx *ctx) { return ctx->t_string; }

Type *type_any(TypeCtx *ctx) { return ctx->t_any; }

Type *type_new_param(TypeCtx *ctx, const char *name) {
    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->kind = TK_TYPEPARAM;
    t->pname = arena_strdup(ctx->arena, name);
    return t;
}

Type *type_ptr(TypeCtx *ctx, Type *base) {
    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->kind = TK_PTR;
    t->base = base;
    return t;
}

Type *type_array(TypeCtx *ctx, Type *elem, int len) {
    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->kind = TK_ARRAY;
    t->base = elem;
    t->len = len;
    return t;
}

Type *type_fnptr(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret) {
    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->kind = TK_FNPTR;
    t->ptypes = ptypes;
    t->nparams = nparams;
    t->ret = ret;
    return t;
}

Type *type_closure(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret) {
    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->kind = TK_CLOSURE;
    t->ptypes = ptypes;
    t->nparams = nparams;
    t->ret = ret;
    return t;
}

Type *type_mptr(TypeCtx *ctx, Type **ptypes, int nparams, Type *ret) {
    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->kind = TK_MPTR;
    t->ptypes = ptypes;
    t->nparams = nparams;
    t->ret = ret;
    return t;
}

UnionDef *type_define_union(TypeCtx *ctx, const char *name) {
    /* reuse a pre-registered (incomplete) union of the same name */
    for (int i = 0; i < ctx->nunion; i++) {
        if (strcmp(ctx->unions[i]->name, name) == 0) {
            /* As for a struct: a pre-scan completes the union so a `match` above
             * its declaration can check exhaustiveness, and the declaration
             * itself still has to be read. */
            if (ctx->unions[i]->complete && !ctx->unions[i]->prescanned)
                return NULL;
            return ctx->unions[i];
        }
    }
    UnionDef *ud = arena_alloc(ctx->arena, sizeof(UnionDef));
    ud->name = arena_strdup(ctx->arena, name);
    ud->complete = 0;
    if (ctx->nunion == ctx->union_cap) {
        int ncap = ctx->union_cap == 0 ? 8 : ctx->union_cap * 2;
        UnionDef **nu = arena_alloc_array(ctx->arena, (size_t)ncap, sizeof(UnionDef *));
        if (ctx->unions != NULL)
            memcpy(nu, ctx->unions, (size_t)ctx->nunion * sizeof(UnionDef *));
        ctx->unions = nu;
        ctx->union_cap = ncap;
    }
    ctx->unions[ctx->nunion++] = ud;
    return ud;
}

Type *type_find_union(TypeCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->nunion; i++) {
        if (strcmp(ctx->unions[i]->name, name) == 0) {
            Type *t = arena_alloc(ctx->arena, sizeof(Type));
            t->kind = TK_UNION;
            t->udef = ctx->unions[i];
            return t;
        }
    }
    return NULL;
}

VariantDef *type_find_variant(TypeCtx *ctx, const char *name, UnionDef **owner) {
    for (int i = 0; i < ctx->nunion; i++) {
        UnionDef *ud = ctx->unions[i];
        for (int v = 0; v < ud->nvariants; v++) {
            if (strcmp(ud->variants[v].name, name) == 0) {
                if (owner != NULL)
                    *owner = ud;
                return &ud->variants[v];
            }
        }
    }
    return NULL;
}

UnionDef *type_find_variant_owner(TypeCtx *ctx, const char *name) {
    UnionDef *owner = NULL;
    return type_find_variant(ctx, name, &owner) != NULL ? owner : NULL;
}

void union_add_variant(TypeCtx *ctx, UnionDef *ud, const char *name, Type **ftypes,
                       const char **fnames, int nfields) {
    int n = ud->nvariants;
    VariantDef *nv = arena_alloc_array(ctx->arena, (size_t)(n + 1), sizeof(VariantDef));
    if (ud->variants != NULL)
        memcpy(nv, ud->variants, (size_t)n * sizeof(VariantDef));
    ud->variants = nv;

    VariantDef *v = &nv[n];
    v->name = arena_strdup(ctx->arena, name);
    v->tag = n; /* discriminants are 0,1,2,... */
    v->nfields = nfields;
    v->fields = NULL;
    if (nfields > 0) {
        v->fields = arena_alloc_array(ctx->arena, (size_t)nfields, sizeof(Field));
        int off = 8; /* payload starts after the int tag at offset 0 */
        int align = 8;
        for (int i = 0; i < nfields; i++) {
            int fa = type_align(ftypes[i]);
            if (fa < 1)
                fa = 1;
            if (fa > align)
                align = fa;
            off = (off + fa - 1) / fa * fa;
            v->fields[i].name = arena_strdup(ctx->arena, fnames[i]);
            v->fields[i].type = ftypes[i];
            v->fields[i].offset = off;
            v->fields[i].is_prop = 0;
            off += type_size(ftypes[i]);
        }
        if (off > ud->size)
            ud->size = off;
        if (align > ud->align)
            ud->align = align;
    }
    ud->nvariants = n + 1;
}

void union_finish(UnionDef *ud) {
    if (ud->align < 8)
        ud->align = 8;
    if (ud->size < 8)
        ud->size = 8;
    ud->size = (ud->size + ud->align - 1) / ud->align * ud->align;
    ud->complete = 1;
}

/* A Result's payload is one machine word. `type_is_scalar` covers int, bool,
 * float, string and pointers -- everything Z can pass in a register -- which is
 * what keeps the layout uniform at sixteen bytes. */
static int result_payload_fits(Type *t) { return t != NULL && type_is_scalar(t); }

int type_is_result(Type *t) {
    return t != NULL && t->kind == TK_UNION && t->udef != NULL &&
           strncmp(t->udef->name, "Result<", 7) == 0;
}

Type *type_result_ok(Type *t) {
    if (!type_is_result(t) || t->udef->nvariants < 1 || t->udef->variants[0].nfields < 1)
        return NULL;
    return t->udef->variants[0].fields[0].type;
}

Type *type_result_err(Type *t) {
    if (!type_is_result(t) || t->udef->nvariants < 2 || t->udef->variants[1].nfields < 1)
        return NULL;
    return t->udef->variants[1].fields[0].type;
}

Type *type_result(TypeCtx *ctx, Type *ok, Type *err) {
    if (!result_payload_fits(ok) || !result_payload_fits(err))
        return NULL;
    /* Interned on the pair, so two mentions of `Result<int, string>` are the same
     * Type and `type_equals` says so. Without this, a function returning a Result
     * and a caller holding one would fail to match. */
    for (int i = 0; i < ctx->nresult; i++) {
        Type *r = ctx->results[i];
        if (type_equals(r->udef->variants[0].fields[0].type, ok) &&
            type_equals(r->udef->variants[1].fields[0].type, err))
            return r;
    }

    char nm[128];
    snprintf(nm, sizeof nm, "Result<%s, %s>", type_name(ctx, ok), type_name(ctx, err));
    UnionDef *ud = type_define_union(ctx, nm);
    if (ud == NULL) {
        /* type_define_union registers by name, and the name is derived from the
         * pair, so reaching here means the interning scan above missed. Falling
         * back to a fresh def keeps a compile going rather than crashing. */
        ud = arena_alloc(ctx->arena, sizeof *ud);
        ud->name = arena_strdup(ctx->arena, nm);
        ud->variants = NULL;
        ud->nvariants = 0;
        ud->size = 0;
        ud->align = 0;
        ud->complete = 0;
    }
    Type *ft[1];
    const char *fn[1];
    ft[0] = ok;
    fn[0] = "value";
    union_add_variant(ctx, ud, "Ok", ft, fn, 1);
    ft[0] = err;
    fn[0] = "error";
    union_add_variant(ctx, ud, "Err", ft, fn, 1);
    union_finish(ud);

    Type *t = arena_alloc(ctx->arena, sizeof *t);
    t->kind = TK_UNION;
    t->udef = ud;
    t->base = NULL;
    t->len = -1;
    t->sdef = NULL;
    t->pname = NULL;
    t->ptypes = NULL;
    t->nparams = 0;
    t->ret = NULL;

    if (ctx->nresult == ctx->result_cap) {
        int ncap = ctx->result_cap ? ctx->result_cap * 2 : 8;
        Type **nr = arena_alloc_array(ctx->arena, (size_t)ncap, sizeof(Type *));
        if (ctx->nresult > 0)
            memcpy(nr, ctx->results, (size_t)ctx->nresult * sizeof(Type *));
        ctx->results = nr;
        ctx->result_cap = ncap;
    }
    ctx->results[ctx->nresult++] = t;
    return t;
}

StructDef *type_define_struct(TypeCtx *ctx, const char *name) {
    /* If a pre-registered (still incomplete) struct with this name exists,
     * reuse it; a complete one means a true redefinition. */
    for (int i = 0; i < ctx->nstructs; i++) {
        if (strcmp(ctx->structs[i]->name, name) == 0) {
            /* A pre-scan marks the struct complete so that code above its
             * declaration can use it, but the declaration has still to be read.
             * Reuse it in that case; a struct that is complete and was not
             * prescanned really is being defined twice. */
            if (ctx->structs[i]->complete && !ctx->structs[i]->prescanned)
                return NULL;
            return ctx->structs[i];
        }
    }
    StructDef *sd = arena_alloc(ctx->arena, sizeof(StructDef));
    sd->name = arena_strdup(ctx->arena, name);
    sd->complete = 0;
    sd->prescanned = 0;
    sd->is_class = 0;
    sd->nvtable = 0;
    sd->vtable_impl = NULL;
    if (ctx->nstructs == ctx->cap) {
        int ncap = ctx->cap == 0 ? 8 : ctx->cap * 2;
        StructDef **ns = arena_alloc_array(ctx->arena, (size_t)ncap, sizeof(StructDef *));
        if (ctx->structs != NULL) {
            memcpy(ns, ctx->structs, (size_t)ctx->nstructs * sizeof(StructDef *));
        }
        ctx->structs = ns;
        ctx->cap = ncap;
    }
    ctx->structs[ctx->nstructs++] = sd;
    return sd;
}

Type *type_find_struct(TypeCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->nstructs; i++) {
        if (strcmp(ctx->structs[i]->name, name) == 0) {
            Type *t = arena_alloc(ctx->arena, sizeof(Type));
            t->kind = TK_STRUCT;
            t->sdef = ctx->structs[i];
            return t;
        }
    }
    return NULL;
}

void struct_add_field(TypeCtx *ctx, StructDef *sd, const char *name, Type *ftype) {
    int n = sd->nfields;
    Field *nf = arena_alloc_array(ctx->arena, (size_t)(n + 1), sizeof(Field));
    if (sd->fields != NULL)
        memcpy(nf, sd->fields, (size_t)n * sizeof(Field));
    sd->fields = nf;
    int align = type_align(ftype);
    if (align < 1)
        align = 1;
    int off = sd->size;
    off = (off + align - 1) / align * align; /* align field offset */
    nf[n].name = arena_strdup(ctx->arena, name);
    nf[n].type = ftype;
    nf[n].offset = off;
    nf[n].is_prop = 0;
    sd->size = off + type_size(ftype);
    if (align > sd->align)
        sd->align = align;
    sd->nfields = n + 1;
}

void struct_finish(StructDef *sd) {
    if (sd->align < 1)
        sd->align = 1;
    sd->size = (sd->size + sd->align - 1) / sd->align * sd->align;
    sd->complete = 1;
}

Field *struct_find_field(StructDef *sd, const char *name) {
    /* Only the type's own fields. A field used to be inherited from a base
     * class, which is why this walked the chain. */
    for (int i = 0; i < sd->nfields; i++)
        if (strcmp(sd->fields[i].name, name) == 0)
            return &sd->fields[i];
    return NULL;
}

/* Computes the vtable layout for a class after its methods are registered.
 * One slot per `virtual` method, in declaration order.
 *
 * This used to inherit the base's slots and let an `override` reuse its index,
 * so a subclass's vtable was the base's with entries replaced. With inheritance
 * gone there is no base to inherit from and no override to reuse a slot, and the
 * layout is simply the class's own virtuals in order. A vtable with no subclass
 * is still a working vtable: it is what a class stored in an interface calls
 * through. */
int class_finish_vtable(TypeCtx *ctx, StructDef *sd) {
    int cap = sd->nmethods + 1;
    sd->vtable_impl = arena_alloc_array(ctx->arena, (size_t)cap, sizeof(char *));
    sd->nvtable = 0;
    for (int i = 0; i < sd->nmethods; i++) {
        StructMethod *m = sd->methods[i];
        if (!m->is_virtual)
            continue;
        int slot = sd->nvtable++;
        m->vtable_index = slot;
        char *mang = arena_alloc(ctx->arena, strlen(sd->name) + strlen(m->name) + 3);
        snprintf(mang, strlen(sd->name) + strlen(m->name) + 3, "%s__%s", sd->name, m->name);
        sd->vtable_impl[slot] = mang;
    }
    return 0;
}

/* Adds an auto-property backing field and records it as a property. */
void struct_add_prop_field(TypeCtx *ctx, StructDef *sd, const char *name, Type *type) {
    struct_add_field(ctx, sd, name, type);
    Field *f = &sd->fields[sd->nfields - 1];
    f->is_prop = 1;
    struct_add_prop(ctx, sd, name, type, f->offset);
}

void struct_add_method(TypeCtx *ctx, StructDef *sd, StructMethod *m) {
    if (sd->nmethods == sd->method_cap) {
        int ncap = sd->method_cap == 0 ? 4 : sd->method_cap * 2;
        StructMethod **nm = arena_alloc_array(ctx->arena, (size_t)ncap, sizeof(StructMethod *));
        if (sd->methods != NULL)
            memcpy(nm, sd->methods, (size_t)sd->nmethods * sizeof(StructMethod *));
        sd->methods = nm;
        sd->method_cap = ncap;
    }
    sd->methods[sd->nmethods++] = m;
}

/* Destructors are registered under `dtor<Name>`. See struct_find_dtor. */
StructMethod *struct_find_dtor(StructDef *sd) {
    size_t n = strlen(sd->name);
    char buf[256];
    if (n + 5 > sizeof buf)
        return NULL;
    memcpy(buf, "dtor", 4);
    memcpy(buf + 4, sd->name, n + 1);
    return struct_find_method(sd, buf);
}

int type_needs_drop(Type *t) {
    if (t == NULL)
        return 0;
    if (t->kind == TK_STRING) {
        /* A string owns its bytes unless it is a literal, and a literal says so
         * in its header (cap == 0), so `z_str_free` can skip it without the
         * compiler having to know which of the two it is holding.
         *
         * This is the one owning type that needed no move semantics to become
         * sound, because a copy of a string is a copy of the bytes rather than a
         * second name for them: the parser turns every store of a borrowed
         * string into a `str_dup`, so two values never name one allocation. */
        return 1;
    }
    if (t->kind == TK_PTR) {
        /* A pointer to a *class* is a handle on a heap object, and a handle owns
         * what it points at -- that is the difference between a class and a
         * struct, and it is why `new Tracked()` has to be destroyed. A pointer
         * to a *struct* is a borrow: the struct is still in its owner's frame,
         * so the pointer owns nothing and dropping through it would free
         * something the frame still holds. */
        if (t->base != NULL && t->base->kind == TK_STRUCT && t->base->sdef != NULL &&
            t->base->sdef->is_class)
            return struct_find_dtor(t->base->sdef) != NULL;
        return 0;
    }
    if (t->kind == TK_ARRAY) {
        /* An array owns its elements, so an array of a class is owning. A fixed
         * array of a struct is not, unless the element is. */
        if (t->base == NULL)
            return 0;
        if (type_needs_drop(t->base))
            return 1;
        return t->base->kind == TK_STRUCT && t->base->sdef != NULL && t->base->sdef->is_class &&
               struct_find_dtor(t->base->sdef) != NULL;
    }
    if (t->kind != TK_STRUCT)
        return 0;
    StructDef *sd = t->sdef;
    if (sd == NULL)
        return 0;
    if (struct_find_dtor(sd) != NULL)
        return 1;
    for (int i = 0; i < sd->nfields; i++)
        if (type_needs_drop(sd->fields[i].type))
            return 1;
    return 0;
}

StructMethod *struct_find_method(StructDef *sd, const char *name) {
    /* NULL in, NULL out. This is the lookup for a type that may not exist, and a
     * caller asking whether a method is there has no type to ask about in the
     * case where the type's own declaration was rejected. */
    if (sd == NULL || name == NULL)
        return NULL;
    for (int i = 0; i < sd->nmethods; i++)
        if (strcmp(sd->methods[i]->name, name) == 0)
            return sd->methods[i];
    return NULL;
}

/* Returns the class that declares method `name`.
 *
 * With inheritance this searched the base chain, so a call to an inherited
 * method mangled against the class that declared it rather than the one it was
 * called through. A class has no base now, so the answer is always `sd` -- and
 * the function stays because a call site asking this question should not have
 * to know the answer is itself. */
StructDef *struct_method_owner(StructDef *sd, const char *name) {
    return struct_find_method(sd, name) != NULL ? sd : NULL;
}

void struct_add_prop(TypeCtx *ctx, StructDef *sd, const char *name, Type *type, int offset) {
    if (sd->nprops == sd->prop_cap) {
        int ncap = sd->prop_cap == 0 ? 4 : sd->prop_cap * 2;
        Field *np = arena_alloc_array(ctx->arena, (size_t)ncap, sizeof(Field));
        if (sd->props != NULL)
            memcpy(np, sd->props, (size_t)sd->nprops * sizeof(Field));
        sd->props = np;
        sd->prop_cap = ncap;
    }
    sd->props[sd->nprops].name = arena_strdup(ctx->arena, name);
    sd->props[sd->nprops].type = type;
    sd->props[sd->nprops].offset = offset;
    sd->nprops++;
}

int type_size(Type *t) {
    switch (t->kind) {
    case TK_VOID:
    /* `any` is only ever a built-in's declared parameter type, and the argument
     * is replaced by its real type before anything asks its size. A word is the
     * answer that cannot be wrong if that ever stops being true. */
    case TK_ANY:
        return 8;
    case TK_IFACE:
        /* One pointer: to the { itab, receiver } cell. */
        return 8;
    case TK_INT:
    case TK_BOOL:
    case TK_F64:
    case TK_STRING:
    case TK_PTR:
        return 8;
    case TK_ARRAY:
        return t->len < 0 ? 8 : t->len * type_size(t->base);
    case TK_STRUCT:
        return t->sdef->size;
    case TK_UNION:
        return t->udef->size;
    case TK_FNPTR:
    case TK_MPTR:
    case TK_CLOSURE:
    case TK_TYPEPARAM:
        return 8;
    }
    return 8;
}

int type_align(Type *t) {
    switch (t->kind) {
    case TK_ARRAY:
        return type_align(t->base);
    case TK_STRUCT:
        return t->sdef->align;
    case TK_UNION:
        return t->udef->align;
    case TK_VOID:
        return 1;
    default:
        return 8;
    }
}

/* True if a value of type `src` may be assigned to `dst`. Beyond exact
 * equality, this allows upcasting a derived class pointer (`Dog*`) to one of
 * its base class pointers (`Shape*`). */
int type_assignable(Type *dst, Type *src) {
    if (type_equals(dst, src))
        return 1;
    /* Any type may be assigned to an interface; whether this one actually
     * satisfies it is checked by the parser, which has the method signatures and
     * can say which method is missing. Answering "yes" here keeps the interface
     * out of every assignability test in the language. */
    if (dst != NULL && dst->kind == TK_IFACE)
        return 1;
    /* No upcast: a derived class used to be assignable to its base here, which
     * was the assignment that could silently slice or hand back a pointer to a
     * prefix of the object. Related types go in an interface instead, and that
     * is checked where the method signatures are. */
    return 0;
}

int type_equals(Type *a, Type *b) {
    /* `any` is what a polymorphic built-in's parameter is declared as, so it
     * matches whatever it is handed. Nothing else can produce it, so this cannot
     * make two unrelated types equal. */
    if ((a != NULL && a->kind == TK_ANY) || (b != NULL && b->kind == TK_ANY))
        return 1;
    if (a == b)
        return 1;
    if (a == NULL || b == NULL)
        return 0;
    if (a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case TK_VOID:
    case TK_INT:
    case TK_BOOL:
    case TK_F64:
    case TK_STRING:
    /* Unreachable: the `any` case is answered above, before the kinds are
     * compared. Listed so that adding a kind does not silently change what
     * `type_equals` says about two of them. */
    case TK_ANY:
        return 1;
    case TK_IFACE:
        return a->idef == b->idef;
    case TK_PTR:
        return type_equals(a->base, b->base);
    case TK_ARRAY:
        return a->len == b->len && type_equals(a->base, b->base);
    case TK_STRUCT:
        return a->sdef == b->sdef;
    case TK_UNION:
        return a->udef == b->udef;
    case TK_FNPTR:
    case TK_CLOSURE:
    case TK_MPTR: {
        if (a->nparams != b->nparams)
            return 0;
        if (!type_equals(a->ret, b->ret))
            return 0;
        for (int i = 0; i < a->nparams; i++)
            if (!type_equals(a->ptypes[i], b->ptypes[i]))
                return 0;
        return 1;
    }
    case TK_TYPEPARAM:
        return a == b;
    }
    return 0;
}

int type_is_scalar(Type *t) {
    /* A `float` is included: it is 8 bytes and occupies one slot, so everything
     * that reasons about a scalar's size is right about it. What it is *not* is
     * a general-purpose-register value, which is what type_is_float is for. */
    return t->kind == TK_INT || t->kind == TK_BOOL || t->kind == TK_F64 || t->kind == TK_STRING ||
           t->kind == TK_PTR;
}

int type_is_float(Type *t) { return t != NULL && t->kind == TK_F64; }

int type_is_callable(Type *t) {
    return t != NULL && (t->kind == TK_FNPTR || t->kind == TK_MPTR || t->kind == TK_CLOSURE);
}

int type_widens_to(Type *dst, Type *src) {
    /* int -> float only. Every int is exactly representable as a binary64, so
     * this cannot lose information, which is what makes it safe to do without
     * being asked. The reverse truncates and rounds, so it is never implicit. */
    return dst != NULL && src != NULL && dst->kind == TK_F64 && src->kind == TK_INT;
}

Type *type_arith_result(TypeCtx *ctx, Type *a, Type *b) {
    if (type_is_float(a) || type_is_float(b))
        return type_f64(ctx);
    return type_int(ctx);
}

/* Renders into a rotating set of static buffers so nested calls (e.g. two
 * type_name results in one message) do not alias. */
const char *type_name(TypeCtx *ctx, Type *t) {
    (void)ctx;
    static char bufs[8][128];
    static int next = 0;
    char *buf = bufs[next];
    next = (next + 1) % 8;

    if (t == NULL) {
        snprintf(buf, sizeof bufs[0], "<null>");
        return buf;
    }
    switch (t->kind) {
    case TK_VOID:
        snprintf(buf, sizeof bufs[0], "void");
        break;
    case TK_IFACE:
        snprintf(buf, sizeof bufs[0], "%s", t->idef != NULL ? t->idef->name : "?");
        break;
    case TK_INT:
        snprintf(buf, sizeof bufs[0], "int");
        break;
    case TK_BOOL:
        snprintf(buf, sizeof bufs[0], "bool");
        break;
    case TK_F64:
        snprintf(buf, sizeof bufs[0], "float");
        break;
    case TK_STRING:
        snprintf(buf, sizeof bufs[0], "string");
        break;
    case TK_PTR:
        snprintf(buf, sizeof bufs[0], "%s*", type_name(ctx, t->base));
        break;
    case TK_ARRAY:
        snprintf(buf, sizeof bufs[0], "%s[]", type_name(ctx, t->base));
        break;
    case TK_CLOSURE:
    case TK_MPTR:
    case TK_FNPTR: {
        /* fn(int, string) -> bool. type_name hands back one rotating slot at a
         * time, so this assembles the signature in a local and then copies it
         * into a slot of its own. */
        char tmp[256];
        const char *lead = t->kind == TK_CLOSURE ? "closure("
                           : t->kind == TK_MPTR  ? "method("
                                                 : "fn(";
        size_t off = (size_t)snprintf(tmp, sizeof tmp, "%s", lead);
        for (int i = 0; i < t->nparams && off < sizeof tmp; i++) {
            const char *pt = type_name(ctx, t->ptypes[i]);
            if (pt == NULL)
                pt = "?";
            off += (size_t)snprintf(tmp + off, sizeof tmp - off, "%s%s", i ? ", " : "", pt);
        }
        if (off < sizeof tmp) {
            /* type_name only returns NULL for a NULL type, which cannot
             * happen here, but the check keeps the warning quiet. */
            const char *rt = t->ret != NULL ? type_name(ctx, t->ret) : NULL;
            snprintf(tmp + off, sizeof tmp - off, ") -> %s", rt != NULL ? rt : "?");
        }
        /* A bound method and a closure print the way a function type does, with a
         * word in front. They share this branch because the three differ only in
         * what the second word of the cell holds, and a diagnostic that names
         * which of them it meant is more use than one that hides it.
         *
         * This used to ask type_name for its own argument, which is an infinite
         * loop -- nothing in a signature mentions the type being printed, so it
         * requested the same string again and again. It escaped notice only
         * because nothing in the test suite ever printed one. */
        size_t need = strlen(tmp) + 1;
        char *out = arena_alloc(ctx->arena, need);
        memcpy(out, tmp, need);
        return out;
    }
    case TK_STRUCT:
        snprintf(buf, sizeof bufs[0], "%s", t->sdef->name);
        break;
    case TK_UNION:
        snprintf(buf, sizeof bufs[0], "%s", t->udef->name);
        break;
    case TK_TYPEPARAM:
        snprintf(buf, sizeof bufs[0], "%s", t->pname ? t->pname : "T");
        break;
    case TK_ANY:
        snprintf(buf, sizeof bufs[0], "any");
        break;
    default:
        /* A kind with no name here is a kind this switch predates. Saying so is
         * better than returning whatever the rotating buffer last held, which is
         * what an unhandled case did: a diagnostic would name a type the reader
         * never wrote. */
        snprintf(buf, sizeof bufs[0], "<kind %d>", (int)t->kind);
        break;
    }
    return buf;
}

/* ---- interfaces ----
 *
 * An interface is a named, ordered list of method signatures. A value of
 * interface type is a pointer to a two-word cell, { itab, receiver }, where the
 * itab is a static array of code pointers -- one per method, in declaration
 * order. Keeping the value to a single word means it passes, stores and returns
 * like a pointer, so nothing in the aggregate machinery has to know about it.
 *
 * The order is the whole contract: a call site resolves a method name to an index
 * once, at compile time, and emits a load from that index. Two types implementing
 * the same interface agree on the layout because they both follow the interface's
 * declaration order. */

IfaceDef *type_define_iface(TypeCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->niface; i++) {
        if (strcmp(ctx->ifaces[i]->name, name) == 0)
            return ctx->ifaces[i];
    }
    IfaceDef *id = arena_alloc(ctx->arena, sizeof *id);
    id->name = arena_strdup(ctx->arena, name);
    id->methods = NULL;
    id->nmethods = 0;
    id->sealed = 0;
    id->prescanned = 0;
    if (ctx->niface == ctx->iface_cap) {
        int ncap = ctx->iface_cap ? ctx->iface_cap * 2 : 8;
        IfaceDef **ni = arena_alloc_array(ctx->arena, (size_t)ncap, sizeof(IfaceDef *));
        if (ctx->niface > 0)
            memcpy(ni, ctx->ifaces, (size_t)ctx->niface * sizeof(IfaceDef *));
        ctx->ifaces = ni;
        ctx->iface_cap = ncap;
    }
    ctx->ifaces[ctx->niface++] = id;
    return id;
}

Type *type_find_iface(TypeCtx *ctx, const char *name) {
    IfaceDef *id = NULL;
    for (int i = 0; i < ctx->niface; i++) {
        if (strcmp(ctx->ifaces[i]->name, name) == 0) {
            id = ctx->ifaces[i];
            break;
        }
    }
    return id != NULL ? type_iface(ctx, id) : NULL;
}

void iface_add_method(TypeCtx *ctx, IfaceDef *id, const char *name, Type *sig) {
    int n = id->nmethods;
    IfaceMethod *nm = arena_alloc_array(ctx->arena, (size_t)(n + 1), sizeof(IfaceMethod));
    if (id->methods != NULL)
        memcpy(nm, id->methods, (size_t)n * sizeof(IfaceMethod));
    id->methods = nm;
    nm[n].name = arena_strdup(ctx->arena, name);
    nm[n].sig = sig;
    id->nmethods = n + 1;
}

IfaceMethod *iface_find_method(IfaceDef *id, const char *name) {
    if (id == NULL)
        return NULL;
    for (int i = 0; i < id->nmethods; i++)
        if (strcmp(id->methods[i].name, name) == 0)
            return &id->methods[i];
    return NULL;
}

Type *type_iface(TypeCtx *ctx, IfaceDef *id) {
    Type *t = arena_alloc(ctx->arena, sizeof *t);
    t->kind = TK_IFACE;
    t->idef = id;
    t->base = NULL;
    t->len = -1;
    t->sdef = NULL;
    t->udef = NULL;
    t->pname = NULL;
    t->ptypes = NULL;
    t->nparams = 0;
    t->ret = NULL;
    return t;
}

/* 1 when the type declares a method with this name and exactly this signature.
 *
 * A struct's methods take the receiver by value and a class's take it by
 * pointer, so the first parameter is compared against the right thing for each
 * rather than being skipped: an interface that says `f(int, string)` is
 * satisfied by `int f(this int, string)` and not by anything else. */
static int tk_is(Type *t, TypeKind k) { return t != NULL && t->kind == k; }

static int method_matches(StructMethod *m, Type *sig) {
    if (m == NULL || !tk_is(sig, TK_FNPTR))
        return 0;
    /* `nparams` counts the declared parameters only: the receiver is not one of
     * them, which is exactly how the interface spells the same method. */
    if (m->nparams != sig->nparams)
        return 0;
    if (!type_equals(m->ret, sig->ret))
        return 0;
    for (int i = 0; i < sig->nparams; i++)
        if (!type_equals(m->ptypes[i], sig->ptypes[i]))
            return 0;
    return 1;
}

int iface_method_matches(StructMethod *m, Type *sig) { return method_matches(m, sig); }

int iface_implemented_by(TypeCtx *ctx, IfaceDef *id, Type *t, int *slots) {
    (void)ctx;
    StructDef *sd = NULL;
    if (tk_is(t, TK_STRUCT))
        sd = t->sdef;
    else if (tk_is(t, TK_PTR) && tk_is(t->base, TK_STRUCT))
        sd = t->base->sdef;
    if (sd == NULL)
        return 0;
    for (int i = 0; i < id->nmethods; i++) {
        StructMethod *m = struct_find_method(sd, id->methods[i].name);
        if (!method_matches(m, id->methods[i].sig))
            return 0;
        if (slots != NULL)
            slots[i] = m->vtable_index;
    }
    return 1;
}
