/* ---- Local register allocation ------------------------------------------ */

/* Returns the LocalInfo for a frame slot, creating it on first sight. */
static LocalInfo *li_for(CG *cg, int slot) {
    for (int i = 0; i < cg->nlocals; i++)
        if (cg->locals[i].slot == slot)
            return &cg->locals[i];
    if (cg->nlocals == cg->loc_cap) {
        if (cg->loc_cap > INT_MAX / 2)
            die_oom();
        int ncap = cg->loc_cap ? cg->loc_cap * 2 : 16;
        if ((size_t)ncap > SIZE_MAX / sizeof(LocalInfo))
            die_oom();
        cg->loc_cap = ncap;
        cg->locals = realloc(cg->locals, (size_t)cg->loc_cap * sizeof(LocalInfo));
        if (cg->locals == NULL)
            die_oom();
    }
    LocalInfo *li = &cg->locals[cg->nlocals++];
    li->slot = slot;
    li->eligible = 1;
    li->d = li->u = -1;
    li->assigned = -1;
    li->const_cand = 0;
    li->weight = 0;
    li->colored = 0;
    li->const_val = 0;
    li->is_float = 0;
    li->reassigned = 0;
    li->is_const = 0;
    return li;
}

/* Marks every local referenced in a subtree as ineligible for a register,
 * because the expression takes its address (gen_addr would need the real
 * memory location).
 *
 * `idx` is the statement the subtree sits in, and it is the statement's own index
 * rather than a constant. It used to be a constant, and the index is what the
 * allocator colours intervals from, so a local whose only use was inside an
 * addressed subtree had that use recorded at statement zero. A local defined at
 * statement zero then had the range `[0, 0]`, which is a valid range, and was
 * given a register -- and the addressed expression went on reading the frame slot
 * that the register had made unreachable. */
static void mark_addr_taken(CG *cg, Expr *e, int idx);

static void walk_alloc_expr(CG *cg, Expr *e, int idx);

static void mark_addr_taken(CG *cg, Expr *e, int idx) {
    if (e == NULL)
        return;
    if (e->kind == E_VAR) {
        li_for(cg, e->slot)->eligible = 0;
        return;
    }
    walk_alloc_expr(cg, e, idx);
}

/* The weight of one access at the current loop depth: 8 per enclosing loop,
 * capped so a deeply nested body cannot overflow the sum. */
static long long loop_weight(CG *cg) {
    int d = cg->alloc_loop_depth > 6 ? 6 : cg->alloc_loop_depth;
    return 1LL << (3 * d);
}

/* Records a use of a local at statement index idx. */
static void li_use(CG *cg, int slot, int idx) {
    LocalInfo *li = li_for(cg, slot);
    if (li->u < idx)
        li->u = idx;
    li->loop_marker = cg->loop_marker;
    li->weight += loop_weight(cg);
}

/* Records a definition of a local at statement index idx. */
static void li_def(CG *cg, int slot, int idx) {
    LocalInfo *li = li_for(cg, slot);
    if (li->d < 0 || idx < li->d)
        li->d = idx;
    if (li->u < idx)
        li->u = idx;
    li->loop_marker = cg->loop_marker;
    li->weight += loop_weight(cg);
}

/* Walks an expression to record defs/uses and disqualify address-taken
 * locals. The context flag `assign_lhs` marks the direct left-hand side of an
 * assignment, which is the one place a plain local's memory location is used;
 * those are handled explicitly in gen_expr. */
