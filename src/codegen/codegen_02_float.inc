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
        /* A captured variable's slot holds a pointer to a heap cell, and the slot
         * is eight bytes wide, so loading it as a double loads an address and
         * prints something like 3.35e-315. A read of a captured float inside its
         * own lambda is rewritten to `*env[k]` and so arrives as an E_DEREF; this
         * case is the enclosing function reading the same variable, and it has to
         * dereference too. */
        const char *gsym = global_sym(cg, e->gidx);
        if (gsym != NULL) {
            /* gen_float has its own E_VAR arm, so the arm gen_expr was given
             * does not cover a float read -- and `slot` is -1 for a global,
             * which here read as `[rbp - -1]`: the saved frame pointer, as a
             * denormal about 1e-38. */
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rip + %s]\n", XMM_ACC, gsym);
            return;
        }
        if (e->agg_param)
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rax]\n", XMM_ACC);
        else if (e->boxed) {
            buf_printf(cg->out, "  mov r11, QWORD PTR [rbp - %d]\n", e->slot);
            buf_printf(cg->out, "  movsd %s, QWORD PTR [r11]\n", XMM_ACC);
        } else
            buf_printf(cg->out, "  movsd %s, QWORD PTR [rbp - %d]\n", XMM_ACC, e->slot);
        return;
    }
    case E_DEREF:
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
