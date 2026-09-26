#include "codegen.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dynamic output buffer for the assembly text. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void die_oom(void) {
    fprintf(stderr, "z: out of memory building assembly\n");
    exit(1);
}

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t ncap = b->cap == 0 ? 4096 : b->cap;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (nd == NULL)
        die_oom();
    b->data = nd;
    b->cap = ncap;
}

static void buf_puts(Buf *b, const char *s) {
    size_t n = strlen(s);
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        die_oom();
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

/* System V integer-argument registers, in order. */
static const char *const ARG_REGS[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

/* Callee-saved registers used to hold local variables whose address is never
 * taken, so their values stay in registers instead of reloading from the
 * stack on every use. rax is the accumulator and r10/r11 are expression
 * scratch, so the pool excludes them. */
static const char *const POOL_REGS[] = {"rbx", "r12", "r13", "r14", "r15"};
#define NPOOL 5

/* Per-scalar-local register-allocation info, keyed by frame slot. */
typedef struct {
    int slot;
    int eligible;   /* may live in a register */
    int d, u;       /* first-def / last-use statement index (d > u => unused) */
    int assigned;   /* index into POOL_REGS, or -1 */
    int const_cand; /* defined by a constant (E_INT) initializer */
    long long const_val;
    int reassigned; /* written by an assignment somewhere in the function */
    int is_const;   /* const_cand && !reassigned => value is a compile-time constant */
} LocalInfo;

/* Innermost-loop-first stack of break/continue targets. The parser rejects
 * break/continue outside a loop, so the top entry is always the right one. */
#define MAX_LOOP_DEPTH 64
typedef struct {
    int brk;
    int cont;
} LoopCtx;

typedef struct {
    Buf *out;
    StringTable *strings;
    int label_counter;
    LoopCtx loops[MAX_LOOP_DEPTH];
    int loop_depth;
    int bounds_checks; /* emit a runtime range check on every array index */
    Arena *arena;      /* for interned assembly symbol names */
    int temp_top;         /* current temporary high-water during emit */
    int temp_high;        /* max temp slots used in this function */
    int cur_locals_bytes; /* total frame bytes used by locals in current fn */
    int cur_ret_slot;     /* frame offset of the hidden $ret buffer (struct returns) */
    int cur_ret_struct;   /* 1 if the current function returns a struct by sret */
    const char *cur_sym;
    LocalInfo *locals; /* per-function register-allocation candidates */
    int nlocals, loc_cap;
    int pool_mask;    /* bitmask of POOL_REGS indices actually used */
    uint64_t *rodata; /* 64-bit constants (magic multipliers etc.) */
    int nrodata, ro_cap;
} CG;

/* Interns a 64-bit constant in .rodata, returning its index (labels are
 * .Lro<idx>). Deduplicated so the measure and emit passes share it. */
static int rodata_add(CG *cg, uint64_t v) {
    for (int i = 0; i < cg->nrodata; i++)
        if (cg->rodata[i] == v)
            return i;
    if (cg->nrodata == cg->ro_cap) {
        cg->ro_cap = cg->ro_cap ? cg->ro_cap * 2 : 8;
        cg->rodata = realloc(cg->rodata, (size_t)cg->ro_cap * sizeof(uint64_t));
        if (cg->rodata == NULL)
            die_oom();
    }
    cg->rodata[cg->nrodata] = v;
    return cg->nrodata++;
}

/* ---- Constant division / modulo strength reduction ---------------------- */
/* Granlund–Montgomery round-up magic. For a positive divisor d that is not a
 * power of two, finds the smallest shift s with the round-up multiplier
 * M = floor(2^(64+s)/d)+1 whose approximation is exact for every 64-bit
 * dividend (the "exactness condition" e = M*d - 2^(64+s) < 2^s). When M needs
 * 65 bits the quotient is recovered as (mulhi(n, M) + n) >> s. Returns 0 on
 * success. */
static int magic_for_divisor(uint64_t d, uint64_t *Mout, int *sout, int *addout) {
    for (int s = 0; s < 64; s++) {
        __uint128_t num = ((__uint128_t)1) << (64 + s);
        __uint128_t M = num / d + 1;
        __uint128_t e = M * d - num; /* 0 <= e < d */
        if (e < ((__uint128_t)1) << s) {
            *Mout = (uint64_t)M;
            *sout = s;
            *addout = (M >> 64) ? 1 : 0;
            return 0;
        }
    }
    return 1;
}

/* Reference model of the emitted sequence, used to self-check the magic at
 * compile time so a bad magic can never silently corrupt a division. */
static int64_t magic_ref_div(int64_t n, uint64_t M, int s, int add) {
    uint64_t mask = (uint64_t)(n >> 63);       /* arithmetic shift: all-ones if n<0 */
    uint64_t an = ((uint64_t)n ^ mask) - mask; /* |n| */
    __uint128_t P = (__uint128_t)an * M;
    uint64_t H = (uint64_t)(P >> 64);
    if (add)
        H += (uint64_t)n;
    uint64_t q = H >> s;
    return n < 0 ? -(int64_t)q : (int64_t)q;
}

/* Emits a fast multiply-shift sequence for `n / d` or `n % d` (op is T_SLASH or
 * T_PERCENT) with n already in rax, leaving the result in rax. Returns 0 if no
 * valid magic exists (caller falls back to idiv). */
static int emit_magic_divmod(CG *cg, TokenKind op, uint64_t d) {
    if (d < 2 || (d & (d - 1)) == 0)
        return 1; /* power of two / trivial: caller handles or uses idiv */
    uint64_t M;
    int s, add;
    if (magic_for_divisor(d, &M, &s, &add) != 0)
        return 1;

    /* Self-check against x86 idiv's truncating semantics on a spread of values. */
    {
        int64_t probes[] = {0,
                            1,
                            -1,
                            2,
                            -2,
                            (int64_t)d,
                            (int64_t)d - 1,
                            (int64_t)d + 1,
                            (int64_t)(d * 3),
                            INT64_MAX,
                            INT64_MIN,
                            INT64_MAX - 1,
                            INT64_MIN + 1,
                            123456789,
                            -123456789};
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            int64_t n = probes[i];
            int64_t want = n / (int64_t)d; /* C division truncates toward zero */
            if (magic_ref_div(n, M, s, add) != want)
                return 1; /* self-check failed: fall back to idiv */
        }
    }

    int mi = rodata_add(cg, M);
    int di = rodata_add(cg, d);
    /* r10 = n (preserved); rax = |n|; multiply-high by M; re-apply the sign. */
    buf_printf(cg->out, "  mov r10, rax\n");
    buf_printf(cg->out, "  mov rax, r10\n  sar rax, 63\n  mov r11, rax\n");
    buf_printf(cg->out, "  xor rax, r10\n  sub rax, r11\n");
    buf_printf(cg->out, "  mul QWORD PTR [rip + .Lro%d]\n", mi);
    if (add)
        buf_printf(cg->out, "  add rdx, r10\n");
    buf_printf(cg->out, "  shr rdx, %d\n", s);
    buf_printf(cg->out, "  mov rax, rdx\n  neg rax\n  test r10, r10\n  cmovns rax, rdx\n");
    if (op == T_PERCENT) {
        /* remainder = n - q*d */
        buf_printf(cg->out, "  imul rax, QWORD PTR [rip + .Lro%d]\n", di);
        buf_printf(cg->out, "  sub r10, rax\n  mov rax, r10\n");
    }
    return 0;
}

static void gen_expr(CG *cg, Expr *e);
static void gen_addr(CG *cg, Expr *e);
static void gen_stmt(CG *cg, Stmt *s);

static int is_kind(Type *t, TypeKind k) { return t != NULL && t->kind == k; }
/* A union is also a multi-word aggregate value represented by its address. */
static int is_aggregate(Type *t) { return is_kind(t, TK_STRUCT) || is_kind(t, TK_UNION); }

/* Reserves n consecutive temp slots (for a struct value) and returns the base. */
static int temp_alloc_many(CG *cg, int n) {
    int base = cg->temp_top;
    cg->temp_top += n;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    return base;
}

/* Copies `size` bytes from rsi (src) to rdi (dst). Clobbers r10. Uses
 * explicit moves rather than rep-prefixed string ops. */
static void emit_memcpy(CG *cg, int size) {
    int off = 0;
    while (size - off >= 8) {
        buf_printf(cg->out, "  mov r10, QWORD PTR [rsi + %d]\n", off);
        buf_printf(cg->out, "  mov QWORD PTR [rdi + %d], r10\n", off);
        off += 8;
    }
    while (off < size) {
        buf_printf(cg->out, "  mov r10b, BYTE PTR [rsi + %d]\n", off);
        buf_printf(cg->out, "  mov BYTE PTR [rdi + %d], r10b\n", off);
        off += 1;
    }
}

/* Locals occupy [rbp-8-locals_bytes, rbp-9]; temporaries sit just below that
 * region, so temp 0 begins 8 bytes under the deepest local. */
static int temp_off(CG *cg, int t) { return 8 + cg->cur_locals_bytes + 8 * (t + 1); }

static int next_label(CG *cg) { return ++cg->label_counter; }

static void store_temp(CG *cg, int t) {
    buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", temp_off(cg, t));
}

static void load_temp(CG *cg, int t, const char *reg) {
    buf_printf(cg->out, "  mov %s, QWORD PTR [rbp - %d]\n", reg, temp_off(cg, t));
}

static int temp_alloc(CG *cg) {
    int t = cg->temp_top++;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    return t;
}

/* Loads the 8-byte value at the address in rax. */
static void load_indirect(CG *cg) { buf_printf(cg->out, "  mov rax, QWORD PTR [rax]\n"); }

/* ---- Local register allocation ------------------------------------------ */

/* Returns the LocalInfo for a frame slot, creating it on first sight. */
static LocalInfo *li_for(CG *cg, int slot) {
    for (int i = 0; i < cg->nlocals; i++)
        if (cg->locals[i].slot == slot)
            return &cg->locals[i];
    if (cg->nlocals == cg->loc_cap) {
        cg->loc_cap = cg->loc_cap ? cg->loc_cap * 2 : 16;
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
    li->const_val = 0;
    li->reassigned = 0;
    li->is_const = 0;
    return li;
}

/* Marks every local referenced in a subtree as ineligible for a register,
 * because the expression takes its address (gen_addr would need the real
 * memory location). */
static void mark_addr_taken(CG *cg, Expr *e);

static void walk_alloc_expr(CG *cg, Expr *e, int idx);

static void mark_addr_taken(CG *cg, Expr *e) {
    if (e == NULL)
        return;
    if (e->kind == E_VAR) {
        li_for(cg, e->slot)->eligible = 0;
        return;
    }
    walk_alloc_expr(cg, e, 0);
}

/* Records a use of a local at statement index idx. */
static void li_use(CG *cg, int slot, int idx) {
    LocalInfo *li = li_for(cg, slot);
    if (li->u < idx)
        li->u = idx;
}

/* Records a definition of a local at statement index idx. */
static void li_def(CG *cg, int slot, int idx) {
    LocalInfo *li = li_for(cg, slot);
    if (li->d < 0 || idx < li->d)
        li->d = idx;
    if (li->u < idx)
        li->u = idx;
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
        li_use(cg, e->slot, idx);
        break;
    case E_ADDR:
        /* `&x` materializes the value of x; for a scalar that is a load, but
         * conservatively treat it as address-taken. */
        mark_addr_taken(cg, e->lhs);
        break;
    case E_INDEX:
        mark_addr_taken(cg, e->lhs); /* base is addressed */
        walk_alloc_expr(cg, e->rhs, idx);
        break;
    case E_FIELD:
        mark_addr_taken(cg, e->lhs); /* base is addressed */
        break;
    case E_DEREF:
        walk_alloc_expr(cg, e->lhs, idx); /* pointer value */
        break;
    case E_ASSIGN:
        if (e->lhs != NULL && e->lhs->kind == E_VAR) {
            li_def(cg, e->lhs->slot, idx);
            li_for(cg, e->lhs->slot)->reassigned = 1;
        } else {
            mark_addr_taken(cg, e->lhs);
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
    case S_VAR:
        if (s->init != NULL)
            walk_alloc_expr(cg, s->init, cur);
        if (s->slot > 0) {
            li_def(cg, s->slot, cur);
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
    case S_WHILE:
        walk_alloc_expr(cg, s->cond, cur);
        walk_alloc_block(cg, s->body, idx);
        break;
    case S_FOR:
        /* Each phase needs its own statement index. A for-loop variable is
         * defined in the init, read in the condition, and live all the way
         * through the body to the step; giving the whole loop one index would
         * make its interval [i,i] and let a body local share the register. */
        walk_alloc_stmt(cg, s->for_init, idx);
        walk_alloc_expr(cg, s->cond, (*idx)++);
        walk_alloc_block(cg, s->body, idx);
        walk_alloc_expr(cg, s->for_step, (*idx)++);
        break;
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
     * are spilled in the prologue). */
    for (int i = 0; i < fn->nparams; i++)
        li_for(cg, fn->params[i]->slot)->eligible = 0;

    int idx = 0;
    walk_alloc_block(cg, fn->fbody, &idx);

    /* Greedy colouring in order of first definition. A local initialized to a
     * constant and never reassigned anywhere is propagated as an immediate
     * (no storage, no register). */
    for (int i = 0; i < cg->nlocals; i++) {
        LocalInfo *li = &cg->locals[i];
        li->assigned = -1;
        /* Const propagation is only safe when the local is never reassigned
         * AND its address is never taken (else it can change through a
         * pointer behind our back) and it is a plain scalar. */
        li->is_const = li->const_cand && !li->reassigned && li->eligible;
        if (li->is_const)
            continue;
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
    switch (e->kind) {
    case E_NULL:
        *out = 0;
        return 1;
    case E_INT:
    case E_BOOL:
        *out = e->ival;
        return 1;
    case E_VAR: {
        if (e->agg_param)
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
static const char *local_reg(CG *cg, int slot) {
    for (int i = 0; i < cg->nlocals; i++)
        if (cg->locals[i].slot == slot && cg->locals[i].assigned >= 0)
            return POOL_REGS[cg->locals[i].assigned];
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
static void gen_leaf_to_reg(CG *cg, Expr *e, const char *reg) {
    if (e->kind == E_INT || e->kind == E_BOOL) {
        buf_printf(cg->out, "  mov %s, %lld\n", reg, e->ival);
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

/* Computes the address of an lvalue expression into rax. */
static void gen_addr(CG *cg, Expr *e) {
    switch (e->kind) {
    case E_VAR:
        if (e->agg_param) {
            /* An aggregate param is passed by reference: the slot holds the
             * address of the caller's value. */
            buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->slot);
        } else {
            buf_printf(cg->out, "  lea rax, [rbp - %d]\n", e->slot);
        }
        break;
    case E_DEREF:
        /* The pointer operand *is* the address. */
        gen_expr(cg, e->lhs);
        break;
    case E_INDEX: {
        int esz = type_size(e->lhs->type->base);
        int t = temp_alloc(cg);
        gen_expr(cg, e->lhs); /* base pointer */
        store_temp(cg, t);
        gen_expr(cg, e->rhs); /* index */
        /* Arrays in Z are always heap allocations from z_newarray, so a
         * value of array type always has a valid length header at ptr[-8] and
         * the index can be range-checked. A TK_PTR index has no header, so
         * there is nothing to check against and it is left alone. */
        if (cg->bounds_checks && is_kind(e->lhs->type, TK_ARRAY)) {
            int ti = temp_alloc(cg);
            int lbad = next_label(cg);
            int lok = next_label(cg);
            store_temp(cg, ti);              /* rax = index */
            load_temp(cg, t, "r11");         /* r11 = base */
            buf_printf(cg->out, "  cmp rax, 0\n  jl .L%d\n", lbad);
            buf_printf(cg->out, "  mov rcx, QWORD PTR [r11 - 8]\n");
            buf_printf(cg->out, "  cmp rax, rcx\n  jge .L%d\n", lbad);
            load_temp(cg, ti, "rax");
            buf_printf(cg->out, "  jmp .L%d\n", lok);
            buf_printf(cg->out, ".L%d:\n", lbad);
            buf_printf(cg->out, "  mov rdi, rax\n  mov rsi, rcx\n");
            buf_printf(cg->out, "  call z_bounds_fail\n");
            buf_printf(cg->out, ".L%d:\n", lok);
            cg->temp_top = ti;
        }
        if (esz != 1)
            buf_printf(cg->out, "  imul rax, %d\n", esz);
        load_temp(cg, t, "r11");
        buf_printf(cg->out, "  add rax, r11\n");
        cg->temp_top = t;
        break;
    }
    case E_FIELD:
        /* Struct field address = base struct address + field offset. For a
         * struct-typed base, gen_expr yields its address; for a pointer, the
         * pointer value is the address. */
        gen_expr(cg, e->lhs);
        if (e->field_off != 0)
            buf_printf(cg->out, "  add rax, %d\n", e->field_off);
        break;
    default:
        gen_expr(cg, e);
        break;
    }
}

/* Emits an arithmetic or comparison combining lhs (in rax) with rhs (in r11). */
/* True for the six relational operators. Mirrors the parser's set; codegen
 * needs its own copy because the two are separate translation units. */
static const char *z_sym(CG *cg, const char *name);
static void gen_icall(CG *cg, Expr *e);
static void gen_mptr(CG *cg, Expr *e);

static int is_cmp_op(TokenKind k) {
    return k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE;
}

/* Reduces the three-way result in r11 to a bool, for a comparison whose
 * operands were not plain integers (string ordering). */
static void emit_cmp_zero(CG *cg, TokenKind op) {
    switch (op) {
    case T_EQ:
        buf_printf(cg->out, "  cmp r11, 0\n  sete al\n  movzx rax, al\n");
        break;
    case T_NE:
        buf_printf(cg->out, "  cmp r11, 0\n  setne al\n  movzx rax, al\n");
        break;
    case T_LT:
        buf_printf(cg->out, "  cmp r11, 0\n  setl al\n  movzx rax, al\n");
        break;
    case T_LE:
        buf_printf(cg->out, "  cmp r11, 0\n  setle al\n  movzx rax, al\n");
        break;
    case T_GT:
        buf_printf(cg->out, "  cmp r11, 0\n  setg al\n  movzx rax, al\n");
        break;
    case T_GE:
        buf_printf(cg->out, "  cmp r11, 0\n  setge al\n  movzx rax, al\n");
        break;
    default:
        buf_printf(cg->out, "  cmp r11, 0\n  sete al\n  movzx rax, al\n");
        break;
    }
}

static void emit_binop_op(CG *cg, TokenKind op) {
    switch (op) {
    case T_PLUS:
        buf_printf(cg->out, "  add rax, r11\n");
        break;
    case T_MINUS:
        buf_printf(cg->out, "  sub rax, r11\n");
        break;
    case T_STAR:
        buf_printf(cg->out, "  imul rax, r11\n");
        break;
    case T_SLASH:
        buf_printf(cg->out, "  cqo\n  idiv r11\n");
        break;
    case T_PERCENT:
        buf_printf(cg->out, "  cqo\n  idiv r11\n  mov rax, rdx\n");
        break;
    case T_AMP:
        buf_printf(cg->out, "  and rax, r11\n");
        break;
    case T_PIPE:
        buf_printf(cg->out, "  or rax, r11\n");
        break;
    case T_CARET:
        buf_printf(cg->out, "  xor rax, r11\n");
        break;
    case T_SHL:
        buf_printf(cg->out, "  mov rcx, r11\n  shl rax, cl\n");
        break;
    case T_SHR:
        buf_printf(cg->out, "  mov rcx, r11\n  sar rax, cl\n");
        break;
    case T_EQ:
        buf_printf(cg->out, "  cmp rax, r11\n  sete al\n  movzx rax, al\n");
        break;
    case T_NE:
        buf_printf(cg->out, "  cmp rax, r11\n  setne al\n  movzx rax, al\n");
        break;
    case T_LT:
        buf_printf(cg->out, "  cmp rax, r11\n  setl al\n  movzx rax, al\n");
        break;
    case T_LE:
        buf_printf(cg->out, "  cmp rax, r11\n  setle al\n  movzx rax, al\n");
        break;
    case T_GT:
        buf_printf(cg->out, "  cmp rax, r11\n  setg al\n  movzx rax, al\n");
        break;
    case T_GE:
        buf_printf(cg->out, "  cmp rax, r11\n  setge al\n  movzx rax, al\n");
        break;
    default:
        buf_printf(cg->out, "  add rax, r11\n");
        break;
    }
}

static void gen_int_literal(CG *cg, long long v) {
    if (v >= -2147483648LL && v <= 2147483647LL) {
        buf_printf(cg->out, "  mov rax, %lld\n", v);
    } else {
        buf_printf(cg->out, "  movabs rax, %lld\n", v);
    }
}

static void gen_call(CG *cg, Expr *e) {
    int sret = is_aggregate(e->type);
    int rt_nt = sret ? (type_size(e->type) + 7) / 8 : 0;
    if (rt_nt < 1)
        rt_nt = 1;
    /* For a struct-returning call, reserve a result buffer in the frame and
     * pass its address as the hidden first argument. */
    int rt_base = sret ? temp_alloc_many(cg, rt_nt) : 0;
    /* A struct temp occupies its reserved slots with field 0 at the lowest
     * slot (temp_off(base+nt-1)), matching the struct-literal layout. */
    int rt_addr = sret ? temp_off(cg, rt_base + rt_nt - 1) : 0;
    int base = cg->temp_top;
    cg->temp_top += e->nargs;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    for (int i = 0; i < e->nargs; i++) {
        gen_expr(cg, e->args[i]);
        store_temp(cg, base + i);
    }
    int regoff = sret ? 1 : 0;
    for (int i = 0; i < e->nargs && i + regoff < 6; i++) {
        load_temp(cg, base + i, ARG_REGS[i + regoff]);
    }
    if (sret) {
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
    }
    buf_printf(cg->out, "  call %s\n", e->is_extern ? e->name : z_sym(cg, e->name));
    if (sret) {
        /* The result value is the address of the buffer. Keep the buffer
         * reserved for the rest of the statement so a later nested call does
         * not clobber it; the arg temps above it are freed. */
        buf_printf(cg->out, "  lea rax, [rbp - %d]\n", rt_addr);
        cg->temp_top = rt_base + rt_nt;
    } else {
        cg->temp_top = base;
    }
}

/* A call through a function pointer. The target is evaluated into a frame temp
 * first, because evaluating the arguments would otherwise clobber rax before
 * the call. */
/* Builds a bound method pointer: a two-word GC cell holding { code, receiver }.
 * The value yielded is the cell's address, an ordinary eight-byte pointer, so
 * the receiver is kept alive by the collector for as long as the pointer is. */
static void gen_mptr(CG *cg, Expr *e) {
    int t = temp_alloc(cg);
    gen_expr(cg, e->lhs); /* the receiver: a class object pointer */
    store_temp(cg, t);
    load_temp(cg, t, "rsi");
    if (e->vtable_index >= 0) {
        /* Virtual: read the implementation out of the receiver's vtable so the
         * binding dispatches on the runtime type. */
        buf_printf(cg->out, "  mov r11, QWORD PTR [rsi]\n");
        buf_printf(cg->out, "  mov rdi, QWORD PTR [r11 + %d]\n", e->vtable_index * 8);
    } else {
        buf_printf(cg->out, "  lea rdi, [rip + %s]\n", z_sym(cg, e->name));
    }
    buf_printf(cg->out, "  call z_newbinding\n");
    cg->temp_top = t;
}

/* A call through a function or method pointer. The target is evaluated into a
 * frame temp first, because evaluating the arguments would otherwise clobber
 * rax before the call. */
static void gen_icall(CG *cg, Expr *e) {
    int sret = is_aggregate(e->type);
    int rt_nt = sret ? (type_size(e->type) + 7) / 8 : 0;
    if (rt_nt < 1)
        rt_nt = 1;
    int rt_base = sret ? temp_alloc_many(cg, rt_nt) : 0;
    int rt_addr = sret ? temp_off(cg, rt_base + rt_nt - 1) : 0;
    /* The parser rejects a bound pointer returning a struct, so a struct
     * result buffer and a bound receiver never both claim rdi here. */
    int bound = is_kind(e->lhs->type, TK_MPTR);
    int tf = temp_alloc(cg);
    gen_expr(cg, e->lhs); /* the callable */
    store_temp(cg, tf);
    int base = cg->temp_top;
    cg->temp_top += e->nargs;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    for (int i = 0; i < e->nargs; i++) {
        gen_expr(cg, e->args[i]);
        store_temp(cg, base + i);
    }
    /* A bound pointer's receiver occupies rdi, so the declared arguments start
     * one register higher. */
    int regoff = (sret || bound) ? 1 : 0;
    for (int i = 0; i < e->nargs && i + regoff < 6; i++)
        load_temp(cg, base + i, ARG_REGS[i + regoff]);
    if (sret)
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
    load_temp(cg, tf, "r11");
    if (bound) {
        /* r11 is the binding cell: { code, receiver }. */
        buf_printf(cg->out, "  mov rdi, QWORD PTR [r11 + 8]\n");
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n");
    }
    buf_printf(cg->out, "  call r11\n");
    if (sret) {
        buf_printf(cg->out, "  lea rax, [rbp - %d]\n", rt_addr);
        cg->temp_top = rt_base + rt_nt;
    } else {
        cg->temp_top = tf;
    }
}

/* Emits `rax = rax <op> imm` when the right operand is a literal. Mirrors
 * emit_binop_op but uses x86 immediate forms (no r11 round-trip). */
/* True when `v` fits the sign-extended imm32 that add/sub/imul/and/or/xor/cmp
 * accept with a 64-bit register operand. Anything wider has to go through a
 * register: `sub rax, 1152921504606846976` is not an encodable instruction, and
 * the assembler rejects it outright rather than picking a wider form. */
static int fits_imm32(long long v) {
    return v >= -2147483648LL && v <= 2147483647LL;
}

static void emit_binop_imm(CG *cg, TokenKind op, long long imm) {
    /* Out-of-range constants take the register path. mov r11, imm64 is always
     * encodable, so this stays correct at the cost of one instruction. */
    if (!fits_imm32(imm)) {
        buf_printf(cg->out, "  mov r11, %lld\n", imm);
        emit_binop_op(cg, op);
        return;
    }
    switch (op) {
    case T_PLUS:
        buf_printf(cg->out, "  add rax, %lld\n", imm);
        break;
    case T_MINUS:
        buf_printf(cg->out, "  sub rax, %lld\n", imm);
        break;
    case T_STAR:
        buf_printf(cg->out, "  imul rax, %lld\n", imm);
        break;
    case T_LT:
        buf_printf(cg->out, "  cmp rax, %lld\n  setl al\n  movzx rax, al\n", imm);
        break;
    case T_LE:
        buf_printf(cg->out, "  cmp rax, %lld\n  setle al\n  movzx rax, al\n", imm);
        break;
    case T_GT:
        buf_printf(cg->out, "  cmp rax, %lld\n  setg al\n  movzx rax, al\n", imm);
        break;
    case T_GE:
        buf_printf(cg->out, "  cmp rax, %lld\n  setge al\n  movzx rax, al\n", imm);
        break;
    case T_EQ:
        buf_printf(cg->out, "  cmp rax, %lld\n  sete al\n  movzx rax, al\n", imm);
        break;
    case T_NE:
        buf_printf(cg->out, "  cmp rax, %lld\n  setne al\n  movzx rax, al\n", imm);
        break;
    default:
        /* Division/remainder: use a multiply-shift magic when the divisor is a
         * positive constant, else fall back to idiv. */
        if ((op == T_SLASH || op == T_PERCENT) && imm > 1 &&
            emit_magic_divmod(cg, op, (uint64_t)imm) == 0) {
            /* magic sequence emitted */
        } else {
            buf_printf(cg->out, "  mov r11, %lld\n", imm);
            emit_binop_op(cg, op);
        }
        break;
    }
}

/* Emits a virtual method call: evaluate args (args[0] = receiver object), load
 * the target from the receiver's vtable at `e->vtable_index`, and call it. */
static void gen_vcall(CG *cg, Expr *e) {
    int sret = is_aggregate(e->type);
    int rt_nt = sret ? (type_size(e->type) + 7) / 8 : 0;
    if (rt_nt < 1)
        rt_nt = 1;
    int rt_base = sret ? temp_alloc_many(cg, rt_nt) : 0;
    int rt_addr = sret ? temp_off(cg, rt_base + rt_nt - 1) : 0;
    int base = cg->temp_top;
    cg->temp_top += e->nargs;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    for (int i = 0; i < e->nargs; i++) {
        gen_expr(cg, e->args[i]);
        store_temp(cg, base + i);
    }
    /* Resolve the target: obj = args[0]; vptr = obj[0]; fn = vtable[vtable_index]. */
    load_temp(cg, base + 0, "r11");                      /* r11 = receiver object */
    buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n"); /* vptr */
    buf_printf(cg->out, "  mov r11, QWORD PTR [r11 + %d]\n", e->vtable_index * 8);
    int regoff = sret ? 1 : 0;
    for (int i = 0; i < e->nargs && i + regoff < 6; i++)
        load_temp(cg, base + i, ARG_REGS[i + regoff]);
    if (sret)
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
    buf_printf(cg->out, "  call r11\n");
    if (sret) {
        buf_printf(cg->out, "  lea rax, [rbp - %d]\n", rt_addr);
        cg->temp_top = rt_base + rt_nt;
    } else {
        cg->temp_top = base;
    }
}

/* True if `e` is a left-leaning arithmetic accumulation chain rooted at a read
 * of `slot` (e.g. `sum + a + b - c` for slot=sum), where every right operand is
 * a constant or a simple leaf. Such a chain can be applied directly in the
 * register already holding `slot`'s value (three-address style). */
static int is_accum_chain(CG *cg, Expr *e, int slot) {
    while (e != NULL) {
        if (e->kind == E_VAR)
            return e->slot == slot && !e->agg_param;
        if (e->kind != E_BINARY)
            return 0;
        if (e->op != T_PLUS && e->op != T_MINUS && e->op != T_STAR)
            return 0;
        long long cr;
        if (!const_fold_val(cg, e->rhs, &cr) && !is_leaf_expr(e->rhs))
            return 0;
        e = e->lhs;
    }
    return 0;
}

/* Applies an accumulation chain (see is_accum_chain) in place into `reg`,
 * which already holds the base value. Recurses down the left spine, then
 * applies each operator to `reg` directly, avoiding the accumulator
 * round-trip through rax. Uses Intel operand order (`add reg, src`). */
static void gen_accum(CG *cg, Expr *e, const char *reg) {
    if (e == NULL || e->kind == E_VAR)
        return; /* base: reg already holds the value */
    gen_accum(cg, e->lhs, reg);
    const char *ins = e->op == T_PLUS ? "add" : e->op == T_MINUS ? "sub" : "imul";
    long long cr;
    if (const_fold_val(cg, e->rhs, &cr) && !fits_imm32(cr))
        buf_printf(cg->out, "  mov r11, %lld\n  %s %s, r11\n", cr, ins, reg);
    else if (const_fold_val(cg, e->rhs, &cr))
        buf_printf(cg->out, "  %s %s, %lld\n", ins, reg, cr);
    else {
        gen_leaf_to_reg(cg, e->rhs, "r11");
        buf_printf(cg->out, "  %s %s, r11\n", ins, reg);
    }
}

/* Lowers a built-in intrinsic. abs/min/max/clamp are a handful of
 * instructions and are emitted inline; sqrt calls the runtime, whose integer
 * root is exact where a Newton iteration in Z itself would not be. */
static void gen_intrinsic(CG *cg, Expr *e) {
    if (strcmp(e->name, "sin") == 0 || strcmp(e->name, "cos") == 0) {
        gen_expr(cg, e->args[0]);
        buf_printf(cg->out, "  mov rdi, rax\n  mov esi, %d\n  call z_trig\n",
                   e->name[0] == 'c' ? 1 : 0);
        return;
    }
    if (strcmp(e->name, "sqrt") == 0) {
        gen_expr(cg, e->args[0]);
        buf_printf(cg->out, "  mov rdi, rax\n  call z_isqrt\n");
        return;
    }
    if (strcmp(e->name, "abs") == 0) {
        gen_expr(cg, e->args[0]);
        /* rax holds -x, rcx the original; keep the negated value when the
         * original was negative, otherwise take the original back. */
        buf_printf(cg->out, "  mov rcx, rax\n  neg rax\n");
        buf_printf(cg->out, "  cmp rcx, 0\n  cmovg rax, rcx\n");
        return;
    }
    if (strcmp(e->name, "min") == 0 || strcmp(e->name, "max") == 0) {
        int want_lt = e->name[1] == 'i'; /* "min" selects the smaller */
        int t = temp_alloc(cg);
        gen_expr(cg, e->args[1]);
        store_temp(cg, t);
        gen_expr(cg, e->args[0]);
        load_temp(cg, t, "r11");
        buf_printf(cg->out, "  cmp rax, r11\n");
        buf_printf(cg->out, want_lt ? "  cmovg rax, r11\n" : "  cmovl rax, r11\n");
        cg->temp_top = t;
        return;
    }
    /* clamp(x, lo, hi) */
    int t = temp_alloc(cg);
    int t2 = temp_alloc(cg);
    gen_expr(cg, e->args[1]);
    store_temp(cg, t);
    gen_expr(cg, e->args[2]);
    store_temp(cg, t2);
    gen_expr(cg, e->args[0]);
    load_temp(cg, t, "r11");
    buf_printf(cg->out, "  cmp rax, r11\n  cmovl rax, r11\n");
    load_temp(cg, t2, "r11");
    buf_printf(cg->out, "  cmp rax, r11\n  cmovg rax, r11\n");
    cg->temp_top = t;
}

static void gen_expr(CG *cg, Expr *e) {
    switch (e->kind) {
    case E_INT:
        gen_int_literal(cg, e->ival);
        break;
    case E_BOOL:
        buf_printf(cg->out, "  mov rax, %d\n", e->ival ? 1 : 0);
        break;
    case E_NULL:
        buf_printf(cg->out, "  mov rax, 0\n");
        break;
    case E_STRING:
        buf_printf(cg->out, "  lea rax, [rip + .Lstr%d]\n", e->str_id);
        break;
    case E_VAR: {
        if (!e->agg_param && !is_aggregate(e->type)) {
            LocalInfo *li = li_lookup(cg, e->slot);
            if (li != NULL && li->is_const) {
                /* Propagated constant: read as an immediate, no memory. */
                buf_printf(cg->out, "  mov rax, %lld\n", li->const_val);
                break;
            }
        }
        const char *reg = (e->agg_param || is_aggregate(e->type)) ? NULL : local_reg(cg, e->slot);
        if (reg != NULL) {
            buf_printf(cg->out, "  mov rax, %s\n", reg);
        } else if (!e->agg_param && !is_aggregate(e->type)) {
            /* Fast path: a plain local read is a single instruction instead of
             * lea + load. */
            buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->slot);
        } else {
            gen_addr(cg, e);
            if (!is_aggregate(e->type))
                load_indirect(cg);
        }
        break;
    }
    case E_INDEX:
    case E_DEREF:
        gen_addr(cg, e);
        if (is_aggregate(e->type)) {
            /* A struct value is represented by its address. */
        } else {
            load_indirect(cg);
        }
        break;
    case E_ADDR:
        gen_addr(cg, e->lhs);
        break;
    case E_FIELD:
        if (is_kind(e->lhs->type, TK_ARRAY)) {
            /* array .length lives in the 8 bytes before the data */
            gen_expr(cg, e->lhs);
            buf_printf(cg->out, "  mov rax, QWORD PTR [rax - 8]\n");
        } else {
            gen_addr(cg, e);
            if (!is_aggregate(e->type))
                load_indirect(cg);
        }
        break;
    case E_STRUCTLIT: {
        /* Build the struct in a contiguous multi-slot temp, then yield its
         * address. The struct is laid out from the *lowest*-address slot
         * (top-1) upward so it never spills into neighbouring temp slots. */
        int sz = type_size(e->type);
        int nt = (sz + 7) / 8;
        if (nt < 1)
            nt = 1;
        temp_alloc_many(cg, nt);
        int top = cg->temp_top;
        int saddr = temp_off(cg, top - 1); /* rbp offset of field 0 */
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n  xor eax, eax\n", saddr);
        buf_printf(cg->out, "  mov rcx, %d\n  rep stosb\n", sz);
        int fi = 0;
        for (int i = 0; i < e->type->sdef->nfields && fi < e->nargs; i++) {
            Field *f = &e->type->sdef->fields[i];
            if (f->is_prop)
                continue;
            cg->temp_top = top;
            gen_expr(cg, e->args[fi++]);
            int faddr = saddr - f->offset;
            if (is_aggregate(f->type)) {
                buf_printf(cg->out, "  mov rsi, rax\n  lea rdi, [rbp - %d]\n", faddr);
                emit_memcpy(cg, type_size(f->type));
            } else {
                buf_printf(cg->out, "  lea r11, [rbp - %d]\n", faddr);
                buf_printf(cg->out, "  mov QWORD PTR [r11], rax\n");
            }
        }
        cg->temp_top = top;
        buf_printf(cg->out, "  lea rax, [rbp - %d]\n", saddr);
        break;
    }
    case E_NEW: {
        int esz = type_size(e->type->base);
        gen_expr(cg, e->lhs); /* count */
        buf_printf(cg->out, "  mov rdi, rax\n  mov rsi, %d\n  call z_newarray\n", esz);
        break;
    }
    case E_UNIONLIT: {
        /* Build a tagged value: zero the union temp, store the discriminant at
         * offset 0, then each payload field at its variant offset. */
        int sz = type_size(e->type);
        int nt = (sz + 7) / 8;
        if (nt < 1)
            nt = 1;
        temp_alloc_many(cg, nt);
        int top = cg->temp_top;
        int saddr = temp_off(cg, top - 1);
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n  xor eax, eax\n", saddr);
        buf_printf(cg->out, "  mov rcx, %d\n  rep stosb\n", sz);
        /* discriminant */
        buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], %d\n", saddr, e->variant->tag);
        for (int i = 0; i < e->nargs && i < e->variant->nfields; i++) {
            cg->temp_top = top;
            Field *f = &e->variant->fields[i];
            gen_expr(cg, e->args[i]);
            int faddr = saddr - f->offset;
            if (is_aggregate(f->type)) {
                buf_printf(cg->out, "  mov rsi, rax\n  lea rdi, [rbp - %d]\n", faddr);
                emit_memcpy(cg, type_size(f->type));
            } else {
                buf_printf(cg->out, "  lea r11, [rbp - %d]\n", faddr);
                buf_printf(cg->out, "  mov QWORD PTR [r11], rax\n");
            }
        }
        cg->temp_top = top;
        buf_printf(cg->out, "  lea rax, [rbp - %d]\n", saddr);
        break;
    }
    case E_MATCH: {
        /* subject address -> tag dispatch -> bind payload -> run body. Each
         * arm jumps PAST its body when the tag does not match. */
        int t = temp_alloc(cg);
        gen_expr(cg, e->lhs); /* union value = its address */
        store_temp(cg, t);    /* keep the address stable across arms */
        int lend = next_label(cg);
        for (int i = 0; i < e->narms; i++) {
            MatchArm *arm = &e->arms[i];
            int lnext = next_label(cg);
            cg->temp_top = t + 1;
            if (arm->variant != NULL) {
                load_temp(cg, t, "r11"); /* r11 = subject address */
                buf_printf(cg->out, "  mov rax, QWORD PTR [r11]\n");
                buf_printf(cg->out, "  cmp rax, %d\n", arm->variant->tag);
                buf_printf(cg->out, "  jne .L%d\n", lnext);
            }
            for (int b = 0; b < arm->nbind && arm->variant != NULL && b < arm->variant->nfields;
                 b++) {
                int off = arm->variant->fields[b].offset;
                cg->temp_top = t + 1;
                load_temp(cg, t, "r11");
                buf_printf(cg->out, "  mov rax, QWORD PTR [r11 + %d]\n", off);
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", arm->bind_slots[b]);
            }
            gen_expr(cg, arm->body);
            buf_printf(cg->out, "  jmp .L%d\n", lend);
            buf_printf(cg->out, ".L%d:\n", lnext);
        }
        buf_printf(cg->out, ".L%d:\n", lend);
        cg->temp_top = t;
        break;
    }
    case E_TERNARY: {
        /* Materialize the condition first (its side effects must run once),
         * then evaluate each branch inside its own control flow. */
        int t = temp_alloc(cg);
        int lelse = next_label(cg);
        int lend = next_label(cg);
        gen_expr(cg, e->lhs);
        store_temp(cg, t);
        cg->temp_top = t;
        load_temp(cg, t, "rax");
        buf_printf(cg->out, "  cmp rax, 0\n  je .L%d\n", lelse);
        gen_expr(cg, e->rhs);
        buf_printf(cg->out, "  jmp .L%d\n", lend);
        buf_printf(cg->out, ".L%d:\n", lelse);
        gen_expr(cg, e->args[0]);
        buf_printf(cg->out, ".L%d:\n", lend);
        break;
    }
    case E_UNARY:
        gen_expr(cg, e->lhs);
        if (e->op == T_MINUS) {
            buf_printf(cg->out, "  neg rax\n");
        } else if (e->op == T_TILDE) {
            buf_printf(cg->out, "  not rax\n");
        } else {
            buf_printf(cg->out, "  cmp rax, 0\n  sete al\n  movzx rax, al\n");
        }
        break;
    case E_INTRINSIC:
        gen_intrinsic(cg, e);
        break;
    case E_FNPTR:
        /* The address of a function is its label; lea rax, [rip + sym] is the
         * position-independent way to take it. */
        buf_printf(cg->out, "  lea rax, [rip + %s]\n",
                   e->is_extern ? e->name : z_sym(cg, e->name));
        break;
    case E_ICALL:
        gen_icall(cg, e);
        break;
    case E_MPTR:
        gen_mptr(cg, e);
        break;
    case E_BINARY: {
        long long cf;
        /* Whole-expression constant folding (covers propagated-constant
         * locals and invariant subtrees). */
        if (e->op != T_AND && e->op != T_OR && const_fold_val(cg, e, &cf)) {
            buf_printf(cg->out, "  mov rax, %lld\n", cf);
            break;
        }
        if (e->op == T_PLUS && is_kind(e->type, TK_STRING)) {
            /* Concatenation. Each operand is materialized as a string (int
             * operands go through z_itoa, bools become "true"/"false") then
             * z_concat(a, b). */
            int t0 = temp_alloc(cg);
            int t1 = temp_alloc(cg);
            gen_expr(cg, e->lhs);
            if (is_kind(e->lhs->type, TK_INT)) {
                buf_printf(cg->out, "  mov rdi, rax\n  call z_itoa\n");
            } else if (is_kind(e->lhs->type, TK_BOOL)) {
                buf_printf(cg->out,
                           "  lea rdi, [rip + .Lfalse_str]\n  lea r11, [rip + .Ltrue_str]\n");
                buf_printf(cg->out, "  cmp rax, 0\n  cmovne rdi, r11\n  mov rax, rdi\n");
            }
            store_temp(cg, t0);
            gen_expr(cg, e->rhs);
            if (is_kind(e->rhs->type, TK_INT)) {
                buf_printf(cg->out, "  mov rdi, rax\n  call z_itoa\n");
            } else if (is_kind(e->rhs->type, TK_BOOL)) {
                buf_printf(cg->out,
                           "  lea rdi, [rip + .Lfalse_str]\n  lea r11, [rip + .Ltrue_str]\n");
                buf_printf(cg->out, "  cmp rax, 0\n  cmovne rdi, r11\n  mov rax, rdi\n");
            }
            store_temp(cg, t1);
            load_temp(cg, t0, "rdi");
            load_temp(cg, t1, "rsi");
            buf_printf(cg->out, "  call z_concat\n");
            cg->temp_top = t0;
        } else if (is_kind(e->lhs->type, TK_STRING) && is_cmp_op(e->op)) {
            /* String ordering. z_strcmp gives a three-way result, which the
             * relational operator then reduces to a bool. */
            int t0 = temp_alloc(cg);
            int t1 = temp_alloc(cg);
            gen_expr(cg, e->lhs);
            store_temp(cg, t0);
            gen_expr(cg, e->rhs);
            store_temp(cg, t1);
            load_temp(cg, t0, "rdi");
            load_temp(cg, t1, "rsi");
            buf_printf(cg->out, "  call z_strcmp\n");
            buf_printf(cg->out, "  mov r11, rax\n");
            emit_cmp_zero(cg, e->op);
            cg->temp_top = t0;
        } else if (e->op == T_AND) {
            int lfalse = next_label(cg);
            int lend = next_label(cg);
            gen_expr(cg, e->lhs);
            buf_printf(cg->out, "  cmp rax, 0\n  je .L%d\n", lfalse);
            gen_expr(cg, e->rhs);
            buf_printf(cg->out, "  jmp .L%d\n.L%d:\n  mov rax, 0\n.L%d:\n", lend, lfalse, lend);
        } else if (e->op == T_OR) {
            int lend = next_label(cg);
            gen_expr(cg, e->lhs);
            buf_printf(cg->out, "  cmp rax, 0\n  jne .L%d\n", lend);
            gen_expr(cg, e->rhs);
            buf_printf(cg->out, ".L%d:\n", lend);
        } else {
            long long cr;
            if (const_fold_val(cg, e->rhs, &cr)) {
                /* The right operand folds to a constant: use an x86 immediate
                 * form (no r11 round-trip, no temp). */
                gen_expr(cg, e->lhs);
                emit_binop_imm(cg, e->op, cr);
            } else if (is_leaf_expr(e->rhs)) {
                /* The right operand is a leaf: evaluate the left into rax, then
                 * drop the right straight into r11 with no temp round-trip. */
                gen_expr(cg, e->lhs);
                gen_leaf_to_reg(cg, e->rhs, "r11");
                emit_binop_op(cg, e->op);
            } else {
                int t = temp_alloc(cg);
                gen_expr(cg, e->rhs);
                store_temp(cg, t);
                gen_expr(cg, e->lhs);
                load_temp(cg, t, "r11");
                cg->temp_top = t;
                emit_binop_op(cg, e->op);
            }
        }
        break;
    }
    case E_ASSIGN: {
        /* A scalar LHS that lives in a register is written in place; no
         * address is materialized. */
        const char *lreg = NULL;
        if (e->lhs != NULL && e->lhs->kind == E_VAR && !is_aggregate(e->type) && !e->lhs->agg_param)
            lreg = local_reg(cg, e->lhs->slot);

        if (lreg != NULL) {
            if (e->compound) {
                int tA = temp_alloc(cg);
                int tB = temp_alloc(cg);
                buf_printf(cg->out, "  mov rax, %s\n", lreg);
                store_temp(cg, tA);
                gen_expr(cg, e->rhs);
                store_temp(cg, tB);
                load_temp(cg, tA, "rax"); /* rax = current (left) */
                load_temp(cg, tB, "r11"); /* r11 = rhs (right) */
                emit_binop_op(cg, e->op);
                buf_printf(cg->out, "  mov %s, rax\n", lreg);
                cg->temp_top = tA;
            } else {
                /* In-place `x = x <op> ...` when x lives in a register: apply
                 * the chain directly to the register (three-address style). */
                if (e->rhs != NULL && is_accum_chain(cg, e->rhs, e->lhs->slot)) {
                    gen_accum(cg, e->rhs, lreg);
                    buf_printf(cg->out, "  mov rax, %s\n", lreg);
                } else {
                    gen_expr(cg, e->rhs);
                    buf_printf(cg->out, "  mov %s, rax\n", lreg);
                }
            }
            break;
        }
        int tA = temp_alloc(cg);
        gen_addr(cg, e->lhs);
        store_temp(cg, tA);
        if (e->compound) {
            int tB = temp_alloc(cg);
            gen_expr(cg, e->rhs);
            store_temp(cg, tB);
            load_temp(cg, tA, "r10");
            buf_printf(cg->out, "  mov rax, QWORD PTR [r10]\n");
            load_temp(cg, tB, "r11");
            emit_binop_op(cg, e->op);
            buf_printf(cg->out, "  mov QWORD PTR [r10], rax\n");
        } else {
            gen_expr(cg, e->rhs);
            if (is_aggregate(e->type)) {
                /* Aggregate assignment copies the whole value. rax holds the
                 * source address. */
                buf_printf(cg->out, "  mov rsi, rax\n");
                load_temp(cg, tA, "rdi");
                emit_memcpy(cg, type_size(e->type));
                load_temp(cg, tA, "rax"); /* assignment value = the struct */
            } else {
                load_temp(cg, tA, "r11");
                buf_printf(cg->out, "  mov QWORD PTR [r11], rax\n");
            }
        }
        cg->temp_top = tA;
        break;
    }
    case E_CALL:
        if (strcmp(e->name, "print") == 0) {
            gen_expr(cg, e->args[0]);
            Type *at = e->args[0]->type;
            if (is_kind(at, TK_STRING)) {
                buf_printf(cg->out, "  mov rdi, rax\n  call puts\n");
            } else if (is_kind(at, TK_BOOL)) {
                int lfalse = next_label(cg);
                int lend = next_label(cg);
                buf_printf(cg->out,
                           "  cmp rax, 0\n  je .L%d\n"
                           "  lea rdi, [rip + .Ltrue_str]\n  jmp .L%d\n"
                           ".L%d:\n  lea rdi, [rip + .Lfalse_str]\n"
                           ".L%d:\n  call puts\n",
                           lfalse, lend, lfalse, lend);
            } else {
                buf_printf(cg->out,
                           "  lea rdi, [rip + .Lfmt_int]\n  mov rsi, rax\n  call printf\n");
            }
        } else {
            gen_call(cg, e);
        }
        break;
    case E_VCALL:
        gen_vcall(cg, e);
        break;
    case E_NEWCLASS: {
        /* Allocate a zeroed object, install the vtable pointer, then run the
         * constructor (a method named like the class) if present. The object
         * pointer is kept in a frame temp because r11/rdi are clobbered by the
         * constructor call. */
        StructDef *sd = e->type->base->sdef;
        int size = type_size(e->type->base);
        int ot = temp_alloc(cg);
        buf_printf(cg->out, "  mov rdi, %d\n  call z_newobj\n", size);
        store_temp(cg, ot);
        load_temp(cg, ot, "r11");
        buf_printf(cg->out, "  lea rdx, [rip + .Lvt_%s]\n", sd->name);
        buf_printf(cg->out, "  mov QWORD PTR [r11], rdx\n");
        if (e->name != NULL) {
            /* Call ctor: this=obj in rdi, then the ctor args in rsi.. */
            int n = e->nargs;
            int base = cg->temp_top;
            cg->temp_top += n;
            if (cg->temp_top > cg->temp_high)
                cg->temp_high = cg->temp_top;
            for (int i = 0; i < n; i++) {
                gen_expr(cg, e->args[i]);
                store_temp(cg, base + i);
            }
            load_temp(cg, ot, "rdi");
            for (int i = 0; i < n && i + 1 < 6; i++)
                load_temp(cg, base + i, ARG_REGS[i + 1]);
            buf_printf(cg->out, "  call %s\n", e->is_extern ? e->name : z_sym(cg, e->name));
            cg->temp_top = base;
        }
        load_temp(cg, ot, "rax"); /* result = object pointer */
        cg->temp_top = ot;
        break;
    }
    default:
        gen_addr(cg, e);
        break;
    }
}

static void gen_block_items(CG *cg, Stmt *block) {
    if (block == NULL)
        return;
    for (int i = 0; i < block->nitems; i++)
        gen_stmt(cg, block->items[i]);
}

/* Conservative side-effect-free test: literals, scalar locals, and arithmetic /
 * comparison / ternary combinations of those. Calls, assignments, allocations,
 * derefs and matches are NOT pure. */
static int is_pure_expr(Expr *e) {
    if (e == NULL)
        return 0;
    switch (e->kind) {
    case E_NULL:
    case E_INT:
    case E_BOOL:
        return 1;
    case E_FNPTR:
        return 1;
    case E_MPTR:
        return is_pure_expr(e->lhs);
    case E_INTRINSIC:
    case E_ICALL: {
        for (int i = 0; i < e->nargs; i++)
            if (!is_pure_expr(e->args[i]))
                return 0;
        return 1;
    }
    case E_VAR:
        return !e->agg_param && !is_aggregate(e->type);
    case E_UNARY:
    case E_BINARY:
        return is_pure_expr(e->lhs) && is_pure_expr(e->rhs);
    case E_TERNARY:
        return is_pure_expr(e->lhs) && is_pure_expr(e->rhs) &&
               (e->nargs == 0 || is_pure_expr(e->args[0]));
    default:
        return 0;
    }
}

/* True if `op` is one of the six integer comparison operators. */
static int is_compare_op(TokenKind op) {
    return op == T_LT || op == T_LE || op == T_GT || op == T_GE || op == T_EQ || op == T_NE;
}

/* Emits the test of `cond`, jumping to `false_label` when it is false. When the
 * condition is a side-effect-free comparison, branches directly on the flags
 * instead of materializing a 0/1 boolean. */
static void gen_cond_branch(CG *cg, Expr *cond, int false_label) {
    if (cond != NULL && cond->kind == E_BINARY && is_compare_op(cond->op) &&
        is_pure_expr(cond->lhs) && is_pure_expr(cond->rhs)) {
        long long cr;
        int lhs_direct = 0;
        const char *lreg = NULL;
        if (cond->lhs->kind == E_VAR && !cond->lhs->agg_param && !is_aggregate(cond->lhs->type))
            lreg = local_reg(cg, cond->lhs->slot);
        if (lreg != NULL)
            lhs_direct = 1; /* compare the register directly, no rax copy */
        if (lhs_direct == 1 && const_fold_val(cg, cond->rhs, &cr) && fits_imm32(cr)) {
            buf_printf(cg->out, "  cmp %s, %lld\n", lreg, cr);
        } else {
            if (const_fold_val(cg, cond->rhs, &cr) && fits_imm32(cr)) {
                gen_expr(cg, cond->lhs);
                buf_printf(cg->out, "  cmp rax, %lld\n", cr);
            } else if (const_fold_val(cg, cond->rhs, &cr)) {
                /* Too wide for a cmp immediate: compare against a register. */
                gen_expr(cg, cond->lhs);
                buf_printf(cg->out, "  mov r11, %lld\n", cr);
                buf_printf(cg->out, "  cmp rax, r11\n");
            } else if (is_leaf_expr(cond->rhs)) {
                gen_expr(cg, cond->lhs);
                gen_leaf_to_reg(cg, cond->rhs, "r11");
                buf_printf(cg->out, "  cmp rax, r11\n");
            } else {
                int t = temp_alloc(cg);
                gen_expr(cg, cond->rhs);
                store_temp(cg, t);
                gen_expr(cg, cond->lhs);
                load_temp(cg, t, "r11");
                cg->temp_top = t;
                buf_printf(cg->out, "  cmp rax, r11\n");
            }
        }
        const char *inv = "e";
        switch (cond->op) {
        case T_LT:
            inv = "ge";
            break;
        case T_LE:
            inv = "g";
            break;
        case T_GT:
            inv = "le";
            break;
        case T_GE:
            inv = "l";
            break;
        case T_EQ:
            inv = "ne";
            break;
        case T_NE:
            inv = "e";
            break;
        default:
            break;
        }
        buf_printf(cg->out, "  j%s .L%d\n", inv, false_label);
        return;
    }
    gen_expr(cg, cond);
    buf_printf(cg->out, "  cmp rax, 0\n  je .L%d\n", false_label);
}

/* Generates an expression whose result value is discarded (an expression
 * statement or a `for` step). For a register-resident `x <op>= v` or
 * `x = x <op> v` accumulation this updates the register in place without
 * materializing the result in rax. */
static void gen_void_expr(CG *cg, Expr *e) {
    if (e != NULL && e->kind == E_ASSIGN && e->lhs != NULL && e->lhs->kind == E_VAR &&
        !is_aggregate(e->type) && !e->lhs->agg_param) {
        const char *lreg = local_reg(cg, e->lhs->slot);
        if (lreg != NULL) {
            if (e->compound) {
                long long cr;
                if (const_fold_val(cg, e->rhs, &cr)) {
                    const char *ins = e->op == T_PLUS    ? "add"
                                      : e->op == T_MINUS ? "sub"
                                      : e->op == T_STAR  ? "imul"
                                                         : NULL;
                    /* A constant wider than imm32 has to travel in a register. */
                    if (ins != NULL && fits_imm32(cr)) {
                        buf_printf(cg->out, "  %s %s, %lld\n", ins, lreg, cr);
                        return;
                    }
                    if (ins != NULL) {
                        buf_printf(cg->out, "  mov r11, %lld\n  %s %s, r11\n", cr, ins, lreg);
                        return;
                    }
                } else if (is_leaf_expr(e->rhs)) {
                    const char *ins = e->op == T_PLUS    ? "add"
                                      : e->op == T_MINUS ? "sub"
                                      : e->op == T_STAR  ? "imul"
                                                         : NULL;
                    if (ins != NULL) {
                        gen_leaf_to_reg(cg, e->rhs, "r11");
                        buf_printf(cg->out, "  %s %s, r11\n", ins, lreg);
                        return;
                    }
                }
            } else if (is_accum_chain(cg, e->rhs, e->lhs->slot)) {
                gen_accum(cg, e->rhs, lreg);
                return;
            } else {
                gen_expr(cg, e->rhs);
                buf_printf(cg->out, "  mov %s, rax\n", lreg);
                return;
            }
        }
    }
    gen_expr(cg, e);
}

static void loop_push(CG *cg, int brk, int cont) {
    if (cg->loop_depth < MAX_LOOP_DEPTH) {
        cg->loops[cg->loop_depth].brk = brk;
        cg->loops[cg->loop_depth].cont = cont;
    }
    cg->loop_depth++;
}

static void loop_pop(CG *cg) {
    if (cg->loop_depth > 0)
        cg->loop_depth--;
}

static void gen_stmt(CG *cg, Stmt *s) {
    /* Temporaries (including struct-return buffers) never live across a
     * statement boundary, so start each statement from a clean temp space. */
    cg->temp_top = 0;
    switch (s->kind) {
    case S_VAR:
        if (s->init != NULL) {
            LocalInfo *li = s->slot > 0 ? li_lookup(cg, s->slot) : NULL;
            if (li != NULL && li->is_const && s->init->kind == E_INT) {
                /* Propagated constant: no storage needed, reads are inlined. */
                break;
            }
            const char *reg = is_aggregate(s->type) ? NULL : local_reg(cg, s->slot);
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
    case S_RETURN:
        if (s->expr != NULL)
            gen_expr(cg, s->expr);
        if (cg->cur_ret_struct && s->expr != NULL && is_aggregate(s->expr->type)) {
            /* Copy the returned struct into the caller's hidden buffer. */
            buf_printf(cg->out, "  mov rsi, rax\n");
            buf_printf(cg->out, "  mov rdi, QWORD PTR [rbp - %d]\n", cg->cur_ret_slot);
            emit_memcpy(cg, type_size(s->expr->type));
        }
        buf_printf(cg->out, "  jmp .Lret_%s\n", cg->cur_sym);
        break;
    case S_IF: {
        int lelse = next_label(cg);
        int lend = next_label(cg);
        gen_cond_branch(cg, s->cond, lelse);
        gen_stmt(cg, s->body);
        buf_printf(cg->out, "  jmp .L%d\n.L%d:\n", lend, lelse);
        if (s->orelse != NULL)
            gen_stmt(cg, s->orelse);
        buf_printf(cg->out, ".L%d:\n", lend);
        break;
    }
    case S_WHILE: {
        int lstart = next_label(cg);
        int lend = next_label(cg);
        buf_printf(cg->out, ".L%d:\n", lstart);
        gen_cond_branch(cg, s->cond, lend);
        loop_push(cg, lend, lstart);
        gen_stmt(cg, s->body);
        loop_pop(cg);
        buf_printf(cg->out, "  jmp .L%d\n.L%d:\n", lstart, lend);
        break;
    }
    case S_FOR: {
        if (s->for_init != NULL)
            gen_stmt(cg, s->for_init);
        int lstart = next_label(cg);
        int lcont = next_label(cg);
        int lend = next_label(cg);
        buf_printf(cg->out, ".L%d:\n", lstart);
        if (s->cond != NULL)
            gen_cond_branch(cg, s->cond, lend);
        loop_push(cg, lend, lcont);
        gen_stmt(cg, s->body);
        loop_pop(cg);
        buf_printf(cg->out, ".L%d:\n", lcont);
        if (s->for_step != NULL)
            gen_void_expr(cg, s->for_step);
        buf_printf(cg->out, "  jmp .L%d\n.L%d:\n", lstart, lend);
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
    case S_BLOCK:
        gen_block_items(cg, s);
        break;
    case S_FUNC:
    case S_STRUCT:
    case S_UNION:
        break;
    }
}

static int align16(int n) { return (n + 15) & ~15; }

/* Every Z-level function is emitted under a reserved prefix.
 *
 * Bare Z names are not safe as assembly symbols. Under `.intel_syntax`, GAS
 * reads `near`, `far`, `byte`, `word`, `dword` and `qword` as keywords, so a
 * Z function called `near` assembled into a branch to an absolute address with
 * no diagnostic from the assembler or the linker -- the program just jumped
 * into the weeds. `short` and `offset` are outright directives and failed to
 * assemble. A prefix also keeps a Z function from colliding with a libc symbol
 * or one of the runtime's own z_* helpers.
 *
 * The separator is `$`, which the assembler accepts in a symbol but which
 * cannot appear in a Z identifier. That makes the collision impossible rather
 * than merely unlikely: no program can define a function whose name already
 * occupies this namespace, so `rnd` and an `export`ed `z_rnd` can coexist.
 * An identifier-shaped prefix such as `z_` cannot promise that.
 *
 * Every Z-level name is prefixed unconditionally. A C symbol is reached by
 * declaring it `extern`, and a Z symbol meant to be visible to C by declaring
 * it `export`; both bypass this function. */
#define Z_SYM_PREFIX "z$"

static const char *z_sym(CG *cg, const char *name) {
    if (name == NULL)
        return Z_SYM_PREFIX "<null>";
    size_t n = strlen(name);
    char *out = arena_alloc(cg->arena, n + sizeof Z_SYM_PREFIX);
    memcpy(out, Z_SYM_PREFIX, sizeof Z_SYM_PREFIX);
    memcpy(out + sizeof Z_SYM_PREFIX - 1, name, n + 1);
    return out;
}

/* Emits one function. The body is generated twice: once to measure peak
 * temporary-slot usage (to size the stack frame), then for real. */
static void emit_function(CG *cg, Stmt *fn) {
    /* The synthesized entry keeps its runtime-style name; an `export`ed
     * function keeps the name as written so C can find it; everything else the
     * user writes is mangled into the Z namespace. */
    const char *sym = fn->is_entry ? "z_main"
                                   : (fn->is_export ? fn->fname : z_sym(cg, fn->fname));
    cg->cur_locals_bytes = fn->locals_bytes;
    /* Struct-returning functions store the caller's buffer pointer in the
     * hidden first parameter (name "$ret"). */
    cg->cur_ret_struct = is_aggregate(fn->ret_type) ? 1 : 0;
    cg->cur_ret_slot = 0;
    if (cg->cur_ret_struct && fn->nparams > 0) {
        cg->cur_ret_slot = fn->params[0]->slot;
    }
    cg->cur_sym = sym;

    alloc_regs(cg, fn);

    size_t saved_len = cg->out->len;
    cg->temp_top = 0;
    cg->temp_high = 0;
    gen_block_items(cg, fn->fbody); /* measure pass */

    cg->out->len = saved_len;
    if (cg->out->data != NULL)
        cg->out->data[saved_len] = '\0';
    cg->temp_top = 0;

    /* Locals occupy [rbp-8-locals_bytes, rbp-9]; temporaries sit below that,
     * so the frame must cover 8 + locals + temps bytes. */
    /* temp_off puts the last temporary at 8 + locals + 8*temp_high, and that
     * slot occupies the eight bytes above it, so the frame has to reach one
     * slot further. Sizing it to exactly that offset left a function with, say,
     * 24 bytes of locals and two temporaries writing 8 bytes below its own
     * stack pointer -- align16 only concealed it when the total was not already
     * 16-aligned. */
    int frame = align16(8 + cg->cur_locals_bytes + 8 * (cg->temp_high + 1));
    /* Each pushed pool register shifts rsp by 8; keep the frame 16-aligned at
     * call sites by adding 8 when an odd number of pool registers is used. */
    int npool_used = 0;
    for (int r = 0; r < NPOOL; r++)
        if (cg->pool_mask & (1 << r))
            npool_used++;
    if (npool_used & 1)
        frame += 8;

    /* An exported symbol has to be visible outside this translation unit. */
    if (fn->is_export)
        buf_printf(cg->out, "  .globl %s\n", sym);
    buf_printf(cg->out, "%s:\n", sym);
    buf_printf(cg->out, "  push rbp\n  mov rbp, rsp\n");
    for (int r = 0; r < NPOOL; r++)
        if (cg->pool_mask & (1 << r))
            buf_printf(cg->out, "  push %s\n", POOL_REGS[r]);
    /* Local and temporary slots are numbered from rbp-8 downwards, which is
     * exactly where the pool registers just landed. Rebase rbp below them so
     * the two regions cannot overlap; the saved registers are then reachable
     * as [rbp+8] .. [rbp+8*N] and the epilogue's pops line up again. */
    if (npool_used > 0)
        buf_printf(cg->out, "  lea rbp, [rbp - %d]\n", 8 * npool_used);
    if (frame > 0)
        buf_printf(cg->out, "  sub rsp, %d\n", frame);
    for (int i = 0; i < fn->nparams; i++) {
        buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], %s\n", fn->params[i]->slot, ARG_REGS[i]);
    }
    gen_block_items(cg, fn->fbody);
    /* rsp is already at the deepest saved pool register once rbp is rebased,
     * so pop them in reverse push order and then rbp. The return value stays
     * in rax. */
    buf_printf(cg->out, ".Lret_%s:\n  mov rsp, rbp\n", sym);
    for (int r = NPOOL - 1; r >= 0; r--)
        if (cg->pool_mask & (1 << r))
            buf_printf(cg->out, "  pop %s\n", POOL_REGS[r]);
    buf_printf(cg->out, "  pop rbp\n  ret\n", sym);
}

static void emit_escaped(CG *cg, const char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            buf_puts(cg->out, "\\\"");
            break;
        case '\\':
            buf_puts(cg->out, "\\\\");
            break;
        case '\n':
            buf_puts(cg->out, "\\n");
            break;
        case '\t':
            buf_puts(cg->out, "\\t");
            break;
        case '\r':
            buf_puts(cg->out, "\\r");
            break;
        default:
            if (c < 32 || c >= 127)
                buf_printf(cg->out, "\\%03o", c);
            else {
                char ch[2] = {(char)c, '\0'};
                buf_puts(cg->out, ch);
            }
        }
    }
}

char *codegen_emit_opts(Arena *arena, Stmt *program, StringTable *strings,
                        const CodegenOptions *opts) {
    Buf out;
    memset(&out, 0, sizeof out);
    CG cg;
    memset(&cg, 0, sizeof cg);
    cg.out = &out;
    cg.strings = strings;
    cg.arena = arena;
    if (opts != NULL)
        cg.bounds_checks = opts->bounds_checks;

    buf_puts(&out, "  .intel_syntax noprefix\n  .text\n");

    Stmt *entry = NULL;
    for (int i = 0; i < program->nitems; i++) {
        if (program->items[i]->kind == S_FUNC && program->items[i]->is_entry) {
            entry = program->items[i];
        }
    }

    for (int i = 0; i < program->nitems; i++) {
        /* fbody == NULL is a declaration with no definition: a forward
         * declaration, or an `extern` whose body lives in C. */
        if (program->items[i]->kind == S_FUNC && program->items[i]->fbody != NULL &&
            !program->items[i]->is_generic_template)
            emit_function(&cg, program->items[i]);
    }

    /* Emit struct methods (mangled `Struct__method`) and property accessors. */
    for (int i = 0; i < program->nitems; i++) {
        if (program->items[i]->kind == S_STRUCT && program->items[i]->sdef != NULL) {
            StructDef *sd = program->items[i]->sdef;
            for (int mi = 0; mi < sd->nmethods; mi++) {
                if (sd->methods[mi]->body != NULL)
                    emit_function(&cg, sd->methods[mi]->body);
            }
        }
    }

    /* The C runtime calls main(); our entry point is renamed z_main so it
     * never collides with the user's own `main`. The `push rbp` realigns the
     * stack for the ABI. */
    if (entry != NULL) {
        /* Initialize the GC's stack base at the top of the stack, then enter. */
        buf_puts(&out, "  .globl main\nmain:\n  push rbp\n  mov rbp, rsp\n");
        buf_puts(&out, "  call z_gc_init\n  call z_main\n");
        if (!is_kind(entry->ret_type, TK_INT))
            buf_puts(&out, "  xor eax, eax\n");
        buf_puts(&out, "  pop rbp\n  ret\n");
    }

    buf_puts(&out, "  .section .rodata\n");
    /* Per-class vtables: an array of .quad function pointers, one per virtual
     * slot. Objects store a pointer to their class's vtable as the first word. */
    for (int i = 0; i < program->nitems; i++) {
        if (program->items[i]->kind != S_STRUCT || program->items[i]->sdef == NULL)
            continue;
        StructDef *sd = program->items[i]->sdef;
        if (!sd->is_class)
            continue;
        buf_printf(&out, ".Lvt_%s:\n", sd->name);
        for (int v = 0; v < sd->nvtable; v++)
            buf_printf(&out, "  .quad %s\n",
                       sd->vtable_impl[v] ? z_sym(&cg, sd->vtable_impl[v]) : "0");
    }
    /* 64-bit magic multipliers and divisors referenced by [rip + .Lro<i>]. */
    for (int i = 0; i < cg.nrodata; i++)
        buf_printf(&out, ".Lro%d:\n  .quad %llu\n", i, (unsigned long long)cg.rodata[i]);
    for (int i = 0; i < strings->count; i++) {
        buf_printf(&out, ".Lstr%d:\n  .asciz \"", i);
        emit_escaped(&cg, strings->items[i]);
        buf_puts(&out, "\"\n");
    }
    buf_puts(&out, ".Lfmt_int:\n  .asciz \"%ld\\n\"\n");
    buf_puts(&out, ".Ltrue_str:\n  .asciz \"true\"\n");
    buf_puts(&out, ".Lfalse_str:\n  .asciz \"false\"\n");
    buf_puts(&out, "  .section .note.GNU-stack,\"\",@progbits\n");

    if (out.data == NULL) {
        out.data = malloc(1);
        if (out.data == NULL)
            die_oom();
        out.data[0] = '\0';
    }
    free(cg.locals);
    free(cg.rodata);
    return out.data;
}

char *codegen_emit(Arena *arena, Stmt *program, StringTable *strings) {
    return codegen_emit_opts(arena, program, strings, NULL);
}
