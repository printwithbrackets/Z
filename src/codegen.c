#include "codegen.h"
#include "limits.h"

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

/* One described variable or parameter, ready to be written as a DIE once the
 * whole compile unit is assembled. */
typedef struct {
    const char *name;
    int file;
    int line;
    int slot;
    /* The DWARF register number this variable is live in, or -1 when it is in its
     * frame slot. The allocator either puts a local in memory or gives it a whole
     * callee-saved register for its entire life, so one location describes it
     * everywhere -- unlike a frame slot, which does not need a range list either.
     * This is what stops an optimised -g build from having no locals at all. */
    int reg;
    int type_code; /* dbg_type_of result, or 0 for untyped */
} DbgVar;

#define DBUG_MAX_VARS 256

typedef struct {
    const char *name;
    int file;
    int line;
    const char *begin_label;
    const char *end_label;
    /* dbg_type_of of the result, or 0 for untyped. Without this the subprogram
     * DIE carries no DW_AT_type at all and a reader infers `void`, so every
     * function looked like it returned nothing. */
    int ret_type;
    int nparams; /* how many leading entries of vars[] are parameters */
    DbgVar vars[DBUG_MAX_VARS];
    int nvars;
} DbgFunc;

#define DBUG_MAX_FUNCS 1024

/* Distinct source files a compilation unit spans (an `import` pulls in more),
 * which is how many entries the DWARF file table can hold. */
#define MAX_DBG_FILES 128

/* Callee-saved registers used to hold local variables whose address is never
 * taken, so their values stay in registers instead of reloading from the
 * stack on every use. rax is the accumulator and r10/r11 are expression
 * scratch, so the pool excludes them. */
static const char *const POOL_REGS[] = {"rbx", "r12", "r13", "r14", "r15"};

/* The DWARF register numbers for the pool, in the same order.
 *
 * DWARF 64-bit numbers the general-purpose registers in the System V order, so
 * this is not the same as anything in the source: rax is 0, rdx 1, rcx 2, rbx 3,
 * rsi 4, rdi 5, rbp 6, rsp 7, and r8 through r15 are 8 through 15. Only the five
 * the pool can hold are listed, since nothing else is ever described this way. */
static const int POOL_DWARF_REGNUM[] = {3, 12, 13, 14, 15};
#define NPOOL 5

/* One (implementing type, interface) pair whose itab has to exist. Collected
 * during the emit pass and written out at the end, so a conversion that is never
 * reached costs nothing. */
typedef struct {
    StructDef *impl;
    IfaceDef *idef;
} ItabUse;

#define MAX_ITABS 256

/* The symbol an itab is emitted under. Derived from both names, so two types
 * implementing one interface get two arrays, and one type implementing two
 * interfaces gets two arrays. */
static const char *iface_itab_symbol(IfaceDef *id, StructDef *impl) {
    static char bufs[8][160];
    static int next = 0;
    char *b = bufs[next];
    next = (next + 1) % 8;
    snprintf(b, sizeof bufs[0], "$itab$%s$%s", impl != NULL ? impl->name : "?",
             id != NULL ? id->name : "?");
    return b;
}

/* Per-scalar-local register-allocation info, keyed by frame slot. */
typedef struct {
    int slot;
    int eligible;   /* may live in a register */
    int d, u;       /* first-def / last-use statement index (d > u => unused) */
    int assigned;   /* index into POOL_REGS, or -1 */
    int const_cand; /* defined by a constant (E_INT) initializer */
    long long const_val;
    /* 1 when the local holds a float. A float is 8 bytes and lives in one slot,
     * but its value belongs in an XMM register, and the register pool here is
     * general-purpose only. Rather than grow a second pool and teach every pass
     * about both, float locals stay in the frame. That costs a load and a store
     * per access and is the conservative choice: the alternative is a register
     * class that half the optimizer does not model. */
    int is_float;
    int reassigned; /* written by an assignment somewhere in the function */
    int is_const;   /* const_cand && !reassigned => value is a compile-time constant */
    /* The loop this local was last read or written in, used to extend its live
     * range across the whole loop. See the S_FOR case in walk_alloc_stmt: an
     * interval is a line, and a value read in a loop's condition is live again on
     * every iteration, not just up to where the condition happens to appear in
     * the emitted order. */
    int loop_marker;
} LocalInfo;

/* How deep inlined bodies may nest. Shared with the parser, which enforces the
 * same limit, so the two agree on what the emitter can hold. */
#define INLINE_MAX_DEPTH 4

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
    int debug_info;    /* emit DWARF: line table plus function/local DIEs */
    int opt;           /* Z_OPT_* level this function is generated at */
    int measuring;     /* 1 during the sizing pass, which emits no code */
    /* Interface conversions actually reached, and the itabs they need. Collected
     * during the emit pass and written out at the end of the file. */
    ItabUse itabs[MAX_ITABS];
    int nitabs;
    /* The label each enclosing inlined body ends at, innermost last. An
     * S_LEAVE jumps to the top of this. */
    int inl_label[INLINE_MAX_DEPTH + 2];
    int inl_depth;
    int emitting_hoist;   /* 1 while emitting a hoisted node's own value */
    Arena *arena;         /* for interned assembly symbol names */
    int temp_top;         /* current temporary high-water during emit */
    int temp_high;        /* max temp slots used in this function */
    int cur_locals_bytes; /* total frame bytes used by locals in current fn */
    int cur_ret_slot;     /* frame offset of the hidden $ret buffer (struct returns) */
    int cur_ret_struct;   /* 1 if the current function returns a struct by sret */
    const char *cur_sym;
    LocalInfo *locals; /* per-function register-allocation candidates */
    int nlocals, loc_cap;
    int pool_mask;   /* bitmask of POOL_REGS indices actually used */
    int loop_marker; /* incremented once per loop walked; see LocalInfo.loop_marker */
    /* Frame slots holding fresh string values that nothing has taken over yet.
     * A temporary lives until the end of the statement that made it, which is
     * the smallest scope that is always reached: `Console.WriteLog(a + b)` allocates a
     * string that no variable ever names, and without this it is live until the
     * program exits.
     *
     * They are *pinned*, which is the whole difficulty. The temp stack is
     * reclaimed by every assignment and call by resetting `temp_top`, so a slot
     * holding a temporary would be handed to the next expression inside the same
     * statement and the pointer overwritten before the release reads it. Pinning
     * makes `temp_alloc` step over them, and the statement that made them unpins
     * them on the way out so the frame does not grow without bound. */
    int *pinned;
    int npinned;
    int pin_cap;
    /* Nonzero while generating an expression that may not run at all: the untaken
     * arm of a ternary, or the right operand of a short-circuited `&&`/`||`. Both
     * are emitted, so anything that records a temporary would leave a slot holding
     * the *previous* statement's pointer, and the release at the end of this
     * statement would free that. Suppressing is the safe direction: the cost when
     * the branch does run is a leak of one value, against a double free when it
     * does not. */
    int no_str_temp;
    uint64_t *rodata; /* 64-bit constants (magic multipliers etc.) */
    int nrodata, ro_cap;
    /* Type names `to_text` needs to print an aggregate, collected as the calls
     * are generated. Exactly the types that are printed and no more -- a table of
     * every type in the unit would need the type context here, and would emit a
     * label for types nothing ever mentions. */
    const char **tname;
    int ntname, tname_cap;

    /* ---- DWARF ----
     *
     * The section contents are not buffered: a DWARF section contains addresses,
     * and the only way to write an address codegen does not know yet is to leave
     * a label reference in the assembly text for the linker to resolve. What is
     * buffered here is the *description* of each function, gathered while the
     * code is emitted and turned into a section at the end. */
    int dbg_file_count; /* distinct source files, i.e. .file entries */
    const char *dbg_file_names[MAX_DBG_FILES];
    DbgFunc *dbg_funcs; /* one entry per emitted function */
    int dbg_nfuncs, dbg_funcs_cap;
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
static int temp_alloc(CG *cg);

