/* ---- loop-invariant code motion (levels 2 and up) ----
 *
 * An arithmetic expression over constants and locals that the loop body never
 * assigns to has the same value on every iteration, so it can be computed once
 * ahead of the loop. This is the largest single win available for the shape of
 * loop Z is written for, and it is what gcc -O1 already does.
 *
 * Scope is deliberately narrow, because a wrong hoist is a miscompile:
 *
 *   * Only side-effect-free integer arithmetic over locals and literals moves.
 *     Division and modulo are excluded even when invariant -- they can trap,
 *     and moving a trap out of a loop changes which iterations fail, so the
 *     operation has to stay where it was.
 *   * Nothing with an unknown effect is descended into for hoisting, so the
 *     "no operand is written by the body" test is sufficient: Z integers do
 *     not alias, and a call is treated as clobbering everything.
 *   * The body is walked including nested loops, so an expression invariant in
 *     the outer loop is hoisted to the outer loop. An inner loop then finds
 *     only what the outer one could not move, which is exactly right.
 *
 * Hoisted values take a fresh frame slot numbered the way the parser would
 * number a local declared next, so they sit below the temporaries. The measure
 * pass runs the same walk and allocates the same slots, so the frame is sized
 * for them without a second mechanism.
 */

/* Operators that are pure integer arithmetic and cannot trap. */
static int licm_op_is_safe(TokenKind op) {
    switch (op) {
    case T_PLUS:
    case T_MINUS:
    case T_STAR:
    case T_AMP:
    case T_PIPE:
    case T_CARET:
    case T_SHL:
    case T_SHR:
        return 1;
    default:
        return 0;
    }
}

#define LICM_MAX_SLOT 256

/* True when the expression may have a side effect, in which case everything it
 * touches is treated as written. Being conservative here is what keeps the
 * rest of the pass simple. */
static int licm_may_write(Expr *e) {
    if (e == NULL)
        return 0;
    switch (e->kind) {
    case E_ASSIGN:
    case E_POSTINC:
    case E_CALL:
    case E_VCALL:
    case E_ICALL:
    case E_NEW:
    case E_NEWCLASS:
    case E_INTRINSIC:
    case E_FNPTR:
    case E_MPTR:
    case E_STRUCTLIT:
    case E_UNIONLIT:
        return 1;
    default:
        /* A match's arms are where its writes are. Reaching only `lhs` and `rhs`
         * -- the subject and nothing else -- asked what the *test* does and
         * ignored what the arms do, so a body that only ever assigns inside a
         * `match` arm looked like it wrote nothing at all. */
        if (licm_may_write(e->lhs) || licm_may_write(e->rhs))
            return 1;
        for (int i = 0; i < e->narms; i++)
            if (licm_may_write(e->arms[i].body))
                return 1;
        return 0;
    }
}

static void licm_note_expr(Expr *e, unsigned char *written) {
    if (e == NULL)
        return;
    if (e->kind == E_ASSIGN || e->kind == E_POSTINC) {
        /* An assignment to a plain local writes exactly that local, so the
         * rest of the body stays analysable. Anything else -- an array
         * element, a field, a dereference -- can write through a pointer, and
         * then nothing can be assumed. `x++` is the same shape: its target is
         * in `lhs`, and missing it here would let a loop counter look
         * loop-invariant. */
        if (e->lhs != NULL && e->lhs->kind == E_VAR) {
            int s = e->lhs->slot;
            if (s > 0 && s < LICM_MAX_SLOT)
                written[s] = 1;
            licm_note_expr(e->rhs, written);
            for (int i = 0; i < e->nargs; i++)
                licm_note_expr(e->args[i], written);
            return;
        }
        memset(written, 1, LICM_MAX_SLOT);
        return;
    }
    if (licm_may_write(e)) {
        /* Something with an effect runs here, so assume every local it could
         * reach is written. */
        memset(written, 1, LICM_MAX_SLOT);
        return;
    }
    licm_note_expr(e->lhs, written);
    licm_note_expr(e->rhs, written);
    licm_note_expr(e->env, written);
    for (int i = 0; i < e->nargs; i++)
        licm_note_expr(e->args[i], written);
    for (int i = 0; i < e->narms; i++)
        licm_note_expr(e->arms[i].body, written);
}

