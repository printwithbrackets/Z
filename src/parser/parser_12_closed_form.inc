/* ---- closed-form induction variables ----
 *
 * A counted loop whose body only accumulates loop-invariant amounts has a value
 * computable without running the loop:
 *
 *     var sum = 0; var i = 0;
 *     while (i < 20000000) { sum = sum + 82; i = i + 1; }
 *
 * adds 82 twenty million times, which is `sum = 82 * 20000000` written once. The
 * loop disappears. The body of that loop is invariant, so this is the limit of
 * what loop-invariant code motion can do -- it hoists the pieces, and this
 * notices there is nothing left to run.
 *
 * It runs as a tree rewrite in the parser rather than as a code-generation
 * trick, so everything downstream -- the register allocator, constant folding,
 * the strength reducer -- sees the finished statement and optimizes it for free.
 * In the example the result is a product of two constants, which the folder then
 * evaluates outright.
 *
 * The pattern is deliberately narrow. Every condition below is there because
 * getting it wrong is a wrong answer, not a missed speedup:
 *
 *   - the step is `i = i + C` or `i -= C` with C a nonzero constant;
 *   - the condition compares that same variable against a bound with `<`, `<=`,
 *     `>` or `>=`, and the bound does not change in the body;
 *   - the body is a straight-line run of `x = x + E`, `x = x - E`, `x += E`,
 *     `x -= E` and plain `x = E`, where x is a scalar local that is not the
 *     induction variable;
 *   - no contribution reads the induction variable, the accumulators, or the
 *     trip count, so each iteration really does add the same amount;
 *   - nothing in the body calls, allocates, or branches, and there is no `break`
 *     or `continue` -- an early exit means the trip count is not this one.
 *
 * Overflow behaves as it did: the closed form computes the same total the loop
 * would have, so a loop that overflowed was already outside what Z defines. */

#define CLOSE_FORM_MAX_ACC 8
#define CLOSE_FORM_MAX_VARS 16

typedef struct {
    int slot;
    char *name;
    Type *type;
    Expr *amount; /* the signed contribution of one iteration */
    /* The statement's own left-hand side, reused as the assignment target rather
     * than looked up again: this pass runs after the function's scope has been
     * popped, so a lookup by name would find nothing. */
    Expr *target;
} CloseFormAcc;

/* 1 when `e` reads any slot in `variant`, which is what stops it being
 * loop-invariant. A local's address counts as a read: the body could write
 * through it. */
static int expr_reads_any(Expr *e, const int *variant, int nvariant) {
    if (e == NULL)
        return 0;
    if (e->kind == E_VAR) {
        for (int i = 0; i < nvariant; i++)
            if (variant[i] == e->slot)
                return 1;
        return 0;
    }
    if (expr_reads_any(e->lhs, variant, nvariant) || expr_reads_any(e->rhs, variant, nvariant))
        return 1;
    for (int i = 0; i < e->nargs; i++)
        if (expr_reads_any(e->args[i], variant, nvariant))
            return 1;
    if (expr_reads_any(e->env, variant, nvariant))
        return 1;
    return 0;
}

/* Flattens an additive chain into the sum of everything it adds to the
 * accumulator, discarding the accumulator itself.
 *
 *     sum = sum + a*b + c*d - (a + c)
 *
 * parses as a left-leaning tree of `+` and `-` rooted at `sum`, and the terms are
 * a*b, c*d and -(a+c). Only the root has to be the variable: an interior `+` is a
 * term in its own right, and `a + c` is just an expression that happens to add.
 * Keeping the terms signed and summing them is what lets the loop be solved --
 * the amount per iteration is a constant, and three separately-hoisted
 * subexpressions would not say so.
 *
 * Returns 0 if any term reads the accumulator, which would make the contribution
 * differ between iterations. `*out` accumulates the running sum and starts NULL. */
