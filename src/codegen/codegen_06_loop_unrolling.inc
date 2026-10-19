/* ---- loop unrolling (level 3) ----
 *
 * At -O3 a loop body is emitted Z_UNROLL times. Four is the usual sweet spot: it
 * removes three-quarters of the loop-back branches, and a larger factor buys
 * little before the body stops fitting in the instruction cache. */

#define Z_UNROLL 4
/* A body bigger than this is left rolled. The point of unrolling is to get the
 * body to sit in the instruction cache across consecutive iterations, and a body
 * that does not fit gains nothing from being copied four times. */
#define Z_UNROLL_MAX_STMT 24

/* Statements in a body, counted only until `cap` is passed: the caller is asking
 * "is this small enough to unroll", and a long body should not cost a full walk
 * to answer no. */
static int stmt_count(Stmt *s, int cap) {
    if (s == NULL)
        return 0;
    int n = 1;
    if (n > cap)
        return n;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems && n <= cap; i++)
            n += stmt_count(s->items[i], cap - n);
        break;
    case S_IF:
    case S_WHILE:
        n += stmt_count(s->body, cap - n);
        n += stmt_count(s->orelse, cap - n);
        break;
    case S_FOR:
        n += stmt_count(s->for_init, cap - n);
        n += stmt_count(s->body, cap - n);
        break;
    default:
        break;
    }
    return n;
}

/* ---- loop unrolling profitability ----
 *
 * Unrolling is not free here, and the reason is specific to how this compiler
 * emits a loop. A rolled iteration costs the body plus three instructions of
 * overhead: the condition's compare, its branch, and the jump back to the top.
 * An unrolled copy costs the same three. So the dynamic instruction count per
 * iteration is *identical* either way -- B+3 rolled against B+3 unrolled -- and
 * the only thing four copies buy is three fewer loop-back jumps per four
 * iterations, paid for with three extra condition tests and four times the body
 * in the instruction cache.
 *
 * That leaves exactly one reason to unroll, and it is a dependence reason: the
 * copies have to have work that can overlap. A variable the body both reads and
 * writes is a recurrence, and its value in iteration n+1 is computed from its
 * value in iteration n, so every copy that touches it queues up behind the same
 * multiply. One recurrence and the copies serialise against each other exactly
 * as one copy did. Two or more and copy 1's chain runs alongside copy 2's, which
 * is the only thing that makes the extra code worth its footprint.
 *
 * math and mix are the one-recurrence case (`s = (s * 31 + i) % d`): unrolling
 * them grew the emitted code by 35% and 11% and moved no timings, because there
 * was never any overlap to expose.
 */

#define DEP_MAX_SLOT 256

/* A slot past the end of the table is one this pass cannot track, and it is
 * dropped rather than assumed either way. That is the safe direction here and
 * the opposite of the hazard loop-invariant code motion has to be careful about:
 * there, treating an untracked slot as "not written" silently hoisted every
 * expression over it out of its loop. Here, dropping it can only lose a chain,
 * and losing a chain means the body rolls -- the same answer the pass gives when
 * it has nothing to say. Claiming a chain that is not there is the failure that
 * would matter, and that cannot happen. */

/* Recurrence counting for one loop body.
 *
 * `opaque` is set when the body contains something whose writes cannot be
 * narrowed to a plain local -- a call, an array store, a field store. Every
 * local then has to be assumed written, which makes the read/write sets
 * meaningless as a dependence answer, so the body is left rolled rather than
 * guessed about. */
typedef struct {
    unsigned char read[DEP_MAX_SLOT];    /* the body reads the slot */
    unsigned char written[DEP_MAX_SLOT]; /* the body writes the slot */
    unsigned char tested[DEP_MAX_SLOT];  /* the loop condition reads it */
    int opaque;
} DepInfo;

static void dep_mark(unsigned char *set, int slot) {
    if (slot > 0 && slot < DEP_MAX_SLOT)
        set[slot] = 1;
}

static void dep_note_expr(DepInfo *d, Expr *e) {
    if (e == NULL)
        return;
    switch (e->kind) {
    case E_ASSIGN:
    case E_POSTINC:
        /* The target of an assignment is a write, not a read. Counting it as
         * both would make every store look like a recurrence, and a body of
         * plain stores would pass the independence test for the wrong reason. */
        if (e->lhs != NULL && e->lhs->kind == E_VAR) {
            dep_mark(d->written, e->lhs->slot);
        } else {
            d->opaque = 1;
        }
        dep_note_expr(d, e->rhs);
        for (int i = 0; i < e->nargs; i++)
            dep_note_expr(d, e->args[i]);
        return;
    case E_CALL:
    case E_VCALL:
    case E_ICALL:
    case E_NEW:
    case E_NEWCLASS:
    case E_FNPTR:
    case E_MPTR:
    case E_CLOSURE:
        /* A call can write through anything it was handed, so the sets say
         * nothing reliable about this body afterwards. */
        d->opaque = 1;
        return;
    default:
        break;
    }
    if (e->kind == E_VAR)
        dep_mark(d->read, e->slot);
    dep_note_expr(d, e->lhs);
    dep_note_expr(d, e->rhs);
    dep_note_expr(d, e->env);
    for (int i = 0; i < e->nargs; i++)
        dep_note_expr(d, e->args[i]);
}