static int temp_alloc_many(CG *cg, int n) {
    /* Reserve `n` slots. It skips pinned ones for the same reason `temp_alloc`
     * does: a pinned slot is holding a string temporary that has to survive until
     * the end of the statement, and a block reservation that walked over one
     * would hand the caller a range containing a live pointer, which is how an
     * integer ended up in a slot the statement was about to free. */
    int base = 0;
    for (int i = 0; i < n; i++) {
        int t = temp_alloc(cg);
        if (i == 0)
            base = t;
    }
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

/* True at a level where the optimizer runs. Everything the M7 pass does --
 * constant folding and propagation, register allocation, leaf and immediate
 * operand selection, branch-on-flags, in-place compound assignment, constant
 * strength reduction -- hangs off constant folding and register allocation, so
 * switching those off at -O0 switches off the whole set. */
static int opt_on(const CG *cg) { return cg->opt >= Z_OPT_DEFAULT; }

static void dbg_loc(CG *cg, Span span);
static void gen_call(CG *cg, Expr *e);
static void gen_icall(CG *cg, Expr *e);
static void gen_vcall(CG *cg, Expr *e);
static int is_compare_op(TokenKind k);
static int next_label(CG *cg) { return ++cg->label_counter; }

/* ---- argument passing ----
 *
 * The System V AMD64 ABI numbers the integer registers and the vector registers
 * independently, so a float does not displace the integers that follow it, and
 * it caps them separately: six integer registers, eight vector registers.
 * Anything past its cap goes on the stack, eight bytes per slot, in argument
 * order -- so the leftmost overflowing argument is at the lowest address.
 *
 * `hidden` counts leading arguments that are not in the source: a struct-return
 * buffer, a bound receiver, a closure's environment. They occupy the first slots
 * of the integer sequence, which is why a struct-returning function can pass one
 * fewer argument in a register than one returning a scalar.
 *
 * Both the call site and the callee's prologue run this. That is the point: if
 * the two ever disagreed about which register a parameter is in, the program
 * would read a live register as if it held something else, and nothing would say
 * so. */
#define Z_MAX_REG_ARGS 6
/* x86-64 has sixteen vector argument registers, not eight. The old figure was
 * the number SysV passes in the *integer* sequence, which is a different limit
 * and the wrong one to be reading from here. */
#define Z_MAX_REG_FLT 16

/* The vector argument registers by name. A table rather than a formatted
 * buffer, because an `ArgAssign` outlives the loop that fills it: naming a
 * register through a pointer into a dead stack slot gave every float argument
 * whichever name that slot happened to end up holding, so a call with two floats
 * passed both in xmm1. */
static const char *const XMM_ARG_REGS[Z_MAX_REG_FLT] = {
    "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
};

typedef struct {
    /* The register an argument arrives in -- "rdi", "xmm0", ... -- or NULL when
     * it arrives on the stack. */
    const char *reg[Z_MAX_ARGS];
    /* Byte offset from the stack pointer at the call, for a stack argument.
     * -1 for a register argument. */
    int soff[Z_MAX_ARGS];
    int nstack; /* how many arguments go on the stack */
    int pad;    /* bytes to subtract from rsp, already rounded to 16 */
} ArgAssign;

static void assign_args(Type **ptypes, int n, int hidden, ArgAssign *out) {
    memset(out, 0, sizeof *out);
    int nint = hidden, nflt = 0, nstack = 0;
    for (int i = 0; i < n; i++) {
        out->soff[i] = -1;
        if (is_kind(ptypes[i], TK_F64)) {
            if (nflt < Z_MAX_REG_FLT) {
                out->reg[i] = XMM_ARG_REGS[nflt];
            } else {
                out->soff[i] = nstack * 8;
                nstack++;
            }
            nflt++;
        } else {
            if (nint < Z_MAX_REG_ARGS) {
                out->reg[i] = ARG_REGS[nint];
            } else {
                out->soff[i] = nstack * 8;
                nstack++;
            }
            nint++;
        }
    }
    out->nstack = nstack;
    /* The stack has to be 16-byte aligned at the call, so the adjustment is a
     * whole number of 16-byte units. The frame is already aligned, so this is
     * enough however many arguments there are. */
    out->pad = (nstack * 8 + 15) & ~15;
}

static void store_temp(CG *cg, int t) {
    buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", temp_off(cg, t));
}

static void load_temp(CG *cg, int t, const char *reg) {
    buf_printf(cg->out, "  mov %s, QWORD PTR [rbp - %d]\n", reg, temp_off(cg, t));
}

static int temp_alloc(CG *cg) {
    int t = cg->temp_top++;
    /* A pinned slot is holding a string temporary that has to survive until the
     * end of the statement, so it cannot be handed out again. Rescan from the
     * start after a step, because the skipped index may itself be followed by
     * another pinned one. The list is a handful of entries at most. */
    for (int i = 0; i < cg->npinned; i++) {
        if (cg->pinned[i] == t) {
            t = cg->temp_top++;
            i = -1;
        }
    }
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    return t;
}

/* ---- float values ----
 *
 * A float is eight bytes, so it occupies a frame slot or a temporary slot exactly
 * like an int does. What differs is which register holds it while it is being
 * computed: floats live in XMM registers, so they are moved with `movsd` rather
 * than `mov`, and combined with the SSE arithmetic instructions rather than the
 * integer ones. A temporary therefore holds either an integer or a float
 * depending on what was stored there, and the code that stores it is the code
 * that knows which.
 *
 * xmm0 is the accumulator, exactly as rax is for integers: an expression leaves
 * its value there. xmm1..xmm3 are scratch. */

static const char *const XMM_ACC = "xmm0";
static const char *const XMM_SCRATCH[] = {"xmm1", "xmm2", "xmm3"};

/* Stores the float in xmm0 into a temporary slot. */
static void store_temp_x(CG *cg, int t) {
    buf_printf(cg->out, "  movsd QWORD PTR [rbp - %d], %s\n", temp_off(cg, t), XMM_ACC);
}

/* Loads a temporary slot holding a float into `reg`. */
static void load_temp_x(CG *cg, int t, const char *reg) {
    buf_printf(cg->out, "  movsd %s, QWORD PTR [rbp - %d]\n", reg, temp_off(cg, t));
}

/* Places already-staged arguments where the ABI says they go, opening the
 * outgoing-argument area if any of them spill.
 *
 * The arguments are in frame temps by the time this runs, which is what makes it
 * safe to write them through rsp: a nested call in an argument expression would
 * otherwise move rsp between computing an offset and using it. `hidden` counts
 * leading arguments the caller supplies but the source does not name. */
static void args_prologue(CG *cg, Expr *e, int base, int hidden, ArgAssign *aa) {
    Type **atypes =
        arena_alloc_array(cg->arena, (size_t)(e->nargs > 0 ? e->nargs : 1), sizeof(Type *));
    for (int i = 0; i < e->nargs; i++)
        atypes[i] = e->args[i]->type;
    assign_args(atypes, e->nargs, hidden, aa);
    if (aa->nstack > 0) {
        buf_printf(cg->out, "  sub rsp, %d\n", aa->pad);
        /* r11 is the scratch: it is not an argument register, so filling the stack
         * slots cannot disturb an argument already placed in one. */
        for (int i = 0; i < e->nargs; i++) {
            if (aa->reg[i] != NULL || aa->soff[i] < 0)
                continue;
            if (is_kind(e->args[i]->type, TK_F64)) {
                buf_printf(cg->out, "  movsd QWORD PTR [rsp + %d], %s\n", aa->soff[i], XMM_ACC);
            } else {
                buf_printf(cg->out, "  mov r11, QWORD PTR [rbp - %d]\n", temp_off(cg, base + i));
                buf_printf(cg->out, "  mov QWORD PTR [rsp + %d], r11\n", aa->soff[i]);
            }
        }
    }
    for (int i = 0; i < e->nargs; i++) {
        if (aa->reg[i] == NULL)
            continue;
        if (is_kind(e->args[i]->type, TK_F64))
            load_temp_x(cg, base + i, aa->reg[i]);
        else
            load_temp(cg, base + i, aa->reg[i]);
    }
}

/* Closes what args_prologue opened. */
static void args_epilogue(CG *cg, const ArgAssign *aa) {
    if (aa->nstack > 0)
        buf_printf(cg->out, "  add rsp, %d\n", aa->pad);
}

/* The bits of `v` as a 64-bit pattern. A double's representation *is* its bit
 * pattern, so this is the whole conversion. Written out rather than punned
 * through a union so it does not depend on the host's endianness matching the
 * target's, which is the one assumption a memcpy pun would quietly make. */
static uint64_t double_bits(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

/* Interns a double and returns its .rodata index. Shares the constant pool with
 * the integer magic numbers, because a pool of 64-bit words is all either needs
 * and the two never collide. */
static int rodata_add_double(CG *cg, double v) { return rodata_add(cg, double_bits(v)); }

/* True for the expression kinds whose value is a float that gen_float knows how
 * to produce, and which have no effect of their own. */
static int is_float_value_expr(Expr *e) {
    switch (e->kind) {
    case E_F64:
    case E_CVT:
    case E_VAR:
    case E_FIELD:
    case E_INDEX:
    case E_DEREF:
    case E_UNARY:
    case E_TERNARY:
    case E_CALL:
    case E_ICALL:
    case E_VCALL:
        return 1;
    default:
        return 0;
    }
}

/* Puts the value of `e` into xmm0, whatever shape it has: a literal, a local, a
 * conversion, or a nested expression. */
static void gen_float(CG *cg, Expr *e);

static void gen_float(CG *cg, Expr *e) {
    switch (e->kind) {
    case E_F64: {
        int idx = rodata_add_double(cg, e->dval);
        buf_printf(cg->out, "  movsd %s, QWORD PTR [rip + .Lro%d]\n", XMM_ACC, idx);
        return;
    }
    case E_INT: {
        /* An int reaching a float instruction has already been widened by the
         * parser, so this only fires for a literal folded early. */
        int idx = rodata_add_double(cg, (double)e->ival);
        buf_printf(cg->out, "  movsd %s, QWORD PTR [rip + .Lro%d]\n", XMM_ACC, idx);
        return;
    }
    case E_CVT: {
        Type *from = e->lhs->type;
        if (is_kind(e->type, TK_F64)) {
            if (is_kind(from, TK_F64)) {
                gen_float(cg, e->lhs);
            } else {
                /* int -> float. cvtsi2sd reads a 64-bit signed integer, and
                 * leaves the float in the low half of the destination. */
                gen_expr(cg, e->lhs);
                buf_printf(cg->out, "  cvtsi2sd %s, rax\n", XMM_ACC);
            }
            return;
        }
        /* float -> int: truncate toward zero, which is what every conversion
         * from a floating type to an integer does. `cvttsd2si` is the
         * truncating form; the rounding one would round 2.5 to 2 and 3.5 to 4,
         * which is not what a cast means. */
        gen_float(cg, e->lhs);
        buf_printf(cg->out, "  cvttsd2si rax, %s\n", XMM_ACC);
        return;
    }
    case E_VAR: {
        if (e->agg_param)
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rax]\n", XMM_ACC);
        else
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rbp - %d]\n", XMM_ACC, e->slot);
        return;
    }
    case E_FIELD: {
        gen_addr(cg, e);
        buf_printf(cg->out, "  movsd %s, QWORD PTR [rax]\n", XMM_ACC);
        return;
    }
    case E_INDEX: {
        /* gen_addr leaves the element's address in rax. */
        gen_addr(cg, e);
        buf_printf(cg->out, "  movsd %s, QWORD PTR [rax]\n", XMM_ACC);
        return;
    }
    case E_BINARY: {
        /* Only float *arithmetic* reaches here. A comparison's result is a bool,
         * so gen_expr routes it to gen_float_cmp instead; falling through to the
         * default would call gen_expr again and recurse until the stack ran out,
         * which is what a stray comparison node here once did. */
        static const char *const ops[] = {"addsd", "subsd", "mulsd", "divsd"};
        TokenKind op = e->op;
        const char *ins = NULL;
        for (size_t i = 0; i < sizeof ops / sizeof(*ops); i++) {
            TokenKind want = (TokenKind[]){T_PLUS, T_MINUS, T_STAR, T_SLASH}[i];
            if (op == want)
                ins = ops[i];
        }
        /* Both operands are staged so the *left* one can be the destination.
         * subsd and divsd are not commutative: with the operands the other way
         * round they quietly compute `rhs - lhs` and `rhs / lhs`, which is a
         * miscompile that no test of addition would ever notice. */
        int tl = temp_alloc(cg);
        int tr = temp_alloc(cg);
        gen_float(cg, e->lhs);
        store_temp_x(cg, tl);
        gen_float(cg, e->rhs);
        store_temp_x(cg, tr);
        load_temp_x(cg, tl, XMM_ACC);
        /* The right operand is read straight from its slot: an SSE arithmetic
         * instruction takes a memory operand, so this is one instruction and no
         * extra register shuffle. */
        buf_printf(cg->out, "  %s %s, QWORD PTR [rbp - %d]\n", ins, XMM_ACC, temp_off(cg, tr));
        cg->temp_top = tl;
        return;
    }
    case E_UNARY: {
        gen_float(cg, e->lhs);
        if (e->op == T_MINUS) {
            /* Exclusive-or with the sign bit. Negating by multiplying by -1.0
             * would be simpler and wrong: it turns -0.0 into +0.0 and turns
             * infinity into itself, because both have their sign bit set
             * already. Exclusive-or flips exactly the one bit it means to. */
            int idx = rodata_add(cg, 0x8000000000000000ULL);
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rip + .Lro%d]\n", XMM_SCRATCH[1], idx);
            /* Destination first: `xorpd xmm0, xmm1` negates xmm0 in place.
             * Writing the mask register first would leave the negated value
             * there, and everything downstream reads xmm0. */
            buf_printf(cg->out, "  xorpd %s, %s\n", XMM_ACC, XMM_SCRATCH[1]);
        }
        return;
    }
    case E_TERNARY: {
        int te = temp_alloc(cg);
        int fe = temp_alloc(cg);
        int lelse = next_label(cg);
        int lend = next_label(cg);
        gen_expr(cg, e->lhs); /* the bool condition, into rax */
        buf_printf(cg->out, "  cmp rax, 0\n  je .L%d\n", lelse);
        gen_float(cg, e->rhs);
        store_temp_x(cg, te);
        buf_printf(cg->out, "  jmp .L%d\n.L%d:\n", lend, lelse);
        gen_float(cg, e->args[0]);
        store_temp_x(cg, te);
        buf_printf(cg->out, ".L%d:\n", lend);
        load_temp_x(cg, te, XMM_ACC);
        cg->temp_top = fe;
        return;
    }
    case E_CALL:
    case E_ICALL:
    case E_VCALL: {
        /* Dispatch by kind, not to gen_call for all three. A float-valued call
         * through a pointer is an indirect call, and gen_call would look for a
         * *name* to call -- of which an E_ICALL has none -- and emit a call to a
         * null symbol. The result of a float-returning call is already in xmm0,
         * which is where gen_float expects to find it, so no move is needed. */
        if (e->kind == E_ICALL)
            gen_icall(cg, e);
        else if (e->kind == E_VCALL)
            gen_vcall(cg, e);
        else
            gen_call(cg, e);
        return;
    }
    default:
        gen_expr(cg, e);
        return;
    }
}

/* Compares two floats and leaves a 0 or 1 in rax.
 *
 * The result follows IEEE-754 rather than intuition. `ucomisd` sets ZF, PF and
 * CF as if the two values were unsigned integers, and sets all three when the
 * comparison is *unordered*, which is what happens if either side is NaN. So:
 *
 *   ==   ZF=1 and PF=0        (PF excludes NaN)
 *   !=   ZF=0 or  PF=1        (NaN is not equal to anything, including itself)
 *   <    CF=1 and PF=0        (operands swapped so "less" lands in CF)
 *   <=   CF=1 or  ZF=1, and PF=0
 *   >    CF=0 and ZF=0        (NaN sets CF, so it is already excluded)
 *   >=   CF=0                 (likewise)
 *
 * The signed conditions are `setb`/`setbe` and the unsigned ones `seta`/`setae`;
 * mixing them up is the classic way to get a comparison that works on every
 * input except the one nobody tested. */
static void gen_float_cmp(CG *cg, Expr *e) {
    int t = temp_alloc(cg);
    /* The left operand ends up in xmm0 and the right in xmm1, so the flags read
     * in the same direction as the source. Getting this the other way round
     * silently inverts every ordering comparison, which is exactly the kind of
     * bug that looks like a working compiler until the first `if (a < b)`. */
    gen_float(cg, e->rhs);
    store_temp_x(cg, t);
    gen_float(cg, e->lhs);
    load_temp_x(cg, t, XMM_SCRATCH[0]);
    cg->temp_top = t;

    TokenKind op = e->op;
    if (op == T_LT || op == T_LE) {
        /* "less" lands in CF, and "less or equal" is CF-or-ZF -- but an
         * unordered comparison sets CF too, so CF alone would report every NaN
         * comparison as true. The parity flag is the one that distinguishes
         * "less" from "unordered", so it has to be folded in. */
        buf_printf(cg->out, "  ucomisd %s, %s\n", XMM_ACC, XMM_SCRATCH[0]);
        buf_printf(cg->out, "  set%s al\n", op == T_LT ? "b" : "be");
        buf_puts(cg->out, "  setnp cl\n  and al, cl\n");
    } else if (op == T_GT || op == T_GE) {
        buf_printf(cg->out, "  ucomisd %s, %s\n", XMM_ACC, XMM_SCRATCH[0]);
        buf_printf(cg->out, "  set%s al\n", op == T_GT ? "a" : "ae");
    } else if (op == T_EQ) {
        buf_printf(cg->out, "  ucomisd %s, %s\n", XMM_ACC, XMM_SCRATCH[0]);
        buf_puts(cg->out, "  sete al\n  setnp cl\n  and al, cl\n");
    } else { /* T_NE */
        buf_printf(cg->out, "  ucomisd %s, %s\n", XMM_ACC, XMM_SCRATCH[0]);
        buf_puts(cg->out, "  setne al\n  setp cl\n  or al, cl\n");
    }
    buf_puts(cg->out, "  movzx eax, al\n");
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
    li->is_float = 0;
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
    li->loop_marker = cg->loop_marker;
}