static int flatten_accum(Parser *p, Expr *e, int slot, int neg, Span span, Expr **out,
                         int *found_base) {
    if (e == NULL)
        return 0;
    if (e->kind == E_VAR) {
        /* The accumulator itself contributes nothing -- that is the base the
         * chain is rooted at. A *different* variable here is an ordinary term:
         * `sum + a` has `a` in exactly this position. A stray read of the
         * accumulator further along is caught below as a term that reads it,
         * which is what makes `x = x * E` and `x = x + x` both fail. */
        if (e->slot == slot) {
            *found_base = 1;
            return 1;
        }
    }
    if (e->kind == E_BINARY && (e->op == T_PLUS || e->op == T_MINUS)) {
        int rneg = neg;
        if (e->op == T_MINUS)
            rneg = !neg;
        if (!flatten_accum(p, e->lhs, slot, neg, span, out, found_base))
            return 0;
        return flatten_accum(p, e->rhs, slot, rneg, span, out, found_base);
    }
    /* A term. It must be an integer, or the algebra below does not hold: `s = s +
     * "x"` looks exactly like an accumulate, and multiplying a string by the trip
     * count is not a thing. And it must not read the accumulator, or the amount
     * would depend on how many times the loop has already run. */
    if (!is_kind(e->type, TK_INT) || expr_reads_any(e, &slot, 1))
        return 0;
    Expr *term = e;
    if (neg) {
        Expr *zero = new_expr(p, E_INT, span);
        zero->type = type_int(p->ty);
        term = make_binary(p, T_MINUS, zero, e, span);
    }
    *out = *out == NULL ? term : make_binary(p, T_PLUS, *out, term, span);
    return 1;
}

/* Matches one accumulate-into-a-local statement and reduces it to the signed
 * amount it adds per iteration.
 *
 *   `x += E` / `x = x + E`  -> +E
 *   `x -= E` / `x = x - E`  -> -E
 *
 * Returns 0 for anything else, including a compound form that is not additive
 * (`x = x * E` would need a power, `x = x / E` a logarithm) and a plain `x = E`,
 * whose result does not depend on the trip count at all. */
static int close_form_match(Parser *p, Stmt *st, CloseFormAcc *acc) {
    if (st == NULL || st->kind != S_EXPR || st->expr == NULL || st->expr->kind != E_ASSIGN)
        return 0;
    Expr *a = st->expr;
    if (a->lhs == NULL || a->lhs->kind != E_VAR || a->lhs->slot <= 0)
        return 0;
    if (is_kind(a->lhs->type, TK_STRUCT) || is_kind(a->lhs->type, TK_UNION) ||
        is_kind(a->lhs->type, TK_ARRAY) || is_kind(a->lhs->type, TK_PTR) ||
        is_kind(a->lhs->type, TK_F64))
        return 0;
    Expr *amount = NULL;
    if (a->compound) {
        if (a->op != T_PLUS && a->op != T_MINUS)
            return 0;
        if (expr_reads_any(a->rhs, &a->lhs->slot, 1))
            return 0;
        if (a->op == T_MINUS) {
            Expr *zero = new_expr(p, E_INT, st->span);
            zero->type = type_int(p->ty);
            amount = make_binary(p, T_MINUS, zero, a->rhs, st->span);
        } else {
            amount = a->rhs;
        }
    } else {
        /* `x = <chain rooted at x>` and nothing else. A self-update that is not
         * additive has the same shape, so the flatten below is what rejects it:
         * `x = x * E` reaches it as a term that reads x. */
        int found_base = 0;
        if (!flatten_accum(p, a->rhs, a->lhs->slot, 0, st->span, &amount, &found_base) ||
            !found_base)
            return 0; /* not rooted at the accumulator: a plain set, not an update */
        if (amount == NULL)
            return 0; /* `x = x`: nothing accumulates */
    }
    acc->slot = a->lhs->slot;
    acc->name = a->lhs->name;
    acc->type = a->lhs->type;
    acc->amount = amount;
    acc->target = a->lhs;
    return 1;
}

/* 1 when `e` contains a call anywhere, including inside a nested expression.
 *
 * The counterpart to `expr_reads_any`, which sees a call's arguments and nothing
 * below them. A pass that multiplies a contribution by a trip count needs to know
 * the contribution is the same every iteration, and a call is where that cannot
 * be established. */
static int expr_has_call(Expr *e) {
    if (e == NULL)
        return 0;
    if (e->kind == E_CALL)
        return 1;
    if (expr_has_call(e->lhs) || expr_has_call(e->rhs) || expr_has_call(e->env))
        return 1;
    for (int i = 0; i < e->nargs; i++)
        if (expr_has_call(e->args[i]))
            return 1;
    for (int i = 0; i < e->narms; i++)
        if (expr_has_call(e->arms[i].body))
            return 1;
    return 0;
}

/* The induction variable, its stride, and the bound, from a loop's step and
 * condition. Returns 1 on a match. */