static void licm_note_stmt(Stmt *s, unsigned char *written) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            licm_note_stmt(s->items[i], written);
        return;
    case S_VAR:
        licm_note_expr(s->init, written);
        if (s->slot > 0 && s->slot < LICM_MAX_SLOT)
            written[s->slot] = 1;
        return;
    case S_EXPR:
        licm_note_expr(s->expr, written);
        return;
    case S_RETURN:
        licm_note_expr(s->expr, written);
        return;
    case S_IF:
        licm_note_expr(s->cond, written);
        licm_note_stmt(s->body, written);
        licm_note_stmt(s->orelse, written);
        return;
    case S_WHILE:
        licm_note_expr(s->cond, written);
        licm_note_stmt(s->body, written);
        return;
    case S_FOR:
        licm_note_stmt(s->for_init, written);
        licm_note_expr(s->cond, written);
        licm_note_stmt(s->body, written);
        licm_note_expr(s->for_step, written);
        return;
    case S_BREAK:
    case S_CONTINUE:
        /* The body may not run to completion, but every hoisted expression is
         * trap-free, so evaluating it early is still harmless. */
        return;
    default:
        memset(written, 1, LICM_MAX_SLOT);
        return;
    }
}

/* True when every leaf is a literal or a read of a local the body never
 * writes, and the node itself is pure arithmetic. */
static int licm_invariant(Expr *e, const unsigned char *written) {
    if (e == NULL)
        return 0;
    switch (e->kind) {
    case E_INT:
    case E_BOOL:
        return 1;
    case E_VAR:
        /* Never a global. A global is writable by any call anywhere in the loop,
         * including one in another file, so a read of one is not provably
         * invariant however obviously loop-local it looks -- and `e->slot` is -1
         * for a global, which the next line would happily call invariant. */
        if (e->gidx >= 0)
            return 0;
        /* A slot past the end of the table is one the pass cannot track, so it
         * is not *provably* invariant and is treated as not invariant. Reading
         * it as invariant silently hoisted every expression over it out of its
         * loop, in any function with more than LICM_MAX_SLOT locals. */
        return e->slot <= 0 || (e->slot < LICM_MAX_SLOT && !written[e->slot]);
    case E_UNARY:
        return (e->op == T_MINUS || e->op == T_TILDE) && licm_invariant(e->lhs, written);
    case E_BINARY:
        return licm_op_is_safe(e->op) && licm_invariant(e->lhs, written) &&
               licm_invariant(e->rhs, written);
    default:
        return 0;
    }
}

typedef struct {
    Expr **items;
    int n;
    int cap;
    CG *cg;
} LicmList;

static void licm_push(LicmList *L, Expr *e) {
    if (L->n == L->cap) {
        int ncap = L->cap == 0 ? 8 : L->cap * 2;
        Expr **ni = arena_alloc_array(L->cg->arena, (size_t)ncap, sizeof(Expr *));
        if (L->n > 0)
            memcpy(ni, L->items, (size_t)L->n * sizeof(Expr *));
        L->items = ni;
        L->cap = ncap;
    }
    L->items[L->n++] = e;
}

/* Returns nonzero when a child was hoisted, which makes this node redundant:
 * its operands are already in slots. */
