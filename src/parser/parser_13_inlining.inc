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

#define INLINE_MAX_STMT 24   /* statements in a body worth copying */
#define INLINE_MAX_DEPTH 4   /* how deep expansions may nest */
#define INLINE_MAX_PER_FN 64 /* expansions allowed in one function */

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
        return 0; /* the body is not this program's to copy */
    if (fn->is_generic_template)
        return 0; /* replaced by a monomorphized instance */
    if (fn->is_entry)
        return 0; /* nothing calls it, and it may not return */
    if (fn->is_ext)
        return 0; /* the receiver is a by-value struct; skip for now */
    if (fn->ret_type != NULL &&
        (is_kind(fn->ret_type, TK_STRUCT) || is_kind(fn->ret_type, TK_UNION)))
        return 0; /* a hidden result pointer the splice would have to rebuild */
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
        return 0; /* a closure in it names the frame this body no longer has */
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
            /* A body with destructors in it is not inlinable. The spliced copy
             * would tear down values in the caller's frame, and the callee's own
             * body would tear the same objects down again at its own exit -- two
             * destructor runs for one construction. See owns_drops. */
            if (it->owns_drops)
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

/* The gvars a program declared, in declaration order.
 *
 * The list lives on the Parser, which is a stack local of parse_program and is
 * gone by the time the driver asks. It is reachable from the program Stmt,
 * which is the one thing that outlives the call.
 *
 * Order matters twice over: the emitted table is indexed by it, so a use site's
 * index and the storage it names have to agree, and z_ginit assigns in it, so it
 * is also the order initializers run in. Both are declaration order because that
 * is the only order a reader can predict. */
static GlobalDef *program_globals(Stmt *program) {
    return program != NULL ? (GlobalDef *)program->globals_head : NULL;
}

int parse_globals(Arena *arena, Stmt *program, ParsedGlobal *out, int cap) {
    (void)arena;
    GlobalDef *g = program_globals(program);
    int n = 0;
    for (; g != NULL; g = g->next) {
        if (out == NULL || n >= cap) {
            n++;
            continue;
        }
        ParsedGlobal *pg = &out[n];
        pg->name = g->name;
        pg->type = g->type;
        pg->init = g->init_is_const ? NULL : g->init;
        pg->span = g->decl_span;
        pg->is_const = g->init_is_const;
        pg->ival = 0;
        pg->dval = 0.0;
        if (g->init_is_const && g->init != NULL) {
            if (g->init->kind == E_INT || g->init->kind == E_BOOL)
                pg->ival = g->init->ival;
            else if (g->init->kind == E_F64)
                pg->dval = g->init->dval;
        }
        /* Assigned by the driver, which owns the symbol namespace: a global's
         * symbol has to be mangled the same way a function's is, and that
         * mangling is the codegen module's business. */
        pg->sym = NULL;
        n++;
    }
    return n;
}