static int close_form_trip(Parser *p, Stmt *loop, Expr *step_in, int *ivar, long long *stride,
                           Expr **bound, int *strict, int *downward) {
    /* A `for` carries its step in the header; a `while` carries it as the last
     * statement of its body, which the caller peels off and passes in. */
    Expr *step = step_in != NULL ? step_in : loop->for_step;
    if (step == NULL || step->kind != E_ASSIGN)
        return 0;
    if (step->lhs == NULL || step->lhs->kind != E_VAR)
        return 0;
    int slot = step->lhs->slot;
    if (slot <= 0 || step->rhs == NULL)
        return 0;
    /* Four spellings of the same step, all a self-update by a constant:
     *
     *     i = i + C    i = i - C    i += C    i -= C
     *
     * The compound forms carry the operator on the assignment node and the bare
     * amount on the right; the spelled-out forms carry it on a binary node whose
     * left has to be the same variable, since `i = j + 1` walks a different one
     * and its trip count is not a stride. `i = i` is a zero stride and is
     * rejected below, along with a non-constant amount. */
    long long c = 0;
    if (step->compound) {
        if (step->op != T_PLUS && step->op != T_MINUS)
            return 0;
        long long d;
        if (!const_fold_static(p, step->rhs, &d))
            return 0;
        c = step->op == T_MINUS ? -d : d;
    } else if (step->rhs->kind == E_VAR) {
        if (step->rhs->slot != slot || !const_fold_static(p, step->rhs, &c))
            return 0;
    } else if (step->rhs->kind == E_BINARY && step->rhs->lhs != NULL &&
               step->rhs->lhs->kind == E_VAR && step->rhs->lhs->slot == slot &&
               (step->rhs->op == T_PLUS || step->rhs->op == T_MINUS)) {
        long long d;
        if (!const_fold_static(p, step->rhs->rhs, &d))
            return 0;
        c = step->rhs->op == T_MINUS ? -d : d;
    } else {
        return 0;
    }
    if (c == 0)
        return 0; /* a zero stride never terminates */
    Expr *cond = loop->cond;
    if (cond == NULL || cond->kind != E_BINARY)
        return 0;
    Expr *cv = NULL;
    Expr *cb = NULL;
    switch (cond->op) {
    case T_LT:
    case T_LE:
        cv = cond->lhs;
        cb = cond->rhs;
        *strict = cond->op == T_LT;
        break;
    case T_GT:
    case T_GE:
        cv = cond->lhs;
        cb = cond->rhs;
        *strict = cond->op == T_GT;
        /* A downward comparison. The stride is *not* negated to suit it: the
         * point of the direction is that the stride has to agree with it. A
         * loop counting down towards a bound below it has a negative stride, and
         * negating here made every one of them look like a sign mismatch, so
         * none of them was ever closed.
         *
         * The disagreement is not a missed speedup. `i < 5` with `i = i - 1`
         * counts up towards a bound below it and cannot terminate, and the trip
         * count arithmetic below will happily return a finite number of
         * iterations for it, which is a program that returns from a loop that
         * does not. The check that refuses it is below. */
        *downward = 1;
        break;
    default:
        return 0;
    }
    if (cv == NULL || cb == NULL || cv->kind != E_VAR || cv->slot != slot)
        return 0;
    *ivar = slot;
    *stride = c;
    *bound = cb;
    return 1;
}

/* True when `st` writes `slot`, which for this pass means mentioning it on the
 * left of an assignment in any spelling.
 *
 * One function because the start-of-loop search has to agree with itself about
 * what a write is: the nearest write wins, and a write it did not recognise
 * would let the search step over it and take an earlier one instead, which is a
 * different loop's start.
 *
 * A statement this cannot see into is NOT reported as a write, because it may
 * contain one. The distinction matters: "does not write it" and "might write
 * it" are different answers, and the search below has to treat the second as
 * the reason to stop. */
static int stmt_writes_slot(Stmt *st, int slot) {
    if (st == NULL)
        return 0;
    if (st->kind == S_VAR)
        return st->slot == slot;
    if (st->kind != S_EXPR || st->expr == NULL)
        return 0;
    Expr *e = st->expr;
    return e->kind == E_ASSIGN && e->lhs != NULL && e->lhs->kind == E_VAR && e->lhs->slot == slot;
}