/* Records a definition of a local at statement index idx. */
static void li_def(CG *cg, int slot, int idx) {
    LocalInfo *li = li_for(cg, slot);
    if (li->d < 0 || idx < li->d)
        li->d = idx;
    if (li->u < idx)
        li->u = idx;
    li->loop_marker = cg->loop_marker;
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
    case E_POSTINC:
        /* The variable is written, so an element or field operand is addressed
         * and its base is marked. Without this a `d[n++]++` would allocate no
         * slot for the base it has to compute an address from. */
        mark_addr_taken(cg, e->lhs);
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
        walk_alloc_expr(cg, s->cond, cur);
        walk_alloc_block(cg, s->body, idx);
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
        walk_alloc_expr(cg, s->cond, (*idx)++);
        walk_alloc_block(cg, s->body, idx);
        walk_alloc_expr(cg, s->for_step, (*idx)++);
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

    /* Greedy colouring in order of first definition, skipping the locals already
     * settled as constants: they need no storage and no register. */
    for (int i = 0; i < cg->nlocals; i++) {
        LocalInfo *li = &cg->locals[i];
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
    if (e == NULL || e->kind != E_VAR || e->agg_param || is_aggregate(e->type))
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
        if (e->boxed) {
            /* The address of a captured variable is inside its box. */
            buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->slot);
            return;
        }
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
        /* A string element is a byte, so its address is base + index with no
         * scaling. The length lives in the header, so there is a bound to check
         * it against exactly as an array has one at ptr[-8]. */
        int is_str = is_kind(e->lhs->type, TK_STRING);
        int esz = is_str ? 1 : type_size(e->lhs->type->base);
        int t = temp_alloc(cg);
        gen_expr(cg, e->lhs); /* base pointer */
        store_temp(cg, t);
        gen_expr(cg, e->rhs); /* index */
        /* Arrays in Z are always heap allocations from z_newarray, so a
         * value of array type always has a valid length header at ptr[-8] and
         * the index can be range-checked. A string has its length 16 bytes
         * back. A TK_PTR index has neither, so there is nothing to check
         * against and it is left alone. */
        if (cg->bounds_checks && (is_kind(e->lhs->type, TK_ARRAY) || is_str)) {
            int ti = temp_alloc(cg);
            int lbad = next_label(cg);
            int lok = next_label(cg);
            store_temp(cg, ti);      /* rax = index */
            load_temp(cg, t, "r11"); /* r11 = base */
            buf_printf(cg->out, "  cmp rax, 0\n  jl .L%d\n", lbad);
            buf_printf(cg->out, "  mov rcx, QWORD PTR [r11 - %d]\n", is_str ? 16 : 8);
            buf_printf(cg->out, "  cmp rax, rcx\n  jge .L%d\n", lbad);
            load_temp(cg, ti, "rax");
            buf_printf(cg->out, "  jmp .L%d\n", lok);
            buf_printf(cg->out, ".L%d:\n", lbad);
            buf_printf(cg->out, "  mov rdi, rax\n  mov rsi, rcx\n");
            buf_printf(cg->out, "  call %s\n", is_str ? "z_str_bounds_fail" : "z_bounds_fail");
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
static void gen_expr(CG *cg, Expr *e);

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

/* The same operators, naming a register other than r11 for the right operand.
 *
 * `shl` and `sar` are the two that cannot simply substitute: the shift count has
 * to be in cl, and r11 is the only scratch register guaranteed free of the
 * argument registers at this point. Naming a callee-saved register instead would
 * be fine, so those two fall back to moving the operand into r11 first. */
/* The same operators, naming a register other than r11 for the right operand.
 *
 * The shifts are the two that cannot simply substitute: the count has to be in
 * cl, and the operand therefore has to be somewhere the `mov rcx, reg` form can
 * read. A callee-saved register satisfies that, so no move is needed for any
 * register this can be handed -- the guard is there so a future caller passing
 * an argument register still gets correct code. */
static void emit_binop_reg(CG *cg, TokenKind op, const char *reg) {
    switch (op) {
    case T_PLUS:
        buf_printf(cg->out, "  add rax, %s\n", reg);
        break;
    case T_MINUS:
        buf_printf(cg->out, "  sub rax, %s\n", reg);
        break;
    case T_STAR:
        buf_printf(cg->out, "  imul rax, %s\n", reg);
        break;
    case T_SLASH:
        buf_printf(cg->out, "  cqo\n  idiv %s\n", reg);
        break;
    case T_PERCENT:
        buf_printf(cg->out, "  cqo\n  idiv %s\n  mov rax, rdx\n", reg);
        break;
    case T_AMP:
        buf_printf(cg->out, "  and rax, %s\n", reg);
        break;
    case T_PIPE:
        buf_printf(cg->out, "  or rax, %s\n", reg);
        break;
    case T_CARET:
        buf_printf(cg->out, "  xor rax, %s\n", reg);
        break;
    case T_SHL:
        buf_printf(cg->out, "  mov rcx, %s\n  shl rax, cl\n", reg);
        break;
    case T_SHR:
        buf_printf(cg->out, "  mov rcx, %s\n  sar rax, cl\n", reg);
        break;
    case T_EQ:
        buf_printf(cg->out, "  cmp rax, %s\n  sete al\n  movzx rax, al\n", reg);
        break;
    case T_NE:
        buf_printf(cg->out, "  cmp rax, %s\n  setne al\n  movzx rax, al\n", reg);
        break;
    case T_LT:
        buf_printf(cg->out, "  cmp rax, %s\n  setl al\n  movzx rax, al\n", reg);
        break;
    case T_LE:
        buf_printf(cg->out, "  cmp rax, %s\n  setle al\n  movzx rax, al\n", reg);
        break;
    case T_GT:
        buf_printf(cg->out, "  cmp rax, %s\n  setg al\n  movzx rax, al\n", reg);
        break;
    case T_GE:
        buf_printf(cg->out, "  cmp rax, %s\n  setge al\n  movzx rax, al\n", reg);
        break;
    default:
        buf_printf(cg->out, "  add rax, %s\n", reg);
        break;
    }
}

/* The same operators with the right operand already in r11, which is the form
 * every other caller reaches: the left is in rax and the right is materialized
 * into r11 by the caller. */
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

/* Evaluate call arguments into frame temps starting at `base`. A float argument
 * has to go through gen_float, which leaves its value in xmm0; staging it with
 * gen_expr instead would save rax, which at that point usually holds the
 * receiver or the object just allocated rather than the value, and the callee
 * would read back a small integer as a denormal double. */
static void gen_args_stage(CG *cg, Expr *e, int base) {
    for (int i = 0; i < e->nargs; i++) {
        if (is_kind(e->args[i]->type, TK_F64)) {
            gen_float(cg, e->args[i]);
            store_temp_x(cg, base + i);
        } else {
            gen_expr(cg, e->args[i]);
            store_temp(cg, base + i);
        }
    }
}

/* The two type-directed built-ins. Both take a parameter declared `any`, which
 * means the parser recorded no type for it, and both are lowered here where the
 * argument's static type is still known. A generic function cannot do this
 * itself: it may ask `typeof(x)`, but every branch after the test still has to
 * type-check, so the branch that would have run is the branch that would not
 * compile.
 *
 * Both are recognised by their declared parameter being TK_ANY rather than by
 * name, so a user function named `to_text` is not mistaken for one of these --
 * a user function's parameters are real types, and a user function whose
 * parameter is literally `any` is not writable. */
static int is_to_text_call(Expr *e) {
    return e->nargs == 1 && e->args[0] != NULL && e->args[0]->type != NULL &&
           e->args[0]->type->kind != TK_ANY && strcmp(e->name, "z_to_text") == 0;
}

static int is_hash_of_call(Expr *e) {
    return e->nargs == 2 && e->args[0] != NULL && e->args[0]->type != NULL &&
           e->args[0]->type->kind != TK_ANY && strcmp(e->name, "z_hash_of") == 0;
}

static int is_drop_value_call(Expr *e) {
    return e->nargs == 1 && e->args[0] != NULL && e->args[0]->type != NULL &&
           e->args[0]->type->kind != TK_ANY && strcmp(e->name, "z_drop_value") == 0;
}

/* True if this is `hold` with a fractional number of seconds, which the runtime
 * has to be told about separately because the argument arrives in xmm rather than
 * in rdi and is a double rather than a whole count of seconds.
 *
 * Recognised by its declared parameter being TK_ANY rather than by the name alone,
 * so a user function that happens to be called `hold` is not mistaken for it --
 * a user function's parameters are real types, and one whose parameter is
 * literally `any` is not writable. */
static int is_holdf_call(Expr *e) {
    return e->nargs == 1 && e->args[0] != NULL && is_kind(e->args[0]->type, TK_F64) &&
           strcmp(e->name, "z_hold") == 0;
}

/* The runtime entry `to_text(x)` lowers to for a value of type `t`. */
static const char *to_text_symbol(Type *t) {
    switch (t->kind) {
    case TK_STRING:
        return "z_str_to_text";
    case TK_BOOL:
        return "z_bool_to_text";
    case TK_F64:
        return "z_float_to_text";
    case TK_VOID:
        return "z_void_to_text";
    case TK_STRUCT:
    case TK_UNION:
    case TK_ARRAY:
        /* An aggregate has no canonical text here, so its type's name is what
         * gets printed -- stable between runs, and more use than an address. */
        return "z_agg_to_text";
    default:
        /* int, a pointer, a function pointer, a closure, an interface: all one
         * machine word, and all read best as a number except the pointers, which
         * read better in hex. */
        return t->kind == TK_INT ? "z_int_to_text" : "z_ptr_to_text";
    }
}

/* Records a type name so the data section can carry the string the call
 * refers to, and returns it. `arena` keeps the name alive: the type's own name
 * is arena-owned already, so this stores the pointer rather than a copy. */
static const char *note_type_name(CG *cg, const char *name) {
    for (int i = 0; i < cg->ntname; i++)
        if (strcmp(cg->tname[i], name) == 0)
            return name;
    if (cg->ntname == cg->tname_cap) {
        int ncap = cg->tname_cap == 0 ? 8 : cg->tname_cap * 2;
        const char **bigger = realloc(cg->tname, (size_t)ncap * sizeof(char *));
        if (bigger == NULL)
            die_oom();
        cg->tname = bigger;
        cg->tname_cap = ncap;
    }
    cg->tname[cg->ntname++] = name;
    return name;
}

static void gen_to_text(CG *cg, Expr *e) {
    Type *at = e->args[0]->type;
    if (is_kind(at, TK_STRUCT) || is_kind(at, TK_UNION) || is_kind(at, TK_ARRAY)) {
        /* The runtime wants a pointer to the name, not the value. */
        const char *tn = is_kind(at, TK_STRUCT)  ? at->sdef->name
                         : is_kind(at, TK_UNION) ? at->udef->name
                                                 : "array";
        if (!is_kind(at, TK_ARRAY))
            note_type_name(cg, tn);
        buf_printf(cg->out, "  lea rdi, [rip + .Ltype_%s]\n", tn);
        buf_printf(cg->out, "  call z_agg_to_text\n");
        return;
    }
    if (is_kind(at, TK_F64)) {
        gen_float(cg, e->args[0]);
        buf_printf(cg->out, "  call %s\n", to_text_symbol(at));
        return;
    }
    gen_expr(cg, e->args[0]);
    buf_printf(cg->out, "  mov rdi, rax\n  call %s\n", to_text_symbol(at));
}

static void gen_hash_of(CG *cg, Expr *e) {
    Type *at = e->args[0]->type;
    gen_expr(cg, e->args[1]); /* the table size */
    buf_printf(cg->out, "  mov rdi, rax\n");
    store_temp(cg, temp_alloc(cg));
    int t = cg->temp_top - 1;
    if (is_kind(at, TK_STRING)) {
        gen_expr(cg, e->args[0]);
    } else {
        gen_expr(cg, e->args[0]);
    }
    load_temp(cg, t, "rsi");
    buf_printf(cg->out, "  mov rdi, rax\n  call %s\n",
               is_kind(at, TK_STRING) ? "z_hash_str" : "z_hash_num");
    cg->temp_top = t;
}

static void gen_drop_value(CG *cg, Expr *e) {
    Type *at = e->args[0]->type;
    /* Only a string owns something today, so every other type lowers to
     * nothing at all rather than to a call. The element is not even read: an
     * `int` in an array is four bytes of whatever was there before, and loading
     * it would be a read the program never asked for. */
    if (!is_kind(at, TK_STRING))
        return;
    Expr *arg = e->args[0];
    int is_slot = arg->kind == E_INDEX || arg->kind == E_FIELD;
    if (!is_slot) {
        gen_expr(cg, arg);
        buf_printf(cg->out, "  mov rdi, rax\n  call z_str_free\n");
        return;
    }
    /* A slot is released *and* blanked, and the order is the whole point.
     *
     * An assignment to an array element already releases the value that was
     * there, so the slot a released element leaves behind is not inert: the next
     * push into it frees the bytes this call just freed. Releasing by hand and
     * walking away is therefore not a way to write this, it is a double free
     * waiting for the vector to be refilled. Storing `.Lstrempty` over the slot
     * leaves a zero-capacity string there, which is exactly what the runtime's
     * free is a no-op on, so the slot can be written again safely.
     *
     * The address and the old value both go to frame slots rather than to
     * registers because `call` clobbers every caller-saved one. */
    gen_addr(cg, arg);
    int a = temp_alloc(cg);
    store_temp(cg, a);
    load_temp(cg, a, "rdi");
    buf_printf(cg->out, "  mov rax, QWORD PTR [rdi]\n");
    int v = temp_alloc(cg);
    store_temp(cg, v);
    buf_printf(cg->out, "  lea rax, [rip + .Lstrempty + 16]\n");
    buf_printf(cg->out, "  mov QWORD PTR [rdi], rax\n");
    load_temp(cg, v, "rdi");
    buf_printf(cg->out, "  call z_str_free\n");
}

static void gen_call(CG *cg, Expr *e) {
    if (is_to_text_call(e)) {
        gen_to_text(cg, e);
        return;
    }
    if (is_hash_of_call(e)) {
        gen_hash_of(cg, e);
        return;
    }
    if (is_drop_value_call(e)) {
        gen_drop_value(cg, e);
        return;
    }
    if (is_holdf_call(e)) {
        /* `hold(500ms)` is a wait of half a second, so it takes a different entry
         * from `hold(5)`: the count of seconds is a double here, and the argument
         * marshalling that follows the ordinary path would leave it in rdi. */
        gen_float(cg, e->args[0]);
        buf_printf(cg->out, "  call z_holdf\n");
        return;
    }
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

    /* One integer argument goes straight from rax into rdi.
     *
     * Every other shape stages its arguments in frame temps, because evaluating
     * one argument can clobber the register an earlier one is already sitting in,
     * and the staged copy is what survives that. With a single argument there is
     * nothing to protect it from: the value is computed last, into rax, and the
     * next instruction moves it to rdi, so the store and the reload are both
     * pure overhead.
     *
     * Everything else about the temporaries is left exactly as it is below,
     * including the reservation above, which happens before the argument is
     * evaluated. That reservation is load-bearing even though this path never
     * writes the slot: an argument that needs frame slots of its own is a struct
     * or union literal, and those slots have to land above the staging area or
     * they land where the statement has pinned a string. Sharing this code path's
     * bookkeeping is what keeps that true, so the saving here is only the two
     * instructions and not the frame slot.
     *
     * A float is excluded because it is computed into the vector accumulator
     * rather than rax, and a struct-returning call because the hidden buffer has
     * already taken rdi. */
    if (e->nargs == 1 && !sret && !is_kind(e->args[0]->type, TK_F64)) {
        gen_expr(cg, e->args[0]);
        buf_printf(cg->out, "  mov %s, rax\n", ARG_REGS[0]);
        buf_printf(cg->out, "  call %s\n", e->is_extern ? e->name : z_sym(cg, e->name));
        cg->temp_top = base;
        return;
    }
    for (int i = 0; i < e->nargs; i++) {
        if (is_kind(e->args[i]->type, TK_F64)) {
            gen_float(cg, e->args[i]);
            store_temp_x(cg, base + i);
        } else {
            gen_expr(cg, e->args[i]);
            store_temp(cg, base + i);
        }
    }
    /* The System V ABI numbers the integer registers and the vector registers
     * independently: the first integer argument goes in rdi whether or not any
     * float has been passed, and the first float goes in xmm0. So each class is
     * counted separately and neither displaces the other. Counting them in one
     * sequence would make `f(1.0, 2)` pass 2 in rsi where the callee looks in
     * rdx.
     *
     * Each class is also capped separately, and anything past its cap goes on the
     * stack rather than being refused. The arguments are already staged in frame
     * temps, so the stack slots can be written now without a nested call moving
     * rsp underneath them. */
    ArgAssign aa;
    args_prologue(cg, e, base, sret ? 1 : 0, &aa);
    if (sret) {
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
    }
    buf_printf(cg->out, "  call %s\n", e->is_extern ? e->name : z_sym(cg, e->name));
    args_epilogue(cg, &aa);
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
     * result buffer and a bound receiver never both claim rdi here. A closure is
     * refused for the same reason, so its environment and a result buffer cannot
     * both want rdi either. */
    int iface = is_kind(e->lhs->type, TK_IFACE);
    int bound = is_kind(e->lhs->type, TK_MPTR);
    /* A closure's cell is { code, env }, the same two words in the same order as
     * a bound method's, so the hidden first argument is the environment for one
     * and the receiver for the other. */
    int closure = is_kind(e->lhs->type, TK_CLOSURE);
    int tf = temp_alloc(cg);
    gen_expr(cg, e->lhs); /* the callable */
    store_temp(cg, tf);
    int base = cg->temp_top;
    cg->temp_top += e->nargs;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    gen_args_stage(cg, e, base);
    /* A bound pointer's receiver, or a closure's environment, occupies rdi, so
     * the declared arguments start one register higher -- and once a hidden
     * argument has taken rdi, the declared arguments continue in the integer
     * sequence, so a float among them is passed as raw bits in a general-purpose
     * register. The vector sequence only applies when nothing is hidden. */
    /* The same assignment the callee's prologue reads, with the hidden argument
     * counted as a leading integer-class one. A float among the declared
     * arguments still arrives in a vector register: the ABI numbers the two
     * sequences independently, so an environment in rdi does not push the first
     * float out of xmm0. */
    ArgAssign aa;
    args_prologue(cg, e, base, (sret || bound || closure || iface) ? 1 : 0, &aa);
    if (sret)
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
    load_temp(cg, tf, "r11");
    if (iface) {
        /* An interface cell is { itab, receiver }. The code address is not in the
         * cell -- the cell holds one itab for the whole interface -- so it comes
         * from the slot this method's name resolved to at parse time. */
        buf_printf(cg->out, "  mov rdi, QWORD PTR [r11 + 8]\n");
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n");
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11 + %d]\n", e->vtable_index * 8);
    } else if (bound) {
        /* r11 is the binding cell: { code, receiver }. */
        buf_printf(cg->out, "  mov rdi, QWORD PTR [r11 + 8]\n");
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n");
    } else if (closure) {
        /* { code, env }: the environment becomes the hidden first argument and
         * the declared arguments were placed from rsi up, which is where the
         * hoisted function expects to find them. */
        buf_printf(cg->out, "  mov rdi, QWORD PTR [r11 + 8]\n");
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n");
    }
    buf_printf(cg->out, "  call r11\n");
    args_epilogue(cg, &aa);
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
static int fits_imm32(long long v) { return v >= -2147483648LL && v <= 2147483647LL; }

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
    gen_args_stage(cg, e, base);
    /* Resolve the target: obj = args[0]; vptr = obj[0]; fn = vtable[vtable_index].
     * Done before the arguments are placed, because r11 is the scratch the
     * placement uses. */
    load_temp(cg, base + 0, "r11");                      /* r11 = receiver object */
    buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n"); /* vptr */
    buf_printf(cg->out, "  mov r11, QWORD PTR [r11 + %d]\n", e->vtable_index * 8);
    /* The resolved code address is in r11, which the argument placement is about
     * to overwrite, so stash *that* -- not the receiver. Reloading the receiver
     * here and saving it would store the object pointer and then call it. */
    int vt = temp_alloc(cg);
    buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], r11\n", temp_off(cg, vt));
    {
        /* A virtual call is reached through a vtable, so the slot to load cannot
         * be known while generating the call. Stash it, place the arguments, then
         * come back for it. */
        ArgAssign aa;
        args_prologue(cg, e, base, sret ? 1 : 0, &aa);
        load_temp(cg, vt, "r11");
        cg->temp_top = vt;
        buf_printf(cg->out, "  call r11\n");
        args_epilogue(cg, &aa);
        if (sret) {
            buf_printf(cg->out, "  lea rax, [rbp - %d]\n", rt_addr);
            cg->temp_top = rt_base + rt_nt;
        } else {
            cg->temp_top = vt;
        }
        return;
    }
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

/* ---- interfaces ----
 *
 * An interface value is a pointer to a { itab, receiver } cell, and the itab is
 * a static array of code pointers, one per method the interface requires, in the
 * interface's declaration order. A call resolves a method name to an index once,
 * at parse time, and loads that slot -- so every implementing type has to agree
 * on the layout, which it does by following the interface's order.
 *
 * A struct's methods are emitted as ordinary `z$` symbols, so its itab points
 * straight at them. A class dispatches through its vtable instead, so its itab
 * points at a small trampoline per slot that loads the receiver's vtable and
 * jumps through it -- which is what keeps a subclass stored in an interface
 * calling the override rather than the implementation the conversion site
 * happened to name. */

/* Records that this pair is needed. Returns 0 if the table is full, which the
 * caller treats as "do not emit": a program needing more distinct interface
 * conversions than this has been tested against should fail to link rather than
 * silently call the wrong thing. */
static int iface_note_itab(CG *cg, StructDef *impl, IfaceDef *idef) {
    for (int i = 0; i < cg->nitabs; i++)
        if (cg->itabs[i].impl == impl && cg->itabs[i].idef == idef)
            return 1;
    if (cg->nitabs >= MAX_ITABS)
        return 0;
    cg->itabs[cg->nitabs].impl = impl;
    cg->itabs[cg->nitabs].idef = idef;
    cg->nitabs++;
    return 1;
}

static void gen_expr_body(CG *cg, Expr *e);
static void gen_stmt_body(CG *cg, Stmt *s);

/* Records a temp slot as holding a live string that must not be handed out again
 * before the current statement ends. Split out of `note_str_temp` so the return
 * path can pin the value it spills across the destructors it runs: those are
 * generated as statements, and every statement resets the temp space, so an
 * unpinned slot would be handed straight back out to them. */
static void pin_temp(CG *cg, int t) {
    if (cg->npinned == cg->pin_cap) {
        int ncap = cg->pin_cap == 0 ? 8 : cg->pin_cap * 2;
        int *nd = realloc(cg->pinned, (size_t)ncap * sizeof(int));
        if (nd == NULL) {
            fprintf(stderr, "z: out of memory\n");
            exit(1);
        }
        cg->pinned = nd;
        cg->pin_cap = ncap;
    }
    cg->pinned[cg->npinned++] = t;
}

/* A fresh string value that nothing has taken over is a temporary, and a
 * temporary has to be released rather than left for the collector that no longer
 * exists.
 *
 * It is spilled into a frame slot first because the release happens at the end of
 * the statement while the value was produced somewhere inside it: `Console.WriteLog(a + b)`
 * has a fresh string in argument position and an int as the statement's own
 * value, so there is no single expression left to attach a release to. The slot
 * is left allocated (`temp_top` is not restored) so nothing reuses it before the
 * statement ends, which is what stops a loop body from overwriting the pointer it
 * is about to free.
 *
 * A literal is excluded: it is static data, not an allocation, so there is nothing
 * to release and a spill would be pure cost. `str_result_owned` is the parser
 * saying a store or a `return` already took this value over, and freeing it here
 * as well is the double free this stage exists to prevent. */
static void note_str_temp(CG *cg, Expr *e) {
    if (cg->no_str_temp)
        return;
    if (e == NULL || e->kind == E_STRING || !is_kind(e->type, TK_STRING))
        return;
    if (expr_is_borrowed_string(e) || e->str_result_owned)
        return;
    /* An assignment's value is the *destination's* value, so its owner is the
     * slot rather than the statement. Releasing it here freed the string the
     * assignment had just stored, which is why a local declared with `var` was
     * fine and `buf = str_buf_new(0)` was not: the first is an S_VAR with no
     * expression node of its own, the second is an E_ASSIGN whose type is the
     * string it just wrote. `move` is the same shape, by definition. */
    if (e->kind == E_ASSIGN || e->kind == E_MOVE)
        return;
    /* A hoisted expression was computed once, before the loop, into a frame slot
     * of its own. Every iteration reads that same value back, so releasing it per
     * iteration frees the same allocation over and over -- and `Console.WriteLog("ab" +
     * "cd")` inside a loop is exactly that, since the concatenation of two
     * literals is loop-invariant and the strength reducer hoists it. The hoisted
     * slot lives as long as the function, which is the right lifetime for a value
     * the loop reads on every pass. */
    if (e->hoisted_slot != 0)
        return;
    int t = temp_alloc(cg);
    store_temp(cg, t);
    pin_temp(cg, t);
}

static void gen_expr(CG *cg, Expr *e) {
    gen_expr_body(cg, e);
    note_str_temp(cg, e);
}

static void gen_expr_body(CG *cg, Expr *e) {
    /* Loop-invariant code motion may have already computed this into a slot
     * ahead of the enclosing loop; read it back instead of recomputing. While
     * emitting that very computation the guard must not fire. */
    if ((e->kind == E_BINARY || e->kind == E_UNARY) && e->hoisted_slot != 0 &&
        !cg->emitting_hoist) {
        buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->hoisted_slot);
        return;
    }
    /* A float-typed expression computes in an XMM register. Dispatching on the
     * *type* rather than on the node kind is what makes this total: a float can
     * arrive as a literal, a local, a field, a conversion, a call or an
     * arithmetic expression, and they all want the same treatment.
     *
     * A whitelist, not "anything that is a float and is not a binary": an
     * assignment is float-typed too, and routing it here would send gen_float's
     * unhandled default back to gen_expr and round forever. Enumerating the
     * kinds that produce a value with no effect of their own means a node kind
     * added later is handled by the integer path (wrong for a float, but loudly
     * wrong in testing) rather than by mutual recursion. */
    if (is_kind(e->type, TK_F64) && is_float_value_expr(e)) {
        gen_float(cg, e);
        return;
    }
    switch (e->kind) {
    case E_INT:
        gen_int_literal(cg, e->ival);
        break;
    case E_F64:
    case E_CVT:
        gen_float(cg, e);
        break;
    case E_BOOL:
        buf_printf(cg->out, "  mov rax, %d\n", e->ival ? 1 : 0);
        break;
    case E_NULL:
        buf_printf(cg->out, "  mov rax, 0\n");
        break;
    case E_STRING:
        buf_printf(cg->out, "  lea rax, [rip + .Lstr%d + 16]\n", e->str_id);
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
        if (e->boxed) {
            /* The slot holds the box pointer; the value is behind it. */
            buf_printf(cg->out, "  mov r11, QWORD PTR [rbp - %d]\n", e->slot);
            if (is_kind(e->type, TK_F64))
                buf_printf(cg->out, "  movsd %s, QWORD PTR [r11]\n", XMM_ACC);
            else if (!is_aggregate(e->type))
                buf_printf(cg->out, "  mov rax, QWORD PTR [r11]\n");
            else
                buf_printf(cg->out, "  mov rax, r11\n");
            break;
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
    case E_MOVE: {
        /* Read the value, then take the source's claim to it away.
         *
         * Poisoning the slot rather than leaving it is what makes a second read
         * a crash instead of a silently correct answer: the parser already
         * rejects the read where it can see it, so this is the backstop for the
         * cases it cannot, such as a move through a closure's box. Null is the
         * poison because every free path here already skips it, so a value that
         * was moved rather than destroyed costs nothing to skip. */
        if (e->lhs != NULL && e->lhs->kind == E_VAR && !e->lhs->agg_param && !e->lhs->boxed &&
            !is_aggregate(e->type)) {
            /* A register-allocated local does not live in its frame slot, so
             * poisoning the slot would leave the register holding the value and
             * the move would read as a move of whatever the slot happens to
             * contain. The register is the one to take the value from *and* the
             * one to clear, which also makes this two instructions. */
            const char *mreg = local_reg(cg, e->lhs->slot);
            if (mreg != NULL) {
                buf_printf(cg->out, "  mov rax, %s\n", mreg);
                buf_printf(cg->out, "  xor %s, %s\n", mreg, mreg);
                break;
            }
            buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->lhs->slot);
            buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], 0\n", e->lhs->slot);
            break;
        }
        gen_expr(cg, e->lhs);
        if (e->lhs != NULL && !is_aggregate(e->type)) {
            int t = temp_alloc(cg);
            if (is_kind(e->type, TK_F64))
                store_temp_x(cg, t);
            else
                store_temp(cg, t);
            gen_addr(cg, e->lhs);
            buf_printf(cg->out, "  mov QWORD PTR [rax], 0\n");
            load_temp(cg, t, "rax");
            cg->temp_top = t;
        }
        break;
    }
    case E_POSTINC: {
        /* The two orders, which is the entire point of the node: read the old
         * value, write the new one, and yield the old. A register-resident local
         * is two instructions. */
        const char *lreg =
            (e->lhs->kind == E_VAR && !e->lhs->agg_param) ? local_reg(cg, e->lhs->slot) : NULL;
        if (lreg != NULL) {
            buf_printf(cg->out, "  mov rax, %s\n", lreg);
            buf_printf(cg->out, "  %s %s, 1\n", e->op == T_PLUS ? "add" : "sub", lreg);
            break;
        }
        int t = temp_alloc(cg);
        gen_addr(cg, e->lhs);
        store_temp(cg, t);
        load_temp(cg, t, "r11");                             /* r11 = the address of the variable */
        buf_printf(cg->out, "  mov rax, QWORD PTR [r11]\n"); /* the old value */
        buf_printf(cg->out, "  %s QWORD PTR [r11], 1\n", e->op == T_PLUS ? "add" : "sub");
        cg->temp_top = t;
        break;
    }
    case E_STRLEN:
        /* The length is the first word of the header, which sits immediately
         * before the bytes the value points at. One load, no call, no scan --
         * this is what `len(s)` was not before the string had a header. */
        gen_expr(cg, e->lhs);
        buf_printf(cg->out, "  mov rax, QWORD PTR [rax - 16]\n");
        break;
    case E_SLICE: {
        /* z_slice(s, a, b) is a call rather than inline code because the
         * clamping -- a negative end counting from the back, either end past the
         * string -- has more cases than are worth spelling out twice. A slice is
         * a copy, so this is one of the two string operations that allocates. */
        int t = temp_alloc(cg);
        gen_expr(cg, e->lhs);
        store_temp(cg, t);
        gen_expr(cg, e->rhs);
        buf_printf(cg->out, "  mov rsi, rax\n");
        gen_expr(cg, e->env);
        buf_printf(cg->out, "  mov rdx, rax\n");
        load_temp(cg, t, "rdi");
        buf_printf(cg->out, "  call z_slice\n");
        cg->temp_top = t;
        break;
    }
    case E_INDEX:
        gen_addr(cg, e);
        if (is_aggregate(e->type)) {
            /* A struct value is represented by its address. */
        } else if (is_kind(e->lhs->type, TK_STRING)) {
            /* A byte, zero-extended, so a value above 127 reads back as
             * 128..255 rather than as a negative int. The eight-byte load the
             * general path does would read three bytes past the string. */
            buf_printf(cg->out, "  movzx eax, BYTE PTR [rax]\n");
        } else {
            load_indirect(cg);
        }
        break;
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
            int faddr = saddr - f->offset;
            if (is_aggregate(f->type)) {
                gen_expr(cg, e->args[fi++]);
                buf_printf(cg->out, "  mov rsi, rax\n  lea rdi, [rbp - %d]\n", faddr);
                emit_memcpy(cg, type_size(f->type));
            } else if (is_kind(f->type, TK_F64)) {
                /* The value arrives in xmm0, not rax. */
                gen_float(cg, e->args[fi++]);
                buf_printf(cg->out, "  movsd QWORD PTR [rbp - %d], %s\n", faddr, XMM_ACC);
            } else {
                gen_expr(cg, e->args[fi++]);
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
    case E_TRY: {
        /* `expr?` on a Result.
         *
         * The operand is an aggregate, so gen_expr leaves its *address* in rax.
         * That address goes into a temp because the early return has to jump away
         * and come back, and the value it left behind has to still be there. The
         * sequence is exactly what an explicit `match` and `return` would emit:
         * read the discriminant, and on Err copy the whole value into the
         * caller's result buffer and leave.
         *
         * Copying all sixteen bytes is sound because every Result has the same
         * layout -- tag at 0, one payload word at 8 -- whatever T and E are. The
         * bytes are an Err carrying an E, and the caller expects a Result whose
         * Err carries that same E, so the value is exactly what was asked for. */
        Type *rt = e->lhs->type;
        if (e->type == NULL || rt == NULL) {
            /* The parser already reported this; emit something harmless so the
             * rest of the function still generates and later errors are real. */
            gen_expr(cg, e->lhs);
            break;
        }
        int tsz = type_size(rt);
        int tnt = (tsz + 7) / 8;
        if (tnt < 1)
            tnt = 1;
        int t = temp_alloc_many(cg, tnt);
        gen_expr(cg, e->lhs);
        store_temp(cg, t);
        int err_tag = rt->udef->variants[1].tag;
        int ok_off = rt->udef->variants[0].fields[0].offset;
        int lcont = next_label(cg);
        load_temp(cg, t, "r11");
        buf_printf(cg->out, "  cmp DWORD PTR [r11], %d\n", err_tag);
        buf_printf(cg->out, "  jne .L%d\n", lcont);
        /* The error path: hand the value back and leave the function.
         *
         * Copy first, then destroy, then jump. The copy is of a temp and the
         * destructors are of locals, so the order is not what makes it sound --
         * what makes it sound is that the value is already in the caller's slot
         * before any destructor runs, so a destructor that itself fails, prints,
         * or re-enters cannot find the result half-written. The jump is last
         * because everything before it runs exactly once, on the one path that
         * takes it. */
        buf_printf(cg->out, "  mov rsi, QWORD PTR [rbp - %d]\n", temp_off(cg, t));
        buf_printf(cg->out, "  mov rdi, QWORD PTR [rbp - %d]\n", cg->cur_ret_slot);
        emit_memcpy(cg, tsz);
        /* The scopes the `?` leaves. Without this the early return skips the
         * teardown the normal `return` gets, and a function that owns a value
         * leaks it every time the error arm is taken. */
        if (e->try_drops != NULL)
            gen_stmt_body(cg, e->try_drops);
        buf_printf(cg->out, "  jmp .Lret_%s\n", cg->cur_sym);
        buf_printf(cg->out, ".L%d:\n", lcont);
        /* The Ok path: the payload word, left in rax as the expression's value. */
        load_temp(cg, t, "rax");
        buf_printf(cg->out, "  mov rax, QWORD PTR [rax + %d]\n", ok_off);
        break;
    }
    case E_IFACE: {
        /* Materialise a concrete value as an interface value: a heap cell holding
         * { itab, receiver }.
         *
         * A class is already a pointer, so it is the receiver as it stands. A
         * struct value has to be copied to the heap first, because the cell
         * outlives the frame the value was in -- the same reason a closure's
         * environment is on the heap. Either way the receiver is a pointer, which
         * is what lets a struct and a class share one calling convention here:
         * Z already passes an aggregate receiver by reference, so a struct
         * method's `this` is a pointer to the value and reads its fields
         * straight through it.
         *
         * The cell is the same two words a bound method uses, so the garbage
         * collector already traces it and the call sequence in gen_icall is
         * shared. */
        StructDef *impl = e->impl;
        IfaceDef *id = e->idef;
        int rtmp = temp_alloc(cg);
        if (impl != NULL && impl->is_class) {
            gen_expr(cg, e->lhs); /* already the object pointer */
        } else {
            /* Two temps, not one: the source address and the new heap block are
             * both live across the allocation, and sharing a slot made the copy
             * read and write the same address -- the receiver ended up pointing
             * into the caller's frame, which the cell then outlived. */
            int src = temp_alloc(cg);
            int blk = temp_alloc(cg);
            gen_expr(cg, e->lhs); /* the address of the value */
            store_temp(cg, src);
            int sz = type_size(e->lhs->type);
            buf_printf(cg->out, "  mov rdi, %d\n  call z_newobj\n", sz);
            store_temp(cg, blk);
            load_temp(cg, src, "rsi");
            load_temp(cg, blk, "rdi");
            emit_memcpy(cg, sz);
            cg->temp_top = src + 1;
        }
        store_temp(cg, rtmp);
        if (!cg->measuring)
            iface_note_itab(cg, impl, id);
        buf_printf(cg->out, "  lea rdi, [rip + %s]\n", iface_itab_symbol(id, impl));
        load_temp(cg, rtmp, "rsi");
        buf_printf(cg->out, "  call z_newiface\n");
        cg->temp_top = rtmp;
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
            int faddr = saddr - f->offset;
            if (is_aggregate(f->type)) {
                gen_expr(cg, e->args[i]);
                buf_printf(cg->out, "  mov rsi, rax\n  lea rdi, [rbp - %d]\n", faddr);
                emit_memcpy(cg, type_size(f->type));
            } else if (is_kind(f->type, TK_F64)) {
                /* The value is in an XMM register, not rax. Storing rax here put
                 * whatever the previous instruction happened to leave there into
                 * the payload, so a variant carrying a double read back as a
                 * denormal built out of an unrelated register. */
                gen_float(cg, e->args[i]);
                buf_printf(cg->out, "  lea r11, [rbp - %d]\n", faddr);
                buf_printf(cg->out, "  movsd QWORD PTR [r11], %s\n", XMM_ACC);
            } else {
                gen_expr(cg, e->args[i]);
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
                Type *ft = arm->variant->fields[b].type;
                cg->temp_top = t + 1;
                load_temp(cg, t, "r11");
                if (is_aggregate(ft)) {
                    /* An aggregate payload lives inline in the union, and the
                     * parser gave the binding a slot that holds a pointer (the
                     * same `agg_param` convention a by-value struct parameter
                     * uses). So the binding is the payload's address, not the
                     * eight bytes at it. */
                    buf_printf(cg->out, "  lea rax, [r11 + %d]\n", off);
                    buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", arm->bind_slots[b]);
                } else {
                    /* A float payload is copied as eight raw bits, which is the
                     * whole double; the binding is read back with movsd. */
                    buf_printf(cg->out, "  mov rax, QWORD PTR [r11 + %d]\n", off);
                    buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", arm->bind_slots[b]);
                }
            }
            /* Both arms are emitted and only one runs, so an arm must not
             * register a temporary: the arm that is skipped leaves its slot
             * holding whatever the last statement put there, and the release at
             * the end of this statement frees that. The match's own value is what
             * survives and the caller records it once.
             *
             * This is the same hole as a ternary's untaken arm, and it was live
             * for the whole of the temporary-release work: `match (r) { Ok(n) =>
             * Console.WriteLog("ok " + itoa(n)), Err(e) => Console.WriteLog("e " + e) }` on a
             * Result whose Ok payload is an int freed an int as a string pointer. A match was the
             * one conditional form not covered. */
            cg->no_str_temp++;
            gen_expr(cg, arm->body);
            cg->no_str_temp--;
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
        /* Both arms are emitted but only one runs. The value that survives is
         * the ternary's own, which the caller records once, so recording inside
         * an arm is not just unnecessary but wrong. */
        cg->no_str_temp++;
        gen_expr(cg, e->rhs);
        buf_printf(cg->out, "  jmp .L%d\n", lend);
        buf_printf(cg->out, ".L%d:\n", lelse);
        gen_expr(cg, e->args[0]);
        buf_printf(cg->out, ".L%d:\n", lend);
        cg->no_str_temp--;
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
        buf_printf(cg->out, "  lea rax, [rip + %s]\n", e->is_extern ? e->name : z_sym(cg, e->name));
        break;
    case E_ICALL:
        gen_icall(cg, e);
        break;

    case E_MPTR:
        gen_mptr(cg, e);
        break;
    case E_BINARY: {
        /* What decides this is the *result* type, not the operand types.
         * `"x = " + 1.5` has a float operand and is string concatenation, so a
         * test on the operands alone sends it into the float path -- which then
         * stores the string pointer's bits into a float slot and adds it to
         * whatever happened to be in xmm0. */
        if (is_kind(e->type, TK_F64)) {
            gen_float(cg, e);
            break;
        }
        /* A float comparison yields a bool, so its operands are the only thing
         * that identifies it. */
        if (is_compare_op(e->op) &&
            (is_kind(e->lhs->type, TK_F64) || is_kind(e->rhs->type, TK_F64))) {
            gen_float_cmp(cg, e);
            break;
        }
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
            /* A float operand is formatted in C and handed back as a string, so
             * the number-to-text rule lives with `Console.WriteLog` rather than being
             * written twice in assembly. z_concat_f takes it in xmm0, which is
             * where a float already is. */
            int lflt = is_kind(e->lhs->type, TK_F64);
            int rflt = is_kind(e->rhs->type, TK_F64);
            if (lflt)
                gen_float(cg, e->lhs);
            else
                gen_expr(cg, e->lhs);
            if (is_kind(e->lhs->type, TK_INT)) {
                buf_printf(cg->out, "  mov rdi, rax\n  call z_itoa\n");
            } else if (is_kind(e->lhs->type, TK_BOOL)) {
                buf_printf(cg->out, "  lea rdi, [rip + .Lfalse_str + 16]\n"
                                    "  lea r11, [rip + .Ltrue_str + 16]\n");
                buf_printf(cg->out, "  cmp rax, 0\n  cmovne rdi, r11\n  mov rax, rdi\n");
            } else if (lflt) {
                buf_printf(cg->out, "  call z_ftoa\n");
            }
            store_temp(cg, t0);
            if (rflt)
                gen_float(cg, e->rhs);
            else
                gen_expr(cg, e->rhs);
            if (is_kind(e->rhs->type, TK_INT)) {
                buf_printf(cg->out, "  mov rdi, rax\n  call z_itoa\n");
            } else if (is_kind(e->rhs->type, TK_BOOL)) {
                buf_printf(cg->out, "  lea rdi, [rip + .Lfalse_str + 16]\n"
                                    "  lea r11, [rip + .Ltrue_str + 16]\n");
                buf_printf(cg->out, "  cmp rax, 0\n  cmovne rdi, r11\n  mov rax, rdi\n");
            } else if (rflt) {
                buf_printf(cg->out, "  call z_ftoa\n");
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
            /* The right operand may not run. See the ternary: recording inside a
             * branch that can be skipped reads as recording whatever the slot
             * held before. */
            cg->no_str_temp++;
            gen_expr(cg, e->rhs);
            cg->no_str_temp--;
            buf_printf(cg->out, "  jmp .L%d\n.L%d:\n  mov rax, 0\n.L%d:\n", lend, lfalse, lend);
        } else if (e->op == T_OR) {
            int lend = next_label(cg);
            gen_expr(cg, e->lhs);
            buf_printf(cg->out, "  cmp rax, 0\n  jne .L%d\n", lend);
            cg->no_str_temp++;
            gen_expr(cg, e->rhs); /* as above: this side may not run */
            cg->no_str_temp--;
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
                 * take the right from wherever it already is.
                 *
                 * A leaf that the register allocator put in a register is named
                 * directly, so `a + b` with both in registers is two instructions
                 * rather than three. Copying it into r11 first is what the
                 * three-address form costs, and it was the last of that in the
                 * hot loop. Naming the operand's own register is safe for every
                 * operator here because the left is already in rax: if the two
                 * happen to be the same register, `add rbx, rbx` and
                 * `sub rbx, rbx` and `imul rbx, rbx` are each exactly right. */
                gen_expr(cg, e->lhs);
                const char *rreg = leaf_live_reg(cg, e->rhs);
                if (rreg != NULL) {
                    emit_binop_reg(cg, e->op, rreg);
                } else {
                    gen_leaf_to_reg(cg, e->rhs, "r11");
                    emit_binop_op(cg, e->op);
                }
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
        /* A string slot owns exactly one value, so a plain assignment releases
         * the one it had. The value is captured first and released after the
         * store, because `s = s + "x"` reads the destination, and because the
         * release is a call and a call clobbers the register the new value is
         * sitting in. */
        int t_old = -1;
        if (e->frees_old) {
            t_old = temp_alloc(cg);
            if (e->lhs != NULL && e->lhs->kind == E_VAR && !e->lhs->agg_param && !e->lhs->boxed) {
                const char *oreg = local_reg(cg, e->lhs->slot);
                if (oreg != NULL)
                    buf_printf(cg->out, "  mov %s, %s\n", "rax", oreg);
                else
                    buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->lhs->slot);
            } else {
                gen_addr(cg, e->lhs);
                buf_printf(cg->out, "  mov rax, QWORD PTR [rax]\n");
            }
            store_temp(cg, t_old);
        }
        /* A captured variable lives in a heap cell, because the closure that
         * reads it may outlive the frame that declared it. Its slot therefore
         * holds a pointer to that cell and every access goes through it. Both
         * sides of the assignment then reach one value, so a counter climbs
         * inside a closure and is seen to have climbed outside it. */
        if (e->lhs != NULL && e->lhs->boxed) {
            int tb = temp_alloc(cg);
            gen_addr(cg, e->lhs); /* rax = the box pointer */
            store_temp(cg, tb);
            int tr = temp_alloc(cg);
            gen_expr(cg, e->rhs);
            if (is_kind(e->type, TK_F64)) {
                store_temp_x(cg, tr);
            } else if (is_aggregate(e->type)) {
                /* A captured struct is a whole cell, not a word, so the copy has
                 * to move every byte. */
                buf_printf(cg->out, "  mov rsi, rax\n");
                load_temp(cg, tb, "rdi");
                emit_memcpy(cg, type_size(e->type));
                cg->temp_top = tb;
                goto assign_done;
            } else {
                store_temp(cg, tr);
            }
            load_temp(cg, tb, "r11");
            if (is_kind(e->type, TK_F64)) {
                load_temp_x(cg, tr, XMM_ACC);
                buf_printf(cg->out, "  movsd QWORD PTR [r11], %s\n", XMM_ACC);
            } else {
                load_temp(cg, tr, "rax");
                buf_printf(cg->out, "  mov QWORD PTR [r11], rax\n");
            }
            cg->temp_top = tb;
            goto assign_done;
        }
        /* A float assignment stores with movsd from xmm0, not mov from rax, and
         * the in-place forms above all reason in terms of rax -- so floats take
         * their own path before any of that. A float local is never in a
         * general-purpose register (see LocalInfo.is_float), so there is no
         * in-place form to miss. */
        if (is_kind(e->type, TK_F64)) {
            int ta = temp_alloc(cg);
            gen_addr(cg, e->lhs);
            store_temp(cg, ta);
            if (e->compound) {
                static const char *const fops[] = {"addsd", "subsd", "mulsd", "divsd"};
                static const TokenKind fkinds[] = {T_PLUS, T_MINUS, T_STAR, T_SLASH};
                const char *ins = NULL;
                for (size_t i = 0; i < sizeof fops / sizeof(*fops); i++)
                    if (e->op == fkinds[i])
                        ins = fops[i];
                int tb = temp_alloc(cg);
                gen_float(cg, e->rhs);
                store_temp_x(cg, tb);
                load_temp(cg, ta, "r11");            /* r11 = destination address */
                load_temp_x(cg, tb, XMM_SCRATCH[1]); /* xmm1 = the right operand */
                buf_printf(cg->out, "  movsd %s, QWORD PTR [r11]\n", XMM_ACC);
                if (ins != NULL)
                    buf_printf(cg->out, "  %s %s, %s\n", ins, XMM_ACC, XMM_SCRATCH[1]);
                buf_printf(cg->out, "  movsd QWORD PTR [r11], %s\n", XMM_ACC);
                cg->temp_top = ta;
            } else {
                gen_float(cg, e->rhs);
                load_temp(cg, ta, "r11");
                buf_printf(cg->out, "  movsd QWORD PTR [r11], %s\n", XMM_ACC);
            }
            cg->temp_top = ta;
            goto assign_done;
        }
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
            goto assign_done;
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
    assign_done:
        /* The old value, released after the store so that the right-hand
         * side of `s = s + "x"` could read the slot it replaces. */
        if (t_old >= 0) {
            load_temp(cg, t_old, "rdi");
            buf_printf(cg->out, "  call z_str_free\n");
        }
        break;
    }
    case E_CLOSURE: {
        /* A { code, env } cell, the same shape a bound method uses.
         *
         * The environment is a heap array of the captured variables' box
         * pointers, gathered here from the enclosing frame -- each argument is
         * already an E_VAR for a captured slot, and reading it yields the box.
         * The code address goes in as the symbol, so it needs no relocation and
         * the cell is two words. */
        int n = e->nargs;
        int base = cg->temp_top;
        cg->temp_top += n > 0 ? n : 1;
        if (cg->temp_top > cg->temp_high)
            cg->temp_high = cg->temp_top;
        for (int i = 0; i < n; i++) {
            /* gen_addr, not gen_expr: the environment holds each captured
             * variable's *box*, so that the closure and the enclosing function
             * reach the same cell and an assignment through one is seen by the
             * other. Reading the slot's contents instead would capture a snapshot
             * and a counter would stop counting the moment the closure was made. */
            gen_addr(cg, e->args[i]);
            store_temp(cg, base + i);
        }
        /* A closure that captured nothing still gets a real (empty) cell, so
         * the call path has one shape rather than two. */
        buf_printf(cg->out, "  mov rdi, %d\n  mov rsi, 8\n  call z_newarray\n", n);
        buf_printf(cg->out, "  mov r11, rax\n");
        for (int i = 0; i < n; i++) {
            load_temp(cg, base + i, "rax");
            buf_printf(cg->out, "  mov QWORD PTR [r11 + %d], rax\n", 8 * i);
        }
        buf_printf(cg->out, "  mov rsi, r11\n");
        buf_printf(cg->out, "  lea rdi, [rip + %s]\n", z_sym(cg, e->name));
        buf_printf(cg->out, "  call z_newbinding\n");
        cg->temp_top = base;
        break;
    }
    case E_CALL:
        if (strcmp(e->name, "print") == 0) {
            Type *at = e->args[0]->type;
            if (is_kind(at, TK_F64)) {
                /* The value is already in xmm0, which is where the ABI puts a
                 * floating-point argument. The formatting happens in C, because
                 * a variadic call with a float is the caller's job to set up and
                 * getting it subtly wrong is a stack-corrupting bug rather than
                 * a wrong digit. */
                gen_float(cg, e->args[0]);
                buf_printf(cg->out, "  call z_print_f\n");
                break;
            }
            gen_expr(cg, e->args[0]);
            if (is_kind(at, TK_STRING)) {
                /* z_print_str, not puts: puts stops at the first zero byte, so a
                 * string containing one would print only its first line. Now that
                 * the length is in the header it is the length that is printed. */
                buf_printf(cg->out, "  mov rdi, rax\n  call z_print_str\n");
            } else if (is_kind(at, TK_BOOL)) {
                int lfalse = next_label(cg);
                int lend = next_label(cg);
                buf_printf(cg->out,
                           "  cmp rax, 0\n  je .L%d\n"
                           "  lea rdi, [rip + .Ltrue_str + 16]\n  jmp .L%d\n"
                           ".L%d:\n  lea rdi, [rip + .Lfalse_str + 16]\n"
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
            /* Call ctor: this=obj in rdi, then the ctor args in rsi.. The
             * receiver is a hidden first argument, so the declared arguments
             * follow in the integer sequence, which is where the constructor's
             * prologue looks for them. */
            int n = e->nargs;
            int base = cg->temp_top;
            cg->temp_top += n;
            if (cg->temp_top > cg->temp_high)
                cg->temp_high = cg->temp_top;
            gen_args_stage(cg, e, base);
            /* `this` in rdi, then the declared arguments from rsi and the vector
             * registers, or the stack. rdi is written after the arguments are
             * placed, because placing them uses rdi. */
            ArgAssign aa;
            args_prologue(cg, e, base, 1, &aa);
            load_temp(cg, ot, "rdi");
            buf_printf(cg->out, "  call %s\n", e->is_extern ? e->name : z_sym(cg, e->name));
            args_epilogue(cg, &aa);
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
    case E_CLOSURE:
        /* Building a closure allocates, so it is not a pure read of a value even
         * though its operands are. */
        return 0;
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
    /* A float comparison cannot take the branch-on-flags path below: that one
     * emits an integer `cmp` against a value in rax, and a float is in an XMM
     * register. The guard is here, at the single entry point, rather than spread
     * through the arms, so there is no way to reach them with a float by
     * accident. */
    if (cond != NULL && cond->kind == E_BINARY && is_compare_op(cond->op) &&
        (is_kind(cond->lhs->type, TK_F64) || is_kind(cond->rhs->type, TK_F64))) {
        gen_float_cmp(cg, cond);
        buf_printf(cg->out, "  cmp rax, 0\n  je .L%d\n", false_label);
        return;
    }
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
                        const char *rreg = leaf_live_reg(cg, e->rhs);
                        if (rreg != NULL) {
                            buf_printf(cg->out, "  %s %s, %s\n", ins, lreg, rreg);
                        } else {
                            gen_leaf_to_reg(cg, e->rhs, "r11");
                            buf_printf(cg->out, "  %s %s, r11\n", ins, lreg);
                        }
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
        return licm_may_write(e->lhs) || licm_may_write(e->rhs);
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
    for (int i = 0; i < e->nargs; i++)
        licm_note_expr(e->args[i], written);
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
    for (int i = 0; i < e->nargs; i++)
        below += licm_walk_expr(cg, e->args[i], written, L);
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

/* Runs the pass over one loop. `step` is the loop's own step expression, or NULL
 * for a `while`: it runs once per iteration, so a local it writes is not
 * invariant, and without it the loop's induction variable looks like a constant
 * and expressions over it get hoisted out of the loop they vary in.
 *
 * `emit` is 0 during the measure pass, which still marks nodes and reserves
 * their slots so the frame accounts for them. */
static void licm_hoist(CG *cg, Stmt *body, Expr *step, int emit) {
    if (body == NULL)
        return;
    unsigned char written[LICM_MAX_SLOT];
    memset(written, 0, sizeof written);
    /* The step first: it is the one write the body walk cannot see, and getting
     * it wrong is a miscompile rather than a missed optimization. */
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
                buf_printf(cg->out, "  jmp .L%d\n", lbl);
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
            licm_hoist(cg, s->body, NULL, !cg->measuring);
        gen_loop(cg, s->body, s->cond, NULL, unroll_factor(cg, s->body, s->cond));
        break;
    }
    case S_FOR: {
        if (cg->opt >= 2)
            licm_hoist(cg, s->body, s->for_step, !cg->measuring);
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
    case S_BLOCK:
        gen_block_items(cg, s);
        /* A block that wraps an inlined body carries the label its `return`s
         * jump to, emitted after the last item so reaching the end of the body
         * and returning from it are the same thing. */
        if (s->inl_label > 0)
            buf_printf(cg->out, ".L%d:\n", s->inl_label);
        break;
    case S_FUNC:
    case S_STRUCT:
    case S_UNION:
        break;
    }
}

static int align16(int n) { return (n + 15) & ~15; }

/* ---- DWARF debug information ----
 *
 * Enough of DWARF 4 for a debugger to set a breakpoint, step, and read the
 * values in a frame.
 *
 * Everything is written as assembly text rather than assembled into a byte
 * buffer, and that is not a style choice. A DWARF section contains addresses,
 * and codegen does not know addresses: it writes assembly, and the linker is
 * what places things. A buffer of already-encoded bytes cannot express "the
 * address of this label", because by the time the bytes exist the label is gone.
 * Emitting `.quad .Ldbg_b0` inline leaves the assembler and linker to resolve
 * it into a relocation, which is the only way the value can be both unknown at
 * compile time and correct in the output.
 *
 *   .debug_line   Not encoded here at all. The assembler builds it from the
 *                 .file and .loc directives emitted next to the code, so the
 *                 address-to-line mapping is produced from the same text that
 *                 produced the addresses and cannot drift from it.
 *   .debug_abbrev A fixed table, written once at the end.
 *   .debug_info   A compile unit, the base types, and one subprogram per
 *                 function with its parameters and locals.
 *
 * Two honest limitations:
 *
 *   - Locals are described only while they live in memory. The register
 *     allocator promotes some to callee-saved registers, and a variable that
 *     moves between a register and the stack over its lifetime needs a location
 *     list to describe accurately. Those variables are left out rather than
 *     given a location that is right only part of the time, so a debugger
 *     reports them as optimized out. Parameters are always in memory, so those
 *     are exact.
 *   - Z has three scalar types, so a local is described as a long, a boolean or
 *     a char pointer, and anything else gets no type at all. A debugger shows
 *     that as an untyped value, which is better than a confident wrong type.
 */
#define DW_TAG_compile_unit 0x11
#define DW_TAG_pointer_type 0x0f
#define DW_TAG_formal_parameter 0x05
#define DW_TAG_base_type 0x24
#define DW_TAG_subprogram 0x2e
#define DW_TAG_variable 0x34

#define DW_CHILDREN_no 0
#define DW_CHILDREN_yes 1

#define DW_AT_location 0x02
#define DW_AT_name 0x03
#define DW_AT_byte_size 0x0b
#define DW_AT_low_pc 0x11
#define DW_AT_high_pc 0x12
#define DW_AT_language 0x13
#define DW_AT_stmt_list 0x10
#define DW_AT_producer 0x25
#define DW_AT_decl_file 0x3a
#define DW_AT_decl_line 0x3b
#define DW_AT_encoding 0x3e
#define DW_AT_external 0x3f
#define DW_AT_frame_base 0x40
#define DW_AT_type 0x49

#define DW_FORM_addr 0x01
#define DW_FORM_data2 0x05
#define DW_FORM_data1 0x0b
#define DW_FORM_string 0x08
#define DW_FORM_ref4 0x13
#define DW_FORM_exprloc 0x18
#define DW_FORM_flag_present 0x19
/* A section-relative offset, which is what DW_AT_stmt_list takes. Using
 * DW_FORM_addr here instead asks for a relocated address, and a non-alloc
 * section like .debug_line has no address: the value comes back as 0 and every
 * reader quietly ignores the line program. */
#define DW_FORM_sec_offset 0x17

/* DW_OP_breg0 through DW_OP_breg31 are 0x70 through 0x8f, so breg6 -- rbp on
 * x86-64 -- is 0x76. Off by one here is 0x77, which is breg7: rsp. A location
 * read through rsp still assembles, still passes every "is the section
 * present" check, and reports a plausible-looking wrong value, which is the
 * worst way for this to fail. */
#define DW_OP_breg6 0x76
/* DW_OP_reg0 .. DW_OP_reg31, one per machine register. */
#define DW_OP_reg0 0x50
#define DW_OP_call_frame_cfa 0x9c
/* DW_OP_fbreg, kept for reference: it is relative to the frame base, which a
 * reader can only resolve with unwind information. */
#define DW_OP_fbreg 0x91

#define DW_ATE_boolean 0x02
#define DW_ATE_signed 0x05
#define DW_ATE_signed_char 0x06
#define DW_ATE_float 0x04

/* DWARF 4, 64-bit DWARF format. The format is 32-bit: the unit length is a
 * 4-byte field, which is what lets a reference to a label stand in for it. */
#define DWARF_VERSION 4

/* Abbreviation codes, fixed so the DIE writer can name them. */
#define DW_ABBREV_CU 1
#define DW_ABBREV_SUBPROGRAM 2
#define DW_ABBREV_PARAM_TYPED 3
#define DW_ABBREV_PARAM 4
#define DW_ABBREV_VAR_TYPED 5
#define DW_ABBREV_VAR 6
#define DW_ABBREV_BASE 7
#define DW_ABBREV_POINTER 8

/* Emits one byte as a `.byte` directive. Abbreviation codes, tags and attribute
 * pairs are all ULEB128, and every value used here fits in a single byte, so
 * the encoding and the directive coincide. */
static void dbg8(Buf *b, unsigned v) { buf_printf(b, "  .byte %u\n", v & 0xffu); }

static void dbg16(Buf *b, unsigned v) { buf_printf(b, "  .short %u\n", v & 0xffffu); }

static void dbg32(Buf *b, unsigned long v) { buf_printf(b, "  .long %lu\n", v & 0xffffffffUL); }

/* A NUL-terminated string in DW_FORM_string form. */
static void dbg_asciz(Buf *b, const char *s) { buf_printf(b, "  .asciz \"%s\"\n", s); }

/* SLEB128, the signed base-128 encoding DWARF uses for frame offsets. An
 * exprloc's length prefix has to be written before the value it measures, so
 * the length and the bytes are produced separately. */
static int sleb128_len(long v) {
    int n = 0;
    for (;;) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7; /* arithmetic: the sign lives in bit 6 of every byte */
        n++;
        if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40) != 0))
            return n;
    }
}

static void sleb128_put(Buf *b, long v) {
    for (;;) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40) != 0)) {
            buf_printf(b, "  .byte %u\n", (unsigned)byte);
            return;
        }
        buf_printf(b, "  .byte %u\n", (unsigned)(byte | 0x80));
    }
}