static void dep_note_stmt(DepInfo *d, Stmt *s) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            dep_note_stmt(d, s->items[i]);
        return;
    case S_VAR:
        /* A declaration is not a recurrence: it is a first definition, so the
         * slot it names starts the loop rather than carrying a value into it. */
        dep_note_expr(d, s->init);
        dep_mark(d->written, s->slot);
        return;
    case S_EXPR:
        dep_note_expr(d, s->expr);
        return;
    case S_RETURN:
        dep_note_expr(d, s->expr);
        return;
    case S_IF:
        dep_note_expr(d, s->cond);
        dep_note_stmt(d, s->body);
        dep_note_stmt(d, s->orelse);
        return;
    case S_WHILE:
    case S_FOR:
        /* A nested loop is a separate iteration space, and its variables are
         * reset by its own `for_init` rather than carried in from the outer
         * iteration. Counting its induction variable as an outer chain claims
         * parallelism that is not there: `for i { for j { s = s + 1 } }` has
         * exactly one outer recurrence, `s`, and reading `j` out of the inner
         * condition would make it look like two. So a nested loop makes the
         * dependence answer unavailable and the body is left rolled. */
        d->opaque = 1;
        return;
    case S_BREAK:
    case S_CONTINUE:
    case S_LEAVE:
        return;
    default:
        d->opaque = 1;
        return;
    }
}

/* Marks every local the loop condition reads. The condition is emitted once per
 * copy, so these are the variables whose value each copy waits on. */
static void dep_note_cond(Expr *e, unsigned char *tested) {
    if (e == NULL)
        return;
    if (e->kind == E_VAR) {
        dep_mark(tested, e->slot);
        return;
    }
    dep_note_cond(e->lhs, tested);
    dep_note_cond(e->rhs, tested);
    for (int i = 0; i < e->nargs; i++)
        dep_note_cond(e->args[i], tested);
}

/* How many loop-carried recurrences in this body are independent of each other
 * and of the loop's own induction variable. */
static int unroll_chain_count(Stmt *body, Expr *cond) {
    DepInfo d;
    memset(&d, 0, sizeof d);
    dep_note_cond(cond, d.tested);
    dep_note_stmt(&d, body);
    if (d.opaque)
        return 0;
    int chains = 0;
    for (int s = 1; s < DEP_MAX_SLOT; s++) {
        if (!d.read[s] || !d.written[s] || d.tested[s])
            continue;
        chains++;
    }
    return chains;
}

/* How many copies of this body to emit. Unrolling needs a condition to test
 * before each copy: that test is what makes an unknown trip count safe, since no
 * copy of the body runs unless the condition held, and the tail is handled by
 * the same test rather than by a computed iteration count. A `for(;;)` has no
 * condition and so is never unrolled. */
static int unroll_factor(CG *cg, Stmt *body, Expr *cond) {
    if (cg->opt < 3 || cond == NULL)
        return 1;
    if (stmt_count(body, Z_UNROLL_MAX_STMT + 1) > Z_UNROLL_MAX_STMT)
        return 1;
    /* At least two independent chains, so there is something for the copies to
     * overlap. The variable the condition tests is left out of the count by
     * `unroll_chain_count`: the test runs before every copy, so that variable's
     * update sits between one copy and the next and no copy can start until it
     * resolves. */
    if (unroll_chain_count(body, cond) < 2)
        return 1;
    return Z_UNROLL;
}

/* Emits a loop, `unroll` copies of the body deep.
 *
 * The condition is tested before every copy instead of once per pass. The body
 * therefore still runs exactly as many times as it did rolled -- what changes is
 * that the last copy branches back to the top while the others fall through into
 * the next copy, so most of the loop-back branches are gone and consecutive
 * iterations sit next to each other for the prefetcher. Each copy pushes its own
 * continuation label, so `continue` runs the step of the copy it appears in and
 * then branches on, and every copy shares `lend`, so one `break` leaves the whole
 * loop.
 *
 * At a factor of 1 this emits exactly what the rolled form did. */
static void release_str_temps(CG *cg, int saved);

static void gen_loop(CG *cg, Stmt *body, Expr *cond, Expr *step, int unroll) {
    int lstart = next_label(cg);
    int lend = next_label(cg);
    buf_printf(cg->out, ".L%d:\n", lstart);
    for (int u = 0; u < unroll; u++) {
        int last = u == unroll - 1;
        int lnext = last ? lstart : next_label(cg);
        int lcont = next_label(cg);
        /* The condition and the step are generated once but run every pass, so
         * their string temporaries have to be released here rather than at the end
         * of the enclosing statement. The enclosing statement's release is emitted
         * after the whole loop, which means every iteration but the last overwrote
         * the slot and never freed what was in it: a leak of one string per
         * iteration, measured at 29 allocations for a 29-iteration step.
         *
         * The body releases its own through `gen_stmt`, so what is left pinned
         * across that call is exactly what the condition recorded. */
        if (cond != NULL) {
            int before = cg->npinned;
            gen_cond_branch(cg, cond, last ? lend : lnext);
            release_str_temps(cg, before);
        }
        loop_push(cg, lend, lcont);
        gen_stmt(cg, body);
        loop_pop(cg);
        buf_printf(cg->out, ".L%d:\n", lcont);
        if (step != NULL) {
            int before = cg->npinned;
            gen_void_expr(cg, step);
            release_str_temps(cg, before);
        }
        buf_printf(cg->out, "  jmp .L%d\n", lnext);
        if (!last)
            buf_printf(cg->out, ".L%d:\n", lnext);
    }
    buf_printf(cg->out, ".L%d:\n", lend);
}