/* True when `st` might write `slot` even though it is not a plain assignment
 * to it.
 *
 * An `if`, a loop, or a block can contain an assignment to the induction
 * variable, and a statement this cannot see into is therefore a statement the
 * start-of-loop search must not step over. Stepping over one lands on an older
 * initialisation, and the closed form computed from that is a different loop's
 * answer: the program computes one thing at -O0 and another at -O2.
 *
 * A plain expression statement whose expression is not an assignment is the
 * same case: `f(i)` can write through a pointer the search cannot follow. */
static int stmt_may_write_slot(Stmt *st, int slot) {
    if (st == NULL)
        return 0;
    if (stmt_writes_slot(st, slot))
        return 0;
    if (st->kind == S_EXPR) {
        /* A plain assignment to something else cannot write the slot. Any
         * other expression might, through a call or a pointer. */
        Expr *e = st->expr;
        if (e != NULL && e->kind == E_ASSIGN)
            return 0;
        return 1;
    }
    if (st->kind == S_VAR)
        return 0;
    /* An `if`, a loop, a block, a `return`: it may contain a write. */
    return 1;
}

/* Applies close_form_loop to every loop in a function body, innermost first, so
 * that a closed form computed for an inner loop is visible to an enclosing one's
 * analysis. An inner loop that collapses leaves an ordinary block of assignments
 * behind, which the enclosing pass then sees as accumulate statements and can
 * close over in turn. */
static void close_form_stmts(Parser *p, Stmt **items, int n) {
    for (int i = 0; i < n; i++) {
        Stmt *s = items[i];
        if (s == NULL)
            continue;
        switch (s->kind) {
        case S_BLOCK:
            close_form_stmts(p, s->items, s->nitems);
            continue;
        case S_FOR:
        case S_WHILE:
            close_form_stmts(p, s->body != NULL && s->body->kind == S_BLOCK ? s->body->items : NULL,
                             s->body != NULL && s->body->kind == S_BLOCK ? s->body->nitems : 0);
            break;
        case S_IF:
            close_form_stmts(p, s->body != NULL && s->body->kind == S_BLOCK ? s->body->items : NULL,
                             s->body != NULL && s->body->kind == S_BLOCK ? s->body->nitems : 0);
            break;
        default:
            continue;
        }
        Stmt *replaced = close_form_loop(p, s, items, i);
        if (replaced != s)
            items[i] = replaced;
    }
}

/* Rewrites a counted accumulate loop into its closed form, or returns it
 * unchanged. */