/* The abbreviation table. Fixed content, written once. */
static void dbg_write_abbrev(Buf *b) {
    /* One abbreviation: code, tag, has-children, then (attribute, form) pairs
     * closed by a zero pair. */
#define ABBREV(code_, tag_, kids_)                                                                 \
    dbg8(b, code_);                                                                                \
    dbg8(b, tag_);                                                                                 \
    dbg8(b, kids_)

#define ATTR(a_, f_)                                                                               \
    dbg8(b, a_);                                                                                   \
    dbg8(b, f_)

#define ATTR_END()                                                                                 \
    dbg8(b, 0);                                                                                    \
    dbg8(b, 0)

    /* The line program the assembler generated, named so DW_AT_stmt_list can
     * point at it. It is emitted before this function runs, and the assembler
     * appends its own program to the section, so this label is the section
     * start. */
    buf_puts(b, "  .section .debug_line\n.Lz_line:\n");

    buf_puts(b, "  .section .debug_abbrev\n.Lz_abbrev:\n");

    ABBREV(DW_ABBREV_CU, DW_TAG_compile_unit, DW_CHILDREN_yes);
    ATTR(DW_AT_producer, DW_FORM_string);
    ATTR(DW_AT_language, DW_FORM_data2);
    ATTR(DW_AT_stmt_list, DW_FORM_sec_offset);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR_END();

    /* external is flag_present, so it costs no bytes in the DIE itself. The
     * frame base is the canonical frame address rather than a register, because
     * codegen rebases rbp below the saved callee-saved registers and so rbp is
     * not a stable reference across a function. */
    /* The return type sits after the name so a reader that only wants to know
     * what a function returns finds it before the addresses. */
    ABBREV(DW_ABBREV_SUBPROGRAM, DW_TAG_subprogram, DW_CHILDREN_yes);
    ATTR(DW_AT_external, DW_FORM_flag_present);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR(DW_AT_type, DW_FORM_ref4);
    ATTR(DW_AT_decl_file, DW_FORM_data1);
    ATTR(DW_AT_decl_line, DW_FORM_data1);
    ATTR(DW_AT_low_pc, DW_FORM_addr);
    ATTR(DW_AT_high_pc, DW_FORM_addr);
    ATTR(DW_AT_frame_base, DW_FORM_exprloc);
    ATTR_END();

    /* Parameters and locals come in typed and untyped pairs. Naming a type Z
     * does not have would be a lie a debugger would faithfully print. */
    ABBREV(DW_ABBREV_PARAM_TYPED, DW_TAG_formal_parameter, DW_CHILDREN_no);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR(DW_AT_decl_file, DW_FORM_data1);
    ATTR(DW_AT_decl_line, DW_FORM_data1);
    ATTR(DW_AT_type, DW_FORM_ref4);
    ATTR(DW_AT_location, DW_FORM_exprloc);
    ATTR_END();

    ABBREV(DW_ABBREV_PARAM, DW_TAG_formal_parameter, DW_CHILDREN_no);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR(DW_AT_decl_file, DW_FORM_data1);
    ATTR(DW_AT_decl_line, DW_FORM_data1);
    ATTR(DW_AT_location, DW_FORM_exprloc);
    ATTR_END();

    ABBREV(DW_ABBREV_VAR_TYPED, DW_TAG_variable, DW_CHILDREN_no);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR(DW_AT_decl_file, DW_FORM_data1);
    ATTR(DW_AT_decl_line, DW_FORM_data1);
    ATTR(DW_AT_type, DW_FORM_ref4);
    ATTR(DW_AT_location, DW_FORM_exprloc);
    ATTR_END();

    ABBREV(DW_ABBREV_VAR, DW_TAG_variable, DW_CHILDREN_no);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR(DW_AT_decl_file, DW_FORM_data1);
    ATTR(DW_AT_decl_line, DW_FORM_data1);
    ATTR(DW_AT_location, DW_FORM_exprloc);
    ATTR_END();

    ABBREV(DW_ABBREV_BASE, DW_TAG_base_type, DW_CHILDREN_no);
    ATTR(DW_AT_name, DW_FORM_string);
    ATTR(DW_AT_byte_size, DW_FORM_data1);
    ATTR(DW_AT_encoding, DW_FORM_data1);
    ATTR_END();

    ABBREV(DW_ABBREV_POINTER, DW_TAG_pointer_type, DW_CHILDREN_no);
    ATTR(DW_AT_byte_size, DW_FORM_data1);
    ATTR(DW_AT_type, DW_FORM_ref4);
    ATTR_END();

    dbg8(b, 0); /* the table's own terminator */

#undef ABBREV
#undef ATTR
#undef ATTR_END
}