static void walk_alloc_expr(CG *cg, Expr *e, int idx) {
    if (e == NULL)
        return;
    switch (e->kind) {
    case E_VAR:
        /* A global is not in the allocator: it has no slot, and slot -1 handed
         * to li_for would create a LocalInfo for it, which the register
         * allocator would then keep. Reads and writes would both go to that
         * invented register and the global in memory would never change. */
        if (e->gidx < 0)
            li_use(cg, e->slot, idx);
        break;
    case E_ADDR:
        /* `&x` materializes the value of x; for a scalar that is a load, but
         * conservatively treat it as address-taken. */
        mark_addr_taken(cg, e->lhs, idx);
        break;
    case E_INDEX:
        /* The index reads its base for the value, which is the pointer to the
         * elements, so an array or string local does not need an address. Only
         * an aggregate base, which is represented by its address, does. Marking
         * every base made any array local ineligible for a register, and the
         * base of a matmul inner loop was then reloaded from the frame on every
         * element access. */
        if (is_aggregate(e->lhs->type))
            mark_addr_taken(cg, e->lhs, idx);
        else
            walk_alloc_expr(cg, e->lhs, idx);
        walk_alloc_expr(cg, e->rhs, idx);
        break;
    case E_POSTINC:
        /* The variable is written, so an element or field operand is addressed
         * and its base is marked. Without this a `d[n++]++` would allocate no
         * slot for the base it has to compute an address from. */
        mark_addr_taken(cg, e->lhs, idx);
        walk_alloc_expr(cg, e->lhs, idx);
        break;
    case E_STRLEN:
        /* The length is in the header the value points into, which is a load
         * from the value: the string itself is not addressed. */
        walk_alloc_expr(cg, e->lhs, idx);
        break;
    case E_SLICE:
        walk_alloc_expr(cg, e->lhs, idx);
        walk_alloc_expr(cg, e->rhs, idx);
        walk_alloc_expr(cg, e->env, idx);
        break;
    case E_FIELD:
        mark_addr_taken(cg, e->lhs, idx); /* base is addressed */
        break;
    case E_DEREF:
        walk_alloc_expr(cg, e->lhs, idx); /* pointer value */
        break;
    case E_ASSIGN:
        if (e->lhs != NULL && e->lhs->kind == E_VAR && e->lhs->gidx < 0) {
            li_def(cg, e->lhs->slot, idx);
            li_for(cg, e->lhs->slot)->reassigned = 1;
        } else if (e->lhs != NULL && e->lhs->kind == E_VAR) {
            /* A global destination: no slot to define. mark_addr_taken is not
             * wanted either -- it would disqualify a base that has no frame
             * slot -- and there is nothing else to record, because the write is
             * visible to every function in the program. */
        } else {
            /* The one that shows. Unlike the four above, this walks the
             * destination without marking it ineligible, because the
             * destination is written rather than read. So the index of
             * `a[i] = t` was the only local here whose range could be invented,
             * and it is the one whose address the assignment has to compute. */
            mark_addr_taken(cg, e->lhs, idx);
        }
        walk_alloc_expr(cg, e->rhs, idx);
        break;
    case E_STRUCTLIT:
    case E_UNIONLIT:
        for (int i = 0; i < e->nargs; i++)
            walk_alloc_expr(cg, e->args[i], idx);
        break;
    case E_CALL:
        for (int i = 0; i < e->nargs; i++)
            walk_alloc_expr(cg, e->args[i], idx);
        break;
    case E_MATCH:
        walk_alloc_expr(cg, e->lhs, idx); /* scrutinee */
        for (int i = 0; i < e->narms; i++) {
            MatchArm *arm = &e->arms[i];
            /* Payload bindings are written directly by match codegen, so keep
             * them in memory. */
            for (int b = 0; b < arm->nbind; b++)
                li_for(cg, arm->bind_slots[b])->eligible = 0;
            walk_alloc_expr(cg, arm->body, idx);
        }
        break;
    case E_TERNARY:
        walk_alloc_expr(cg, e->lhs, idx);
        walk_alloc_expr(cg, e->rhs, idx);
        if (e->nargs > 0)
            walk_alloc_expr(cg, e->args[0], idx);
        break;
    default:
        walk_alloc_expr(cg, e->lhs, idx);
        walk_alloc_expr(cg, e->rhs, idx);
        /* Any remaining expression form (E_CALL/E_VCALL/E_NEWCLASS/...) may
         * hold sub-expressions (receiver, arguments) that reference locals;
         * walk them all so liveness is never under-counted. */
        for (int i = 0; i < e->nargs; i++)
            walk_alloc_expr(cg, e->args[i], idx);
        break;
    }
}

static void walk_alloc_stmt(CG *cg, Stmt *s, int *idx);

/* Stretches the live range of every local that one loop touched out to the end
 * of that loop.
 *
 * The allocator colours [def, use] intervals, and a loop is not a line: a value
 * read in the condition is read again on the next iteration, and one assigned in
 * the step is live all the way round. Left as plain intervals, a local read only
 * in the condition has its last use *before* the body, so the body looks like a
 * place where its register is free -- and the first body local the allocator
 * sees is handed the very register the condition is read back from. The loop
 * then runs one iteration and exits with the wrong value, which is what a
 * growable vector's rehash looked like: correct at -O0, and one entry left after
 * the first growth at -O1.
 *
 * Only locals this loop actually touched are extended, so one the loop never
 * mentions is not penalised and the allocator stays as aggressive outside loops
 * as it was before. `marker` is the counter value this loop's walk stamped on
 * them, and `start`/`end` bracket it. */