static int licm_walk_expr(CG *cg, Expr *e, const unsigned char *written, LicmList *L) {
    if (e == NULL)
        return 0;
    int below = licm_walk_expr(cg, e->lhs, written, L);
    below += licm_walk_expr(cg, e->rhs, written, L);
    below += licm_walk_expr(cg, e->env, written, L);
    for (int i = 0; i < e->nargs; i++)
        below += licm_walk_expr(cg, e->args[i], written, L);
    for (int i = 0; i < e->narms; i++)
        below += licm_walk_expr(cg, e->arms[i].body, written, L);
    if (e->hoisted_slot != 0) {
        /* The measure pass already decided this and reserved the slot. The
         * emit pass only has to schedule the computation. */
        if (L != NULL)
            licm_push(L, e);
        return 1;
    }
    if (below > 0)
        return 1;
    if (e->kind != E_BINARY && e->kind != E_UNARY)
        return 0;
    if (!is_kind(e->type, TK_INT) && !is_kind(e->type, TK_BOOL))
        return 0;
    if (!licm_invariant(e, written))
        return 0;
    /* Reserve the slot in the measure pass too, so the frame is sized for it. */
    cg->cur_locals_bytes += 8;
    e->hoisted_slot = 8 + cg->cur_locals_bytes;
    if (L != NULL)
        licm_push(L, e);
    return 1;
}

static void licm_walk_stmt(CG *cg, Stmt *s, const unsigned char *written, LicmList *L) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            licm_walk_stmt(cg, s->items[i], written, L);
        return;
    case S_VAR:
        licm_walk_expr(cg, s->init, written, L);
        return;
    case S_EXPR:
        licm_walk_expr(cg, s->expr, written, L);
        return;
    case S_RETURN:
        licm_walk_expr(cg, s->expr, written, L);
        return;
    case S_IF:
        licm_walk_expr(cg, s->cond, written, L);
        licm_walk_stmt(cg, s->body, written, L);
        licm_walk_stmt(cg, s->orelse, written, L);
        return;
    case S_WHILE:
        licm_walk_expr(cg, s->cond, written, L);
        licm_walk_stmt(cg, s->body, written, L);
        return;
    case S_FOR:
        licm_walk_expr(cg, s->cond, written, L);
        licm_walk_stmt(cg, s->for_init, written, L);
        licm_walk_stmt(cg, s->body, written, L);
        licm_walk_expr(cg, s->for_step, written, L);
        return;
    default:
        return;
    }
}

/* Runs the pass over one loop.
 *
 * `step` is the loop's own step expression, or NULL for a `while`: it runs once
 * per iteration, so a local it writes is not invariant, and without it the
 * loop's induction variable looks like a constant and expressions over it get
 * hoisted out of the loop they vary in.
 *
 * `cond` is the loop's condition, for the same reason and by the same argument.
 * A condition is usually a read, but nothing stops it writing --
 * `while ((k = k + 1) < 4)` is ordinary -- and the body walk cannot see it. Handed
 * only the body, the pass concluded that `k` was invariant and hoisted `k * 2` out
 * of the loop, computing it once with `k` still zero.
 *
 * `emit` is 0 during the measure pass, which still marks nodes and reserves
 * their slots so the frame accounts for them. */
static void licm_hoist(CG *cg, Stmt *body, Expr *step, Expr *cond, int emit) {
    if (body == NULL)
        return;
    unsigned char written[LICM_MAX_SLOT];
    memset(written, 0, sizeof written);
    /* The condition and the step first: they are the writes the body walk cannot
     * see, and getting either wrong is a miscompile rather than a missed
     * optimization. */
    if (cond != NULL)
        licm_note_expr(cond, written);
    if (step != NULL)
        licm_note_expr(step, written);
    licm_note_stmt(body, written);
    LicmList L;
    memset(&L, 0, sizeof L);
    L.cg = cg;
    licm_walk_stmt(cg, body, written, emit ? &L : NULL);
    if (!emit)
        return;
    /* L.items is in post-order, so an operand is always computed before the
     * expression that reads its slot. */
    cg->emitting_hoist = 1;
    for (int i = 0; i < L.n; i++) {
        gen_expr(cg, L.items[i]);
        buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", L.items[i]->hoisted_slot);
    }
    cg->emitting_hoist = 0;
}