/* The 1-based DWARF file number for `path`, adding a .file entry the first time
 * the file is seen. A compilation unit spans several files once `import` is
 * used, and every line marker names one of them by number.
 *
 * The entries are emitted as they are discovered, which means the first one
 * lands in the middle of the code. That is fine: a .file directive is a
 * declaration, not code, wherever it appears. */
static int dbg_file(CG *cg, const char *path) {
    if (path == NULL)
        return 1;
    for (int i = 0; i < cg->dbg_file_count; i++) {
        if (strcmp(cg->dbg_file_names[i], path) == 0)
            return i + 1;
    }
    if (cg->dbg_file_count >= MAX_DBG_FILES)
        return 1;
    cg->dbg_file_names[cg->dbg_file_count] = path;
    cg->dbg_file_count++;
    buf_printf(cg->out, "  .file %d \"%s\"\n", cg->dbg_file_count, path);
    return cg->dbg_file_count;
}

/* Emits a line marker so the line table maps the code about to be generated
 * back to the statement the user wrote. */
static void dbg_loc(CG *cg, Span span) {
    if (!cg->debug_info || cg->measuring || span.file == NULL || span.line <= 0)
        return;
    int f = dbg_file(cg, span.file);
    buf_printf(cg->out, "  .loc %d %d %d\n", f, span.line, span.col > 0 ? span.col : 0);
}

