#include "codegen.h"
#include "z_limits.h" /* project-local: Z_MAX_ARGS etc. */
#include <limits.h>   /* system: INT_MAX for checked growth */

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
    if (extra > SIZE_MAX - b->len - 1)
        die_oom();
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t ncap = b->cap == 0 ? 4096 : b->cap;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
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

/* Debug-only cap: at most 256 described locals per function. Only affects -g
 * DWARF output; past the cap the first 256 variables are described and one
 * warning is emitted (see dbg_add_var). Codegen itself is unaffected. */
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

/* Legacy cap, now unused: DbgFunc storage is grown dynamically with realloc
 * (see emit_function), so there is no function-count limit. Kept so the
 * historical bound stays visible. */
#define DBUG_MAX_FUNCS 1024

/* Debug-only cap: at most 128 distinct source files in the DWARF file table
 * (an `import` pulls in more). Past the cap line markers reuse file 1 and one
 * warning is emitted (see dbg_file); the program itself is unaffected. */
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

/* Correctness cap: at most 256 distinct (implementing type, interface) pairs
 * per compilation. Past the cap compilation fails with an explicit error
 * (see iface_note_itab) rather than emitting a reference to a table that was
 * never written, which would fail later at link time with no Z-level message. */
#define MAX_ITABS 256

/* The symbol an itab is emitted under. Derived from both names, so two types
 * implementing one interface get two arrays, and one type implementing two
 * interfaces gets two arrays. Sized dynamically on the arena: a fixed buffer
 * here truncated long type names so two distinct pairs shared one symbol.
 * Takes the arena directly so this helper can live before the CG definition. */
static const char *iface_itab_symbol(Arena *arena, IfaceDef *id, StructDef *impl) {
    const char *an = impl != NULL && impl->name != NULL ? impl->name : "?";
    const char *bn = id != NULL && id->name != NULL ? id->name : "?";
    int need = snprintf(NULL, 0, "$itab$%s$%s", an, bn);
    if (need < 0)
        die_oom();
    char *out = arena_alloc(arena, (size_t)need + 1);
    if (out == NULL)
        die_oom();
    snprintf(out, (size_t)need + 1, "$itab$%s$%s", an, bn);
    return out;
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
    /* How hot the local is: each def or use adds 8^depth, where depth is the
     * number of loops around it. The colourer gives registers to the heaviest
     * locals first, so a loop counter read a dozen times per iteration beats a
     * pointer that is only declared once outside the loop. */
    long long weight;
    int colored; /* already considered by alloc_regs */
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
    /* The program's gvars. Reached through the CG rather than passed down every
     * call: a global reference is an E_VAR like any other, so the only thing
     * that has to know about them is the E_VAR path and the emitter. */
    const GlobalTable *globals;
    int nglobals;
    int label_counter;
    LoopCtx loops[MAX_LOOP_DEPTH];
    int loop_depth;
    int bounds_checks; /* array/string range checks and `*` null checks; one flag
                            covers both (--no-bounds disables both) */
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
    int pool_mask;        /* bitmask of POOL_REGS indices actually used */
    int loop_marker;      /* incremented once per loop walked; see LocalInfo.loop_marker */
    int alloc_loop_depth; /* loops around the statement the allocation walk is on; see
                             LocalInfo.weight */
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
        if (cg->ro_cap > INT_MAX / 2)
            die_oom();
        int ncap = cg->ro_cap ? cg->ro_cap * 2 : 8;
        if ((size_t)ncap > SIZE_MAX / sizeof(uint64_t))
            die_oom();
        cg->ro_cap = ncap;
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
static const char *global_sym(CG *cg, int gidx);
static int globals_need_init(CG *cg);

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

/* The code generator is split across the files in src/codegen and included here in order, so
 * the concatenation is the single translation unit parser.c used to be.
 * The split is a map, not a change: see the section banners in each part. */
#include "codegen/codegen_01_argument_passing.inc"
#include "codegen/codegen_02_float.inc"
#include "codegen/codegen_03_register_allocation.inc"
#include "codegen/codegen_04_gvar_addressing.inc"
#include "codegen/codegen_05_interfaces.inc"
#include "codegen/codegen_06_loop_unrolling.inc"
#include "codegen/codegen_07_licm.inc"
#include "codegen/codegen_08_dwarf.inc"
#include "codegen/codegen_09_gvar_storage_init.inc"