static Stmt *close_form_loop(Parser *p, Stmt *loop, Stmt **siblings, int sib_index) {
    if (p->opt_level < 2)
        return loop;
    if (loop->kind != S_FOR && loop->kind != S_WHILE)
        return loop;
    /* A `while` keeps its step in the body, so peel it off and count only what is
     * left as the body's accumulators. The trip count does not include the step,
     * which runs one extra time -- as it does in the original loop. */
    Stmt *wbody = loop->body;
    int peel = 0;
    Expr *step_expr = NULL;
    if (loop->kind == S_WHILE) {
        if (wbody == NULL || wbody->kind != S_BLOCK || wbody->nitems < 1)
            return loop;
        Stmt *last = wbody->items[wbody->nitems - 1];
        if (last->kind != S_EXPR || last->expr == NULL || last->expr->kind != E_ASSIGN)
            return loop;
        step_expr = last->expr;
        peel = 1;
    }
    int ivar;
    long long stride;
    Expr *bound = NULL;
    int strict = 0;
    int downward = 0;
    if (!close_form_trip(p, loop, step_expr, &ivar, &stride, &bound, &strict, &downward))
        return loop;
    long long limit;
    if (!const_fold_static(p, bound, &limit))
        return loop;

    Stmt *body = loop->body;
    if (body == NULL)
        return loop;
    /* Only a straight-line block. A nested loop, a branch or a `break` means the
     * trip count is not the one computed here. */
    if (body->kind != S_BLOCK)
        return loop;
    int nbody = body->nitems - peel;

    /* Collect the accumulators, and the slots the body writes, so a later
     * contribution can be checked against them. */
    CloseFormAcc accs[CLOSE_FORM_MAX_ACC];
    int nacc = 0;
    int written[CLOSE_FORM_MAX_VARS];
    int nwritten = 0;

    for (int i = 0; i < nbody; i++) {
        Stmt *st = body->items[i];
        if (st->kind == S_BREAK || st->kind == S_CONTINUE || st->kind == S_RETURN)
            return loop;
        if (st->kind != S_EXPR) {
            /* A declaration or a nested control-flow statement: not this shape. */
            /* A declaration, a nested loop or any other control flow: not this
             * shape. (S_EXPR and the loop kinds are all that reach here.) */
            return loop;
        }
        CloseFormAcc one;
        if (!close_form_match(p, st, &one))
            return loop;
        if (one.slot == ivar)
            return loop; /* the loop variable is also an accumulator */
        if (nwritten < CLOSE_FORM_MAX_VARS)
            written[nwritten++] = one.slot;
        int found = -1;
        for (int k = 0; k < nacc; k++)
            if (accs[k].slot == one.slot)
                found = k;
        if (found >= 0) {
            /* Two updates to one accumulator in one iteration: add them, which is
             * exact for integers and is what the loop would have done. */
            accs[found].amount = make_binary(p, T_PLUS, accs[found].amount, one.amount, st->span);
        } else {
            if (nacc >= CLOSE_FORM_MAX_ACC)
                return loop;
            accs[nacc++] = one;
        }
    }
    if (nacc == 0)
        return loop;

    /* Every contribution must be the same on every iteration: it may not read the
     * induction variable, nor an accumulator, since the value of an accumulator
     * changes each iteration. */
    for (int k = 0; k < nacc; k++) {
        if (expr_reads_any(accs[k].amount, &ivar, 1))
            return loop;
        if (expr_reads_any(accs[k].amount, written, nwritten))
            return loop;
        /* And it may not be a call.
         *
         * `expr_reads_any` sees a call's *arguments*, so `n = n + scaled(i)` is
         * rejected, because `i` is right there in the argument list. It cannot
         * see inside the callee, and a function that answers differently each time
         * is the ordinary case for anything reading input, a clock, or a random
         * source. Multiplying one answer by the trip count is then not an
         * approximation, it is a different number: three lines of input gave 1 + 2
         * + 3 = 6 from the loop and 1 * 3 = 3 from the closed form.
         *
         * This is a refusal rather than a proof, which is the honest kind. There
         * is no way to know from here that a callee is pure. */
        if (expr_has_call(accs[k].amount))
            return loop;
    }

    /* Trip count. The start has to be a constant: it is the one number this
     * cannot otherwise recover. A `for` that declares its own loop variable
     * carries it in the initializer; a `while` leaves it wherever the preceding
     * statements put it, so scan backwards for the nearest write to it. */
    long long start = 0;
    int have_start = 0;
    if (loop->for_init != NULL && loop->for_init->kind == S_VAR && loop->for_init->init != NULL &&
        loop->for_init->init->kind == E_INT && loop->for_init->slot == ivar) {
        start = loop->for_init->init->ival;
        have_start = 1;
    }
    /* A `for` may also *assign* its loop variable rather than declare it, and that
     * is the same claim about where the count starts. It used to be missed, and
     * missing it was not a refusal: the search below fell through to the nearest
     * write it could see, which was a `var i = 0` somewhere above, and the closed
     * form then counted from there. `for (i = 5; i < 9; ...)` became nine
     * iterations instead of four. */
    if (!have_start && stmt_writes_slot(loop->for_init, ivar) && loop->for_init->kind == S_EXPR) {
        Expr *a = loop->for_init->expr;
        if (a != NULL && !a->compound && a->rhs != NULL && a->rhs->kind == E_INT) {
            start = a->rhs->ival;
            have_start = 1;
        }
    }
    if (!have_start && siblings != NULL) {
        /* Only the *nearest* write counts. An earlier one whose value is not a
         * constant would make any guess wrong, so the search stops there. The
         * induction variable is known by now, which matters: a scan that did not
         * know it latched onto whichever local it saw first. */
        for (int k = sib_index - 1; k >= 0 && !have_start; k--) {
            Stmt *prev = siblings[k];
            if (!stmt_writes_slot(prev, ivar)) {
                /* A statement that might write the induction variable stops the
                 * search. Stepping over it would land on an older write, and
                 * the trip count computed from that is a different loop's
                 * answer -- the program would compute one thing at -O0 and
                 * another at -O2. Refusing the rewrite is the honest answer:
                 * the start value is not known, so no trip count is known. */
                if (stmt_may_write_slot(prev, ivar))
                    break;
                continue;
            }
            /* The nearest write wins, and the search stops either way. A write
             * whose value is not a constant leaves the start unknown, which
             * refuses the rewrite rather than guessing. */
            if (prev->kind == S_VAR) {
                if (prev->init != NULL && prev->init->kind == E_INT) {
                    start = prev->init->ival;
                    have_start = 1;
                }
            } else {
                Expr *a = prev->expr;
                if (a != NULL && !a->compound && a->rhs != NULL && a->rhs->kind == E_INT) {
                    start = a->rhs->ival;
                    have_start = 1;
                }
            }
            break;
        }
    }
    if (!have_start)
        return loop;

    long long span = limit - start;
    long long n;
    if (!downward) {
        /* Upwards: the stride has to be positive and the bound above the start.
         * A non-positive stride here is a loop that counts away from its bound,
         * which never terminates, and returning a trip count for it would be a
         * finite answer for an infinite loop. */
        if (stride <= 0)
            return loop;
        if (span <= 0)
            return loop; /* the loop never runs: the trip count is 0, and the
                          * code below assumes at least one iteration */
        n = strict ? (span + stride - 1) / stride : span / stride + 1;
    } else {
        /* Downwards: the mirror image. A positive stride counts up past a bound
         * it is being compared against with `>`, and never terminates either. */
        if (stride >= 0)
            return loop;
        if (span >= 0)
            return loop;
        long long d = -stride;
        long long sp = -span;
        n = strict ? (sp + d - 1) / d : sp / d + 1;
    }
    if (n < 1)
        return loop;

    /* Build the replacement: a run of statements assigning each accumulator its
     * closed form, in place of the loop. */
    int cap = nacc + 1;
    Stmt **items = arena_alloc_array(p->arena, (size_t)cap, sizeof(Stmt *));
    int nitems = 0;
    Expr *count = new_expr(p, E_INT, loop->span);
    count->type = type_int(p->ty);
    count->ival = n;

    for (int k = 0; k < nacc; k++) {
        Expr *scaled = make_binary(p, T_STAR, accs[k].amount, count, loop->span);
        Expr *cur = accs[k].target;
        Expr *sum = make_binary(p, T_PLUS, cur, scaled, loop->span);
        if (sum->type == NULL)
            sum->type = accs[k].type;
        Expr *asg = new_expr(p, E_ASSIGN, loop->span);
        asg->lhs = cur;
        asg->rhs = sum;
        asg->type = accs[k].type;
        Stmt *st = new_stmt(p, S_EXPR, loop->span);
        st->expr = asg;
        items[nitems++] = st;
    }
    /* The induction variable ends where the loop left it.
     *
     * The replacement used to assign the accumulators and nothing else, so a read
     * of the loop variable after the loop saw the value from *before* it. That is
     * the one thing about this rewrite that is observable, and it is observable
     * exactly where a loop is normally invisible: a counter that is printed after
     * it, or handed to something else. The loop's own step ran last, so this does
     * too.
     *
     * The step's own left-hand side is reused as the target rather than looked up,
     * for the reason the accumulators' is: this pass runs after the scope is
     * gone, so a name would not resolve. */
    {
        Expr *step = step_expr != NULL ? step_expr : loop->for_step;
        if (step != NULL && step->lhs != NULL && step->lhs->kind == E_VAR) {
            Expr *st0 = new_expr(p, E_INT, loop->span);
            st0->type = type_int(p->ty);
            st0->ival = start;
            Expr *st1 = new_expr(p, E_INT, loop->span);
            st1->type = type_int(p->ty);
            st1->ival = stride;
            Expr *adv = make_binary(p, T_STAR, count, st1, loop->span);
            Expr *fin = make_binary(p, T_PLUS, st0, adv, loop->span);
            if (fin->type == NULL)
                fin->type = type_int(p->ty);
            Expr *asg = new_expr(p, E_ASSIGN, loop->span);
            asg->lhs = step->lhs;
            asg->rhs = fin;
            asg->type = type_int(p->ty);
            Stmt *st = new_stmt(p, S_EXPR, loop->span);
            st->expr = asg;
            items[nitems++] = st;
        }
    }
    if (nitems == 0)
        return loop;
    Stmt *blk = new_stmt(p, S_BLOCK, loop->span);
    blk->items = items;
    blk->nitems = nitems;
    return blk;
}
