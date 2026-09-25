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
    ctx->t_string = arena_alloc(arena, sizeof(Type));
    ctx->t_string->kind = TK_STRING;
}

Type *type_void(TypeCtx *ctx) { return ctx->t_void; }
Type *type_int(TypeCtx *ctx) { return ctx->t_int; }
Type *type_bool(TypeCtx *ctx) { return ctx->t_bool; }
Type *type_string(TypeCtx *ctx) { return ctx->t_string; }

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

UnionDef *type_define_union(TypeCtx *ctx, const char *name) {
    /* reuse a pre-registered (incomplete) union of the same name */
    for (int i = 0; i < ctx->nunion; i++) {
        if (strcmp(ctx->unions[i]->name, name) == 0) {
            if (ctx->unions[i]->complete)
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

StructDef *type_define_struct(TypeCtx *ctx, const char *name) {
    /* If a pre-registered (still incomplete) struct with this name exists,
     * reuse it; a complete one means a true redefinition. */
    for (int i = 0; i < ctx->nstructs; i++) {
        if (strcmp(ctx->structs[i]->name, name) == 0) {
            if (ctx->structs[i]->complete)
                return NULL;
            return ctx->structs[i];
        }
    }
    StructDef *sd = arena_alloc(ctx->arena, sizeof(StructDef));
    sd->name = arena_strdup(ctx->arena, name);
    sd->complete = 0;
    sd->is_class = 0;
    sd->base = NULL;
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
    /* Search the class itself, then base classes (inherited fields live at
     * lower offsets). */
    for (; sd != NULL; sd = sd->base)
        for (int i = 0; i < sd->nfields; i++)
            if (strcmp(sd->fields[i].name, name) == 0)
                return &sd->fields[i];
    return NULL;
}

/* Computes the vtable layout for a class after its methods are registered.
 * Inherits the base's virtual slots (same indices), then appends new virtuals.
 * Each slot stores the most-derived implementation name for this class.
 * Returns 0 on success, -1 if a method is declared `override` but no matching
 * base virtual exists. */
int class_finish_vtable(TypeCtx *ctx, StructDef *sd) {
    int base_n = sd->base ? sd->base->nvtable : 0;
    int cap = base_n + sd->nmethods + 1;
    sd->vtable_impl = arena_alloc_array(ctx->arena, (size_t)cap, sizeof(char *));
    for (int i = 0; i < base_n; i++)
        sd->vtable_impl[i] = sd->base->vtable_impl[i];
    sd->nvtable = base_n;
    for (int i = 0; i < sd->nmethods; i++) {
        StructMethod *m = sd->methods[i];
        if (!m->is_virtual)
            continue;
        int slot = -1;
        if (m->is_override) {
            /* Occupy the base's slot for the overridden method. */
            StructMethod *bm = sd->base ? struct_find_method(sd->base, m->name) : NULL;
            if (bm == NULL || !bm->is_virtual)
                return -1;
            slot = bm->vtable_index;
        } else {
            slot = sd->nvtable++;
        }
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

StructMethod *struct_find_method(StructDef *sd, const char *name) {
    /* Search the class itself, then base classes (inheritance). */
    for (; sd != NULL; sd = sd->base)
        for (int i = 0; i < sd->nmethods; i++)
            if (strcmp(sd->methods[i]->name, name) == 0)
                return sd->methods[i];
    return NULL;
}

/* Returns the class that actually declares method `name` (searching sd then its
 * bases), so calls to inherited methods mangle against the declaring class. */
StructDef *struct_method_owner(StructDef *sd, const char *name) {
    for (; sd != NULL; sd = sd->base)
        for (int i = 0; i < sd->nmethods; i++)
            if (strcmp(sd->methods[i]->name, name) == 0)
                return sd;
    return NULL;
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
        return 0;
    case TK_INT:
    case TK_BOOL:
    case TK_STRING:
    case TK_PTR:
        return 8;
    case TK_ARRAY:
        return t->len < 0 ? 8 : t->len * type_size(t->base);
    case TK_STRUCT:
        return t->sdef->size;
    case TK_UNION:
        return t->udef->size;
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
    if (dst != NULL && src != NULL && dst->kind == TK_PTR && src->kind == TK_PTR &&
        dst->base != NULL && src->base != NULL && dst->base->kind == TK_STRUCT &&
        src->base->kind == TK_STRUCT && dst->base->sdef && src->base->sdef) {
        StructDef *b = dst->base->sdef;
        for (StructDef *d = src->base->sdef; d != NULL; d = d->base)
            if (d == b)
                return 1;
    }
    return 0;
}

int type_equals(Type *a, Type *b) {
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
    case TK_STRING:
        return 1;
    case TK_PTR:
        return type_equals(a->base, b->base);
    case TK_ARRAY:
        return a->len == b->len && type_equals(a->base, b->base);
    case TK_STRUCT:
        return a->sdef == b->sdef;
    case TK_UNION:
        return a->udef == b->udef;
    case TK_TYPEPARAM:
        return a == b;
    }
    return 0;
}

int type_is_scalar(Type *t) {
    return t->kind == TK_INT || t->kind == TK_BOOL || t->kind == TK_STRING || t->kind == TK_PTR;
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
    case TK_INT:
        snprintf(buf, sizeof bufs[0], "int");
        break;
    case TK_BOOL:
        snprintf(buf, sizeof bufs[0], "bool");
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
    case TK_STRUCT:
        snprintf(buf, sizeof bufs[0], "%s", t->sdef->name);
        break;
    case TK_UNION:
        snprintf(buf, sizeof bufs[0], "%s", t->udef->name);
        break;
    case TK_TYPEPARAM:
        snprintf(buf, sizeof bufs[0], "%s", t->pname ? t->pname : "T");
        break;
    }
    return buf;
}