/* The base type a Z type maps onto, as a code, or 0 when there is no honest
 * mapping: `int` is a 64-bit signed value, `bool` is an int holding 0 or 1, and
 * a string is a NUL-terminated char pointer.
 *
 * A code rather than an offset, because the base type DIEs are written after
 * every function has been recorded, and their offsets are only known once the
 * compile unit's layout is fixed. The code is translated to an offset at the
 * end, in one place. */
/* The .debug_info offset of the base type a dbg_type_of code stands for, or 0
 * for a type Z has no DWARF spelling for. One function so the subprogram's
 * return type and a variable's type cannot disagree about which offset a code
 * means. */
static int dbg_type_ref(int code, int off_long, int off_bool, int off_str, int off_ptr,
                        int off_double, int off_void) {
    switch (code) {
    case 1:
        return off_long;
    case 2:
        return off_bool;
    case 3:
        return off_str;
    case 4:
        return off_ptr;
    case 5:
        return off_double;
    case 6:
        return off_void;
    default:
        return 0;
    }
}

static int dbg_type_of(Type *t) {
    if (t == NULL)
        return 0;
    if (is_kind(t, TK_INT))
        return 1;
    if (is_kind(t, TK_BOOL))
        return 2;
    if (is_kind(t, TK_STRING))
        return 3;
    if (is_kind(t, TK_F64))
        return 5;
    if (is_kind(t, TK_PTR))
        return 4;
    if (is_kind(t, TK_VOID))
        return 6;
    return 0;
}