/* Generates one statement and releases every temporary string it made.
 *
 * The list is saved and restored rather than cleared, so an enclosing statement's
 * own temporaries survive: `f(g())` releases `g()`'s string when the call
 * statement ends, not when the function does. Loops go through here once per
 * iteration, which is what makes `for (...) { Console.WriteLog(a + b); }` release per
 * iteration rather than accumulating. */
/* Releases every string temporary recorded since `saved`, and unpins the slots so
 * they go back to the pool. The frame keeps its high-water mark, which is what
 * temp_high is for. */
static void release_str_temps(CG *cg, int saved) {
    for (int i = saved; i < cg->npinned; i++) {
        load_temp(cg, cg->pinned[i], "rdi");
        buf_printf(cg->out, "  call z_str_free\n");
    }
    cg->npinned = saved;
}

/* True when control cannot fall out of the bottom of `s`, because its last
 * statement transfers control somewhere else: a return, a break, a continue, or
 * the end of an inlined body. A loop does not count, since it falls through once
 * its condition fails, and an `if` does not count unless both arms leave, which
 * is not worth tracking -- being wrong here would only cost the jump it removes.
 *
 * This is what lets the `if` below drop the jump that would follow such a branch.
 * That jump is not merely useless, it sits in the middle of the path: a `return`
 * inside the branch emits its own jump, and an unconditional jump after an
 * unconditional jump is one the front end still has to fetch, decode and predict
 * on every pass. In a recursive function with a base case, that is the path half
 * the calls take. */
static int stmt_leaves(CG *cg, Stmt *s) {
    if (s == NULL)
        return 0;
    switch (s->kind) {
    case S_RETURN:
    case S_BREAK:
    case S_CONTINUE:
    case S_LEAVE:
        return 1;
    case S_BLOCK:
        return s->nitems > 0 && stmt_leaves(cg, s->items[s->nitems - 1]);
    default:
        return 0;
    }
}

static void gen_stmt(CG *cg, Stmt *s) {
    int saved = cg->npinned;
    gen_stmt_body(cg, s);
    release_str_temps(cg, saved);
}