static void extend_live_across_loop(CG *cg, int marker, int start, int end) {
    for (int i = 0; i < cg->nlocals; i++) {
        LocalInfo *li = &cg->locals[i];
        if (li->loop_marker != marker)
            continue; /* this loop never mentioned it */
        if (li->d < 0 || li->d >= start)
            continue; /* defined inside the loop, so already contained */
        if (li->u < end)
            li->u = end;
    }
}

static void walk_alloc_block(CG *cg, Stmt *blk, int *idx) {
    if (blk == NULL)
        return;
    for (int i = 0; i < blk->nitems; i++)
        walk_alloc_stmt(cg, blk->items[i], idx);
}

static void walk_alloc_stmt(CG *cg, Stmt *s, int *idx) {
    if (s == NULL)
        return;
    int cur = (*idx)++;
    switch (s->kind) {
    case S_LEAVE:
        return; /* a jump: no definition, no use */
    case S_VAR:
        if (s->init != NULL)
            walk_alloc_expr(cg, s->init, cur);
        if (s->slot > 0) {
            li_def(cg, s->slot, cur);
            if (s->boxed) {
                /* A captured local is neither a constant nor a register
                 * candidate. Constant propagation is the dangerous one: a
                 * `var n = 0` that a closure increments is never reassigned in
                 * *this* function -- the assignment is inside the hoisted lambda
                 * -- so it looks like the literal 0, and the declaration is
                 * dropped along with the box the environment points at. */
                LocalInfo *bi = li_for(cg, s->slot);
                bi->const_cand = 0;
                bi->eligible = 0;
            }
            /* Record the floatness once, at the declaration, which is the only
             * place the declared type is known. Everything downstream asks
             * local_reg, which consults this. */
            if (type_is_float(s->type))
                li_for(cg, s->slot)->is_float = 1;
            /* A local initialized with a plain integer constant is a candidate
             * for compile-time propagation (finalized in alloc_regs). */
            if (s->init != NULL && s->init->kind == E_INT && !is_aggregate(s->type)) {
                LocalInfo *li = li_for(cg, s->slot);
                li->const_cand = 1;
                li->const_val = s->init->ival;
            }
        }
        break;
    case S_EXPR:
        walk_alloc_expr(cg, s->expr, cur);
        break;
    case S_RETURN:
        walk_alloc_expr(cg, s->expr, cur);
        break;
    case S_IF:
        walk_alloc_expr(cg, s->cond, cur);
        walk_alloc_block(cg, s->body, idx);
        walk_alloc_block(cg, s->orelse, idx);
        break;
    case S_WHILE: {
        int marker = ++cg->loop_marker;
        int loop_start = *idx;
        cg->alloc_loop_depth++;
        walk_alloc_expr(cg, s->cond, cur);
        walk_alloc_block(cg, s->body, idx);
        cg->alloc_loop_depth--;
        extend_live_across_loop(cg, marker, loop_start, *idx);
        break;
    }
    case S_FOR: {
        /* Each phase needs its own statement index. A for-loop variable is
         * defined in the init, read in the condition, and live all the way
         * through the body to the step; giving the whole loop one index would
         * make its interval [i,i] and let a body local share the register. */
        int marker = ++cg->loop_marker;
        int loop_start = *idx;
        walk_alloc_stmt(cg, s->for_init, idx);
        cg->alloc_loop_depth++;
        walk_alloc_expr(cg, s->cond, (*idx)++);
        walk_alloc_block(cg, s->body, idx);
        walk_alloc_expr(cg, s->for_step, (*idx)++);
        cg->alloc_loop_depth--;
        extend_live_across_loop(cg, marker, loop_start, *idx);
        break;
    }
    case S_BLOCK:
        walk_alloc_block(cg, s, idx);
        break;
    case S_BREAK:
    case S_CONTINUE:
    case S_FUNC:
    case S_STRUCT:
    case S_UNION:
        break;
    }
}

/* Assigns pool registers to eligible locals using interval colouring: two
 * locals whose [def, use] statement ranges overlap receive different
 * registers, so a register is never live for two locals at once. */