/* Records a described variable for a function, ignoring the ones that cannot be
 * described honestly or do not fit. */
static void dbg_add_var(CG *cg, DbgFunc *f, const char *name, Span span, int slot, int reg,
                        int type_code) {
    if (f->nvars >= DBUG_MAX_VARS)
        return;
    if (name == NULL || name[0] == '\0')
        return;
    /* A name the compiler generated, not one the user wrote. */
    if (name[0] == '$')
        return;
    DbgVar *v = &f->vars[f->nvars++];
    v->name = name;
    v->file = dbg_file(cg, span.file);
    v->line = span.line > 0 ? span.line : 0;
    v->slot = slot;
    v->reg = reg;
    v->type_code = type_code;
}

/* Collects the locals of a function body, in source order.
 *
 * A local is skipped when the register allocator gave it a register: its value
 * is not in the frame at all, and there is no single address that describes it
 * for the whole of its life. */
static void dbg_collect_locals(CG *cg, DbgFunc *f, Stmt *s) {
    if (s == NULL)
        return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            dbg_collect_locals(cg, f, s->items[i]);
        return;
    case S_FOR:
        dbg_collect_locals(cg, f, s->for_init);
        dbg_collect_locals(cg, f, s->body);
        return;
    case S_IF:
    case S_WHILE:
        dbg_collect_locals(cg, f, s->body);
        dbg_collect_locals(cg, f, s->orelse);
        return;
    case S_VAR: {
        if (s->name == NULL || s->slot <= 0)
            return;
        /* A promoted local is described at its register rather than skipped. The
         * register holds it for the whole of the declaration, so the one location
         * is exact rather than approximate, and skipping it loses the variable
         * from the debug info entirely -- which is what used to happen above -O0. */
        LocalInfo *li = li_lookup(cg, s->slot);
        int reg = -1;
        if (li != NULL && li->assigned >= 0 && !li->is_float)
            reg = POOL_DWARF_REGNUM[li->assigned];
        dbg_add_var(cg, f, s->name, s->span, s->slot, reg, dbg_type_of(s->type));
        return;
    }
    default:
        return;
    }
}

/* Writes the .debug_info section: the compile unit, the three base types Z can
 * describe, and one subprogram per emitted function.
 *
 * The base types come first and have a known shape, so their offsets within the
 * section can be computed ahead of writing the variables that refer to them.
 * ref4 offsets are measured from the first byte of .debug_info, which is the
 * unit length field. */
static void dbg_write_info(CG *cg) {
    Buf *b = cg->out;
    if (cg->dbg_nfuncs == 0)
        return;

    /* A file to name the unit after: the first one any function came from. */
    const char *primary = cg->dbg_file_count > 0 ? cg->dbg_file_names[0] : "z";

    /* Offsets of the base types, worked out from the layout below.
     *
     *   0x00  unit length                     4
     *   0x04  version                         2
     *   0x06  abbrev table offset             4   (a reloc, but fixed width)
     *   0x0a  address size                    1
     *   0x0b  CU DIE: abbrev code             1
     *   0x0c  producer string                 len("z")+1
     *   ...   language                        2
     *   ...   stmt_list (a 4-byte section offset)
     *   ...   name string                     len(primary)+1
     */
    int off = 4 + 2 + 4 + 1 + 1 + (int)strlen("z") + 1 + 2 + 4;
    off += (int)strlen(primary) + 1;

    /* long: abbrev, "long\0", size, encoding */
    int off_long = off;
    off += 1 + 5 + 1 + 1;
    /* int (Z's bool): abbrev, "int\0", size, encoding */
    int off_bool = off;
    off += 1 + 4 + 1 + 1;
    /* char, the pointee of a string: abbrev, "char\0", size, encoding */
    int off_char = off;
    off += 1 + 5 + 1 + 1;
    /* char *: abbrev, size, ref4 to char */
    int off_str = off;
    off += 1 + 1 + 4;
    /* double: Z's `float` is binary64, which is the name DWARF uses for it. */
    int off_double = off;
    off += 1 + 7 + 1 + 1;
    /* void, the pointee of any other pointer. Without a target type a reader
     * treats a pointer as something to dereference and reports an error instead
     * of showing the address; void* at least shows the address. */
    int off_void = off;
    off += 1 + 5 + 1 + 1;
    /* void *: abbrev, size, ref4 to void */
    int off_ptr = off;
    off += 1 + 1 + 4;

    buf_puts(b, "  .section .debug_info\n.Lz_info:\n");
    /* The unit length covers every byte after itself -- the version, the abbrev
     * offset, the address size and all the DIEs -- so it is measured from a
     * label placed just past the length field, not from the start of the
     * section. Expressed as a label difference because the size is not known
     * until the last DIE is written. */
    buf_puts(b, "  .long .Lz_info_end - .Lz_info_after_len\n");
    buf_puts(b, ".Lz_info_after_len:\n");
    dbg16(b, DWARF_VERSION);
    /* 32-bit DWARF format throughout: a 4-byte unit length and a 4-byte abbrev
     * offset. Mixing a 32-bit length with a 64-bit abbrev offset produces a
     * unit every reader rejects. */
    buf_puts(b, "  .long .Lz_abbrev\n");
    dbg8(b, 8); /* address size */
    buf_puts(b, ".Lz_info_body:\n");

    /* The compile unit. */
    dbg8(b, DW_ABBREV_CU);
    dbg_asciz(b, "z");
    dbg16(b, 0x1d);                    /* DW_LANG_C99: the closest thing DWARF has to "Z" */
    buf_puts(b, "  .long .Lz_line\n"); /* the assembler's own line program */
    dbg_asciz(b, primary);

    /* Base types. */
    dbg8(b, DW_ABBREV_BASE);
    dbg_asciz(b, "long");
    dbg8(b, 8);
    dbg8(b, DW_ATE_signed);

    dbg8(b, DW_ABBREV_BASE);
    dbg_asciz(b, "int");
    dbg8(b, 8);
    dbg8(b, DW_ATE_boolean);

    dbg8(b, DW_ABBREV_BASE);
    dbg_asciz(b, "char");
    dbg8(b, 1);
    dbg8(b, DW_ATE_signed_char);

    dbg8(b, DW_ABBREV_POINTER);
    dbg8(b, 8);
    dbg32(b, (unsigned long)off_char);

    dbg8(b, DW_ABBREV_BASE);
    dbg_asciz(b, "double");
    dbg8(b, 8);
    dbg8(b, DW_ATE_float);

    dbg8(b, DW_ABBREV_BASE);
    dbg_asciz(b, "void");
    dbg8(b, 0); /* no size: an incomplete type */
    dbg8(b, DW_ATE_signed);

    dbg8(b, DW_ABBREV_POINTER);
    dbg8(b, 8);
    dbg32(b, (unsigned long)off_void);

    for (int i = 0; i < cg->dbg_nfuncs; i++) {
        DbgFunc *f = &cg->dbg_funcs[i];
        dbg8(b, DW_ABBREV_SUBPROGRAM);
        dbg_asciz(b, f->name);
        dbg32(b, (unsigned long)dbg_type_ref(f->ret_type, off_long, off_bool, off_str, off_ptr,
                                             off_double, off_void));
        dbg8(b, (unsigned)f->file);
        dbg8(b, (unsigned)f->line);
        /* The two addresses. Each is a reference to a label placed around the
         * function body, which the linker resolves; the extent is measured to a
         * label after the final ret rather than to a byte count, because the
         * size of the emitted code is not known here either. */
        buf_printf(b, "  .quad %s\n", f->begin_label);
        buf_printf(b, "  .quad %s\n", f->end_label);
        /* frame base = rbp + 0, the same register the locations below are
         * relative to, so the two agree. */
        dbg8(b, 2);
        dbg8(b, DW_OP_breg6);
        dbg8(b, 0);

        for (int j = 0; j < f->nvars; j++) {
            DbgVar *v = &f->vars[j];
            int toff = dbg_type_ref(v->type_code, off_long, off_bool, off_str, off_ptr, off_double,
                                    off_void);
            int typed = toff != 0;
            /* A formal_parameter and a variable have separate abbreviations for
             * both the typed and untyped shapes; which pair applies is fixed by
             * the function that recorded the variable. */
            int is_param = j < f->nparams;
            unsigned code;
            if (is_param)
                code = typed ? DW_ABBREV_PARAM_TYPED : DW_ABBREV_PARAM;
            else
                code = typed ? DW_ABBREV_VAR_TYPED : DW_ABBREV_VAR;
            dbg8(b, code);
            dbg_asciz(b, v->name);
            dbg8(b, (unsigned)v->file);
            dbg8(b, (unsigned)v->line);
            if (typed)
                dbg32(b, (unsigned long)toff);
            /* DW_OP_breg6 <sleb>: the value lives at rbp-slot, and rbp holds
             * exactly the address the prologue built the frame against, so the
             * offset is just -slot.
             *
             * The obvious alternative, DW_OP_fbreg, is relative to the canonical
             * frame address -- and a reader cannot work out where that is
             * without unwind information, which codegen does not emit. With only
             * a line table, a reader falls back to assuming the frame pointer is
             * unchanged since the call, which is true at the call site and false
             * everywhere in the body, so every variable read back as whatever
             * happened to be on the stack. Addressing rbp directly needs no
             * unwind information at all. */
            if (v->reg >= 0) {
                /* DW_OP_reg0+n is a single byte and takes no operand, which is
                 * the whole of the location: the value is in that register. */
                dbg8(b, 1);
                dbg8(b, DW_OP_reg0 + (unsigned)v->reg);
            } else {
                long o = -(long)v->slot;
                dbg8(b, (unsigned)(1 + sleb128_len(o)));
                dbg8(b, DW_OP_breg6);
                sleb128_put(b, o);
            }
        }
        dbg8(b, 0); /* end of the subprogram's children */
    }

    dbg8(b, 0); /* end of the compile unit's children */
    buf_puts(b, ".Lz_info_end:\n");
    buf_puts(b, "  .section .text\n");
}

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
    const char *sym = fn->is_entry ? "z_main" : (fn->is_export ? fn->fname : z_sym(cg, fn->fname));
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
    cg->measuring = 1;
    gen_block_items(cg, fn->fbody); /* measure pass */

    cg->measuring = 0;
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

    /* The two labels that bracket the body, for DWARF's low_pc and high_pc. A
     * counter keeps them unique: a generic instantiated twice emits the same
     * function twice, and one repeated label would not assemble. */
    int have_dbg = cg->debug_info;
    DbgFunc *df = NULL;
    char dbg_begin[32], dbg_end[32];
    if (have_dbg) {
        snprintf(dbg_begin, sizeof dbg_begin, ".Ldbg_b%d", cg->dbg_nfuncs);
        snprintf(dbg_end, sizeof dbg_end, ".Ldbg_e%d", cg->dbg_nfuncs);
        if (cg->dbg_nfuncs == cg->dbg_funcs_cap) {
            int ncap = cg->dbg_funcs_cap == 0 ? 32 : cg->dbg_funcs_cap * 2;
            DbgFunc *nf = realloc(cg->dbg_funcs, (size_t)ncap * sizeof(DbgFunc));
            if (nf == NULL)
                die_oom();
            cg->dbg_funcs = nf;
            cg->dbg_funcs_cap = ncap;
        }
        df = &cg->dbg_funcs[cg->dbg_nfuncs++];
        memset(df, 0, sizeof *df);
        /* The name the reader wrote, not the emitted symbol: a nested function's
         * symbol is `$fn<owner>_<name>` and a hoisted lambda's is `$lam<n>`,
         * neither of which appears anywhere in the source. */
        df->name = fn->src_fname != NULL ? fn->src_fname : fn->fname != NULL ? fn->fname : sym;
        df->ret_type = dbg_type_of(fn->ret_type);
        df->file = dbg_file(cg, fn->span.file);
        df->line = fn->span.line > 0 ? fn->span.line : 0;
        df->begin_label = arena_strdup(cg->arena, dbg_begin);
        df->end_label = arena_strdup(cg->arena, dbg_end);

        /* Parameters first, so the writer can tell them from locals. vis_start
         * skips `this` and the hidden sret buffer, neither of which the user
         * named. Params are always spilled to the frame in the prologue, so
         * their location is exact. */
        (void)npool_used;
        for (int i = fn->vis_start; i < fn->nparams && i < fn->vis_start + 8; i++) {
            Expr *pe = fn->params[i];
            dbg_add_var(cg, df, pe->name, pe->span, pe->slot, -1, dbg_type_of(pe->type));
        }
        df->nparams = df->nvars;

        buf_printf(cg->out, "%s:\n", dbg_begin);
    }
    buf_printf(cg->out, "%s:\n", sym);
    /* The pool registers are saved *before* the frame pointer is established,
     * which is the usual shape and here is also the correct one. It leaves rbp
     * pointing at the top of this function's own frame, so a local at [rbp - k]
     * cannot collide with a saved register, and an incoming stack argument really
     * is at [rbp + 16 + soff] the way the ABI says it is.
     *
     * Saving rbp first and the pool registers after it needs rbp rebased below
     * them with a `lea`, to keep the two regions from overlapping -- and once it
     * is rebased, [rbp + 16] is a saved register rather than the caller's first
     * stack argument. Every parameter past the sixth was then read out of the
     * wrong slot, in any function that had both, at every optimization level
     * including -O0. `manyargs` is eight parameters and no locals, which is why
     * it passed: nothing there ever took a register.
     *
     * Dropping the `lea` also keeps this frame the shape a reader expects, which
     * is the form a frame-pointer-omitting pass would have to preserve. */
    for (int r = 0; r < NPOOL; r++)
        if (cg->pool_mask & (1 << r))
            buf_printf(cg->out, "  push %s\n", POOL_REGS[r]);
    buf_printf(cg->out, "  push rbp\n  mov rbp, rsp\n");
    if (frame > 0)
        buf_printf(cg->out, "  sub rsp, %d\n", frame);
    /* Spill the parameters into their frame slots.
     *
     * Where each one arrived is decided by assign_args, the same function the
     * call sites use. A float comes in a vector register, and the vector sequence
     * is numbered independently of the integer one, so a float parameter is read
     * from xmm<n> even when a hidden argument -- a closure's environment, a
     * struct-return buffer -- already occupies rdi. That is what the System V ABI
     * actually says, and the two conventions cannot be mixed: a method reached
     * both directly and through a method pointer would otherwise read its float
     * parameter from a different register depending on how it was called.
     *
     * An argument past its register cap is read from the caller's stack, one
     * eight-byte slot per argument in order, and that offset is measured from rbp,
     * which means it has to account for everything this prologue pushed. Between
     * the caller's stack and rbp there is, in order, the return address, this
     * function's saved frame pointer, and one slot per pool register it saved --
     * so the first stack argument is at [rbp + 16 + 8*npool], not [rbp + 16].
     *
     * That term was missing, and every parameter past the sixth was read out of a
     * saved callee-saved register instead of the caller's stack, so a function with
     * more than six integer parameters returned whatever happened to be in rbx. It
     * is a constant offset per function, which is why nothing about the call site
     * is wrong and why `manyargs` passes: it has eight parameters and no locals, so
     * it never saves a pool register and the term is zero. */
    if (fn->nparams > 0) {
        Type **ptypes = arena_alloc_array(cg->arena, (size_t)fn->nparams, sizeof(Type *));
        for (int i = 0; i < fn->nparams; i++)
            ptypes[i] = fn->params[i]->type;
        ArgAssign aa;
        assign_args(ptypes, fn->nparams, 0, &aa);
        for (int i = 0; i < fn->nparams; i++) {
            if (is_kind(fn->params[i]->type, TK_F64) && aa.reg[i] != NULL) {
                buf_printf(cg->out, "  movsd QWORD PTR [rbp - %d], %s\n", fn->params[i]->slot,
                           aa.reg[i]);
            } else if (aa.reg[i] == NULL) {
                buf_printf(cg->out, "  mov r11, QWORD PTR [rbp + %d]\n",
                           16 + 8 * npool_used + aa.soff[i]);
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], r11\n", fn->params[i]->slot);
            } else if (fn->params[i]->boxed) {
                /* A captured parameter: the slot must hold the box, not the
                 * value, or a closure reading the environment would find an
                 * integer where it expects a pointer. */
                buf_printf(cg->out, "  mov rdi, %s\n  call z_box\n", aa.reg[i]);
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", fn->params[i]->slot);
            } else {
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], %s\n", fn->params[i]->slot,
                           aa.reg[i]);
            }
        }
    }
    gen_block_items(cg, fn->fbody);
    if (have_dbg)
        dbg_collect_locals(cg, df, fn->fbody);
    /* rsp is already at the deepest saved pool register once rbp is rebased,
     * so pop them in reverse push order and then rbp. The return value stays
     * in rax. */
    /* The pool registers were pushed before rbp, so they come off after it. The
     * return value stays in rax, which neither of these touches. */
    buf_printf(cg->out, ".Lret_%s:\n  mov rsp, rbp\n", sym);
    buf_printf(cg->out, "  pop rbp\n", sym);
    for (int r = NPOOL - 1; r >= 0; r--)
        if (cg->pool_mask & (1 << r))
            buf_printf(cg->out, "  pop %s\n", POOL_REGS[r]);
    buf_printf(cg->out, "  ret\n", sym);
    if (have_dbg)
        buf_printf(cg->out, "%s:\n", dbg_end);
}