static void gen_stmt_body(CG *cg, Stmt *s) {
    /* Temporaries (including struct-return buffers) never live across a
     * statement boundary, so start each statement from a clean temp space. */
    cg->temp_top = 0;
    /* Mark the line before the statement's code, so the line table attributes
     * the instructions to the line the user wrote them on. */
    dbg_loc(cg, s->span);
    switch (s->kind) {
    case S_LEAVE:
        /* End of an inlined body. The label is whatever block encloses it, found
         * through the inlining stack, so a `return` from several levels of
         * nesting inside the copy still lands at the right place. */
        if (cg->inl_depth > 0) {
            int lbl = cg->inl_label[cg->inl_depth - 1];
            if (lbl > 0)
                buf_printf(cg->out, "  jmp .Li%d\n", lbl);
        }
        break;
    case S_VAR:
        if (s->init != NULL) {
            LocalInfo *li = s->slot > 0 ? li_lookup(cg, s->slot) : NULL;
            if (li != NULL && li->is_const && !s->boxed && s->init->kind == E_INT) {
                /* Propagated constant: no storage needed, reads are inlined.
                 * A captured local is excepted. Its slot holds a pointer to a
                 * heap cell, and the cell is built here by reading that slot --
                 * so skipping the store left the box holding whatever the frame
                 * happened to contain, and a `var k = 7;` captured by a lambda
                 * read back as garbage. Constant propagation inlines reads of
                 * the value, never reads through the box, so the two are
                 * independent and both have to happen. */
                break;
            }
            const char *reg = is_aggregate(s->type) ? NULL : local_reg(cg, s->slot);
            if (s->boxed) {
                /* A captured local is boxed at its declaration: evaluate the
                 * initializer, put it in a fresh heap cell, and leave the cell's
                 * address in the slot. The closure's environment is built from
                 * these same pointers, so both sides name one value. */
                if (is_kind(s->type, TK_F64)) {
                    gen_float(cg, s->init);
                    buf_printf(cg->out, "  movq rdi, %s\n  call z_box_f\n", XMM_ACC);
                } else if (is_aggregate(s->type)) {
                    /* A struct gets a cell of its own size, written in place. */
                    int t = temp_alloc(cg);
                    gen_expr(cg, s->init); /* rax = the source address */
                    store_temp(cg, t);
                    buf_printf(cg->out, "  mov rdi, %d\n  call z_box_n\n", type_size(s->type));
                    load_temp(cg, t, "rsi");
                    buf_printf(cg->out, "  mov rdi, rax\n");
                    emit_memcpy(cg, type_size(s->type));
                    cg->temp_top = t;
                } else {
                    gen_expr(cg, s->init);
                    buf_printf(cg->out, "  mov rdi, rax\n  call z_box\n");
                }
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", s->slot);
                break;
            }
            if (is_kind(s->type, TK_F64)) {
                /* A float stays in the frame (see LocalInfo.is_float), so the
                 * store is a movsd of the value the initializer left in xmm0. */
                gen_float(cg, s->init);
                buf_printf(cg->out, "  movsd QWORD PTR [rbp - %d], %s\n", s->slot, XMM_ACC);
                break;
            }
            gen_expr(cg, s->init);
            if (is_aggregate(s->type)) {
                /* rax = source address; copy into the local's frame slot. */
                buf_printf(cg->out, "  mov rsi, rax\n");
                buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", s->slot);
                emit_memcpy(cg, type_size(s->type));
            } else if (reg != NULL) {
                buf_printf(cg->out, "  mov %s, rax\n", reg);
            } else {
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", s->slot);
            }
        }
        break;
    case S_EXPR:
        if (s->expr != NULL)
            gen_void_expr(cg, s->expr);
        break;
    case S_RETURN: {
        /* The destructors for every scope this `return` leaves.
         *
         * They run *after* the returned value has been computed, never before
         * it. A returned local is one of the values those destructors destroy,
         * and the copy the parser wraps a borrowed return in is what reads it --
         * so drops-then-value freed the bytes the copy was about to read, and the
         * caller received a `str_dup` of freed memory. It showed up as a `return`
         * of a local built by a `+` reporting a length in the hundreds of
         * thousands, because `str_dup` reads the length out of the header of
         * memory the collector had already been handed. `?` orders its
         * `try_drops` this way already, for the same reason.
         *
         * The value is spilled across them because a destructor is a call and a
         * call clobbers rax, which is where the value is. The slot is pinned so
         * the destructors, generated as their own statements, are not handed it. */
        int tret = -1;
        int saved_pins = cg->npinned;
        if (s->expr != NULL) {
            gen_expr(cg, s->expr);
            /* A struct return is already in the caller's buffer by then, so only
             * a value still in rax needs somewhere to survive the calls. */
            if (s->ret_drops != NULL && !cg->cur_ret_struct) {
                tret = temp_alloc(cg);
                store_temp(cg, tret);
                pin_temp(cg, tret);
            }
        }
        if (cg->cur_ret_struct && s->expr != NULL && is_aggregate(s->expr->type)) {
            /* Copy the returned struct into the caller's hidden buffer. */
            buf_printf(cg->out, "  mov rsi, rax\n");
            buf_printf(cg->out, "  mov rdi, QWORD PTR [rbp - %d]\n", cg->cur_ret_slot);
            emit_memcpy(cg, type_size(s->expr->type));
        }
        if (s->ret_drops != NULL) {
            gen_stmt(cg, s->ret_drops);
            /* Unpin rather than leave it: this is the value being handed back, so
             * the statement's own release pass must not free it. */
            cg->npinned = saved_pins;
            if (tret >= 0)
                load_temp(cg, tret, "rax");
        }
        buf_printf(cg->out, "  jmp .Lret_%s\n", cg->cur_sym);
        break;
    }
    case S_IF: {
        int lelse = next_label(cg);
        int lend = next_label(cg);
        gen_cond_branch(cg, s->cond, lelse);
        gen_stmt(cg, s->body);
        /* Skipped when the branch leaves, since nothing can arrive at the end
         * label from there. The else label is emitted either way: it is where the
         * condition lands, and with no else-arm it is also where the statement
         * after the `if` begins. */
        if (!stmt_leaves(cg, s->body))
            buf_printf(cg->out, "  jmp .L%d\n", lend);
        buf_printf(cg->out, ".L%d:\n", lelse);
        if (s->orelse != NULL)
            gen_stmt(cg, s->orelse);
        buf_printf(cg->out, ".L%d:\n", lend);
        break;
    }
    case S_WHILE: {
        if (cg->opt >= 2)
            licm_hoist(cg, s->body, NULL, s->cond, !cg->measuring);
        gen_loop(cg, s->body, s->cond, NULL, unroll_factor(cg, s->body, s->cond));
        break;
    }
    case S_FOR: {
        if (cg->opt >= 2)
            licm_hoist(cg, s->body, s->for_step, s->cond, !cg->measuring);
        if (s->for_init != NULL)
            gen_stmt(cg, s->for_init);
        gen_loop(cg, s->body, s->cond, s->for_step, unroll_factor(cg, s->body, s->cond));
        break;
    }
    case S_BREAK:
        /* The parser rejects break/continue outside a loop, so the top of the
         * stack always belongs to the innermost enclosing loop. */
        if (cg->loop_depth > 0)
            buf_printf(cg->out, "  jmp .L%d\n", cg->loops[cg->loop_depth - 1].brk);
        break;
    case S_CONTINUE:
        if (cg->loop_depth > 0)
            buf_printf(cg->out, "  jmp .L%d\n", cg->loops[cg->loop_depth - 1].cont);
        break;
    case S_BLOCK: {
        /* The label goes on the stack for the length of the body, so a `return`
         * inside it can find the end of *this* body and not the end of whichever
         * one encloses it. The stack was there and nothing pushed onto it, so
         * `inl_depth` stayed zero and every `return` in an inlined body fell
         * through into the rest of that body and carried on. A `return` as the
         * last statement was unaffected, which is why no test could see it.
         *
         * A body that is not inlined has no label and pushes nothing, so a
         * `return` that reaches one from inside an inlined body jumps to the
         * innermost labelled body, which is the copy it belongs to. */
        int pushed = 0;
        if (s->inl_label > 0 && cg->inl_depth < (int)(sizeof cg->inl_label / sizeof(int))) {
            cg->inl_label[cg->inl_depth++] = s->inl_label;
            pushed = 1;
        }
        gen_block_items(cg, s);
        if (pushed)
            cg->inl_depth--;
        /* A block that wraps an inlined body carries the label its `return`s
         * jump to, emitted after the last item so reaching the end of the body
         * and returning from it are the same thing.
         *
         * `Li`, not `L`. These numbers come from the parser's counter and every
         * other label in the file comes from the code generator's, and the two
         * counters are seeded independently and both start low. Sharing one `.L`
         * namespace meant a loop in a spliced body and the end of another spliced
         * body could land on the same number, and the assembler stopped on the
         * duplicate symbol: a generic function with an inlinable call in it did
         * not build at all. */
        if (s->inl_label > 0)
            buf_printf(cg->out, ".Li%d:\n", s->inl_label);
        break;
    }
    case S_FUNC:
    case S_STRUCT:
    case S_UNION:
        break;
    }
}

static int align16(int n) { return (n + 15) & ~15; }
