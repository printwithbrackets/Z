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

/* Emits a null check on the address in rax before it is loaded through or
 * stored through. A null pointer otherwise faults at a low address that the
 * OS maps to nothing, which is a bare SIGSEGV with no message. Controlled by
 * the same flag as the array checks: --no-bounds turns off both, so the one
 * flag means "trust the program" (there is no separate --no-null-check).
 * Preserves rax; clobbers only rdi. */
static void emit_null_check(CG *cg) {
    if (!cg->bounds_checks)
        return;
    int lok = next_label(cg);
    buf_printf(cg->out, "  test rax, rax\n  jnz .L%d\n", lok);
    buf_printf(cg->out, "  call z_null_deref_fail\n");
    buf_printf(cg->out, ".L%d:\n", lok);
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