/* Emits `n` bytes as the body of an .asciz, escaping what the assembler needs
 * escaped. The length is a parameter rather than a terminator because a Z string
 * may contain a zero byte -- that is the whole point of it having a length --
 * and a loop that stopped at the first zero would silently emit a truncated
 * literal while the header above it claimed the full length. */
static void emit_escaped_n(CG *cg, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
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
    cg.opt = Z_OPT_DEFAULT;
    if (opts != NULL) {
        cg.bounds_checks = opts->bounds_checks;
        cg.debug_info = opts->debug_info;
        cg.opt = opts->opt_level;
        if (cg.opt < Z_OPT_MIN)
            cg.opt = Z_OPT_MIN;
        if (cg.opt > Z_OPT_MAX)
            cg.opt = Z_OPT_MAX;
    }

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
            !program->items[i]->is_generic_template) {
            emit_function(&cg, program->items[i]);
        }
    }

    /* Emit struct methods (mangled `Struct__method`) and property accessors. */
    for (int i = 0; i < program->nitems; i++) {
        if (program->items[i]->kind == S_STRUCT && program->items[i]->sdef != NULL) {
            StructDef *sd = program->items[i]->sdef;
            /* A struct reached through two declarations -- which a generic
             * instance is, while it is being built, since a method inside its own
             * body can name the type again -- is emitted once. Emitting it twice
             * gave the assembler two definitions of the same symbol, and the
             * error named the constructor rather than anything that would have
             * pointed here. */
            for (int prev = 0; prev < i; prev++) {
                if (program->items[prev]->kind == S_STRUCT && program->items[prev]->sdef == sd) {
                    sd = NULL;
                    break;
                }
            }
            if (sd == NULL)
                continue;
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
        buf_puts(&out, "  .globl main\nmain:\n  push rbp\n  mov rbp, rsp\n");
        buf_puts(&out, "  call z_main\n");
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
    /* Interface tables, one per (implementing type, interface) pair the program
     * actually converted to. Each is an array of code pointers in the interface's
     * declaration order; a class's entries are trampolines that go through the
     * receiver's vtable, so a subclass stored in an interface still calls the
     * override. */
    for (int i = 0; i < cg.nitabs; i++) {
        StructDef *impl = cg.itabs[i].impl;
        IfaceDef *id = cg.itabs[i].idef;
        if (impl == NULL || id == NULL)
            continue;
        if (impl->is_class) {
            /* One trampoline per slot, and these are instructions rather than
             * data: they are reached by jumping to them, so they have to be in
             * .text. Emitting them here among the vtables put executable bytes in
             * .rodata, and calling one jumped into the string table. */
            buf_puts(&out, "  .text\n");
            for (int m = 0; m < id->nmethods; m++) {
                StructMethod *sm = struct_find_method(impl, id->methods[m].name);
                if (sm == NULL)
                    continue;
                buf_printf(&out, "$tr$%s$%s$%d:\n", impl->name, id->name, m);
                buf_printf(&out, "  mov rax, QWORD PTR [rdi]\n");
                buf_printf(&out, "  mov rax, QWORD PTR [rax + %d]\n", sm->vtable_index * 8);
                buf_puts(&out, "  jmp rax\n");
            }
            buf_puts(&out, "  .section .rodata\n");
        }
        buf_printf(&out, "%s:\n", iface_itab_symbol(id, impl));
        for (int m = 0; m < id->nmethods; m++) {
            StructMethod *sm = struct_find_method(impl, id->methods[m].name);
            if (impl->is_class) {
                buf_printf(&out, "  .quad $tr$%s$%s$%d\n", impl->name, id->name, m);
            } else if (sm != NULL && sm->body != NULL) {
                buf_printf(&out, "  .quad %s\n", z_sym(&cg, sm->body->fname));
            } else {
                buf_puts(&out, "  .quad 0\n");
            }
        }
    }
    /* 64-bit magic multipliers and divisors referenced by [rip + .Lro<i>]. */
    for (int i = 0; i < cg.nrodata; i++)
        buf_printf(&out, ".Lro%d:\n  .quad %llu\n", i, (unsigned long long)cg.rodata[i]);
    /* A string literal is a Z string: a { len, cap } header, the bytes, and a
     * trailing zero. The header is static data rather than something built at
     * run time, so a literal costs a `lea` and no allocation, exactly as before
     * -- the length is already known at compile time, which is the one case
     * where knowing it costs nothing.
     *
     * `cap` is 0, and that is the whole point of the field here rather than a
     * coincidence: 0 is how the runtime tells a literal from a heap string, and
     * so a literal is never freed and never grown in place. `cap == len` would
     * say "no spare capacity" just as well, but it would also be indistinguishable
     * from a heap string holding exactly its own length -- which is every string
     * the library returns -- and freeing one of those would be a free() of a
     * .rodata address.
     *
     * The label names the *bytes*, sixteen past the header, because that is the
     * value a Z string holds.
     *
     * .rodata, so a literal really is read-only. That is what makes the
     * cap == 0 rule above a guarantee rather than a convention: a literal
     * handed to the buffer-append path reallocates instead of writing in place,
     * so the one path that would scribble over a literal cannot reach it. */
    buf_puts(&out, "  .section .rodata\n");
    for (int i = 0; i < strings->count; i++) {
        buf_printf(&out, ".align 8\n.Lstr%d:\n  .quad %d\n  .quad 0\n  .asciz \"", i,
                   strings->lens[i]);
        emit_escaped_n(&cg, strings->items[i], strings->lens[i]);
        buf_puts(&out, "\"\n");
    }
    /* The empty string, for a program that has no literals at all. Without it
     * there is no label to point a default at, and a zero-length string is a
     * perfectly ordinary thing to have. */
    buf_puts(&out, ".align 8\n.Lstrempty:\n  .quad 0\n  .quad 0\n  .asciz \"\"\n");
    /* .Lfmt_int is a printf format, not a Z string, so it gets no header. The
     * two bool strings are Z strings -- a bool concatenates into one, and that
     * is not a special case worth branching on -- so they are laid out with
     * headers like every other literal and referenced sixteen past the label. */
    /* The names `to_text` prints for an aggregate, one per type that a call
     * actually reached. The calls were recorded while the code was generated, so
     * this is exactly the set of labels the body refers to. */
    buf_puts(&out, ".Ltype_array:\n  .asciz \"array\"\n");
    for (int i = 0; i < cg.ntname; i++) {
        buf_printf(&out, ".Ltype_%s:\n  .asciz \"", cg.tname[i]);
        emit_escaped_n(&cg, cg.tname[i], (int)strlen(cg.tname[i]));
        buf_puts(&out, "\"\n");
    }
    buf_puts(&out, ".Lfmt_int:\n  .asciz \"%ld\\n\"\n");
    buf_puts(&out, ".align 8\n.Ltrue_str:\n  .quad 4\n  .quad 0\n  .asciz \"true\"\n");
    buf_puts(&out, ".align 8\n.Lfalse_str:\n  .quad 5\n  .quad 0\n  .asciz \"false\"\n");

    /* The debug sections go last, once every function's description is known.
     * They return to .text afterwards so the section note below is not left
     * pointing into .debug_info. */
    if (cg.debug_info) {
        dbg_write_abbrev(&out);
        dbg_write_info(&cg);
    }

    buf_puts(&out, "  .section .note.GNU-stack,\"\",@progbits\n");

    if (out.data == NULL) {
        out.data = malloc(1);
        if (out.data == NULL)
            die_oom();
        out.data[0] = '\0';
    }
    free(cg.locals);
    free(cg.rodata);
    free(cg.dbg_funcs);
    free(cg.tname);
    return out.data;
}

char *codegen_emit(Arena *arena, Stmt *program, StringTable *strings) {
    return codegen_emit_opts(arena, program, strings, NULL);
}