static void alloc_regs(CG *cg, Stmt *fn) {
    cg->nlocals = 0;
    cg->pool_mask = 0;
    if (fn == NULL)
        return;
    /* Parameters stay in memory (their values arrive in argument registers and
     * are spilled in the prologue). A float parameter arrives in an XMM
     * register and is spilled too, but by a different instruction, so the value
     * is a double rather than a 64-bit integer. */
    for (int i = 0; i < fn->nparams; i++)
        li_for(cg, fn->params[i]->slot)->eligible = 0;

    int idx = 0;
    walk_alloc_block(cg, fn->fbody, &idx);

    /* Which locals are compile-time constants is a property of the source rather
     * than of the optimizer, and the load and store paths use it at every level,
     * so it is settled here for all of them.
     *
     * Handing out *registers* is level-dependent, and -O0 must not do it: there
     * `local_reg` refuses to name one, so the prologue would save rbx..r15 and the
     * epilogue restore them with nothing in between ever reading them. It costs
     * twice over, because a local holding a register is taken to have no address,
     * so the debug info pass leaves it out -- and at -O0 it does have an address,
     * and a debugger watching a -O0 build could not see the variable at all.
     *
     * The walk above still runs, because the debug info pass needs the slot list
     * it builds whether or not anything ends up in a register. */
    for (int i = 0; i < cg->nlocals; i++) {
        LocalInfo *li = &cg->locals[i];
        li->assigned = -1;
        /* Const propagation is only safe when the local is never reassigned
         * AND its address is never taken (else it can change through a
         * pointer behind our back) and it is a plain scalar. */
        li->is_const = li->const_cand && !li->reassigned && li->eligible;
    }
    if (!opt_on(cg))
        return;

    /* Greedy colouring, heaviest local first, skipping the locals already
     * settled as constants: they need no storage and no register. The overlap
     * test below is what keeps two locals from sharing a register, and it does
     * not depend on the order, so only which locals win a register changes. Ties
     * go to the earlier definition, as they did before weights existed. */
    for (;;) {
        int best = -1;
        for (int i = 0; i < cg->nlocals; i++) {
            LocalInfo *li = &cg->locals[i];
            if (li->colored || li->is_const)
                continue;
            if (best < 0 || li->weight > cg->locals[best].weight)
                best = i;
        }
        if (best < 0)
            break;
        LocalInfo *li = &cg->locals[best];
        li->colored = 1;
        if (!li->eligible || li->d < 0 || li->u < li->d)
            continue;
        for (int r = 0; r < NPOOL; r++) {
            int clash = 0;
            for (int j = 0; j < cg->nlocals; j++) {
                LocalInfo *o = &cg->locals[j];
                if (o == li || o->assigned != r || !o->eligible)
                    continue;
                if (o->d < 0 || o->u < o->d)
                    continue;
                if (o->d <= li->u && li->d <= o->u) { /* overlapping ranges */
                    clash = 1;
                    break;
                }
            }
            if (!clash) {
                li->assigned = r;
                cg->pool_mask |= (1 << r);
                break;
            }
        }
    }
}

/* Returns the LocalInfo for a frame slot, or NULL. */
static LocalInfo *li_lookup(CG *cg, int slot) {
    for (int i = 0; i < cg->nlocals; i++)
        if (cg->locals[i].slot == slot)
            return &cg->locals[i];
    return NULL;
}

/* Folds `e` to a compile-time integer constant if every leaf is a literal or a
 * propagated-constant local. Returns 1 on success. */
static int const_fold_val(CG *cg, Expr *e, long long *out) {
    if (e == NULL)
        return 0;
    if (!opt_on(cg))
        return 0; /* -O0 does no constant folding */
    switch (e->kind) {
    case E_NULL:
        *out = 0;
        return 1;
    case E_INT:
    case E_BOOL:
        *out = e->ival;
        return 1;
    case E_VAR: {
        if (e->agg_param || e->gidx >= 0)
            return 0;
        LocalInfo *li = li_lookup(cg, e->slot);
        if (li != NULL && li->is_const) {
            *out = li->const_val;
            return 1;
        }
        return 0;
    }
    case E_UNARY: {
        long long v;
        if (!const_fold_val(cg, e->lhs, &v))
            return 0;
        if (e->op == T_MINUS) {
            *out = -v;
            return 1;
        }
        if (e->op == T_TILDE) {
            *out = ~v;
            return 1;
        }
        return 0;
    }
    case E_BINARY: {
        long long a, b;
        if (!const_fold_val(cg, e->lhs, &a) || !const_fold_val(cg, e->rhs, &b))
            return 0;
        switch (e->op) {
        case T_PLUS:
            *out = a + b;
            return 1;
        case T_MINUS:
            *out = a - b;
            return 1;
        case T_STAR:
            *out = a * b;
            return 1;
        case T_SLASH:
            if (b == 0)
                return 0;
            *out = a / b;
            return 1;
        case T_PERCENT:
            if (b == 0)
                return 0;
            *out = a % b;
            return 1;
        case T_LT:
            *out = a < b;
            return 1;
        case T_LE:
            *out = a <= b;
            return 1;
        case T_GT:
            *out = a > b;
            return 1;
        case T_GE:
            *out = a >= b;
            return 1;
        case T_EQ:
            *out = a == b;
            return 1;
        case T_NE:
            *out = a != b;
            return 1;
        case T_AMP:
            *out = a & b;
            return 1;
        case T_PIPE:
            *out = a | b;
            return 1;
        case T_CARET:
            *out = a ^ b;
            return 1;
        case T_SHL:
            if (b < 0 || b > 63)
                return 0;
            *out = a << b;
            return 1;
        case T_SHR:
            if (b < 0 || b > 63)
                return 0;
            *out = a >> b;
            return 1;
        default:
            return 0;
        }
    }
    default:
        return 0;
    }
}

/* Returns the pool register holding `slot`'s value, or NULL. */
/* The register an eligible local was assigned, or NULL when it stays in
 * memory. -O0 never allocates one, so every local is a memory reference. */
static const char *local_reg(CG *cg, int slot) {
    if (!opt_on(cg))
        return NULL;
    for (int i = 0; i < cg->nlocals; i++) {
        if (cg->locals[i].slot != slot || cg->locals[i].assigned < 0)
            continue;
        if (cg->locals[i].is_float)
            return NULL; /* stays in the frame; see LocalInfo.is_float */
        return POOL_REGS[cg->locals[i].assigned];
    }
    return NULL;
}

/* True if `e` is a leaf that can be materialized into a register with a single
 * instruction, without using rax (so a left operand already in rax survives). */
static int is_leaf_expr(Expr *e) {
    if (e->kind == E_INT || e->kind == E_BOOL)
        return 1;
    if (e->kind == E_VAR && !e->agg_param && !is_aggregate(e->type))
        return 1;
    return 0;
}

/* Emits a leaf `e` (already known to satisfy is_leaf_expr) into `reg`. */
/* The register a leaf currently occupies, or NULL if it has none: not a local,
 * a propagated constant (which lives in the instruction stream, not a register),
 * or something that was spilled. Lets an operator name the operand instead of
 * copying it into a scratch register first. */
static const char *leaf_live_reg(CG *cg, Expr *e) {
    /* `boxed` is not an optional check here. A captured variable's slot holds a
     * pointer to a heap cell, and the register that slot was allocated holds that
     * same pointer, so naming it as "the register the operand lives in" hands the
     * caller a box pointer where a value is expected. `a + a` over a captured `a`
     * added the pointer to itself. */
    if (e == NULL || e->kind != E_VAR || e->agg_param || e->boxed || is_aggregate(e->type))
        return NULL;
    LocalInfo *li = li_lookup(cg, e->slot);
    if (li != NULL && li->is_const)
        return NULL;
    return local_reg(cg, e->slot);
}

static void gen_leaf_to_reg(CG *cg, Expr *e, const char *reg) {
    if (e->kind == E_INT || e->kind == E_BOOL) {
        buf_printf(cg->out, "  mov %s, %lld\n", reg, e->ival);
        return;
    }
    /* Answered before the allocator is consulted, because a global has no slot
     * to be keyed by and -1 would be looked up as a local. */
    const char *gsym = global_sym(cg, e->gidx);
    if (gsym != NULL) {
        if (is_kind(e->type, TK_F64))
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rip + %s]\n", reg, gsym);
        else
            buf_printf(cg->out, "  mov %s, QWORD PTR [rip + %s]\n", reg, gsym);
        return;
    }
    /* A captured variable has to be loaded through its box, exactly as the E_VAR
     * arm of gen_expr does it. Reaching here with a boxed slot meant loading the
     * pointer instead, which is the same defect leaf_live_reg has to refuse. */
    if (e->boxed) {
        buf_printf(cg->out, "  mov r11, QWORD PTR [rbp - %d]\n", e->slot);
        if (is_kind(e->type, TK_F64))
            buf_printf(cg->out, "  movsd %s, QWORD PTR [r11]\n", reg);
        else
            buf_printf(cg->out, "  mov %s, QWORD PTR [r11]\n", reg);
        return;
    }
    LocalInfo *li = li_lookup(cg, e->slot);
    if (li != NULL && li->is_const) {
        buf_printf(cg->out, "  mov %s, %lld\n", reg, li->const_val);
        return;
    }
    const char *lreg = local_reg(cg, e->slot);
    if (lreg != NULL)
        buf_printf(cg->out, "  mov %s, %s\n", reg, lreg);
    else
        buf_printf(cg->out, "  mov %s, QWORD PTR [rbp - %d]\n", reg, e->slot);
}
