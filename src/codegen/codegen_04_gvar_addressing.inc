/* ---- gvar: reading and writing project storage ----
 *
 * A global is a real symbol, not a frame slot, so every access is rip-relative
 * against it. `gen_addr` produces the address and the existing load and store
 * paths do the rest, which is why a global goes through `E_VAR` at all rather
 * than needing a parallel expression kind: there is one way to name one and it
 * lands where a local's name lands.
 *
 * The register allocator is keyed by slot and a global's slot is -1, so
 * gen_leaf_to_reg has to answer for it before the allocator is consulted -- a
 * slot of -1 would otherwise be looked up as a local and read whatever -1 is
 * standing for.
 */
static const char *global_sym(CG *cg, int gidx) {
    if (gidx < 0 || gidx >= cg->nglobals || cg->globals == NULL)
        return NULL;
    return cg->globals[gidx].sym;
}

/* Computes the address of an lvalue expression into rax. */
static void gen_addr(CG *cg, Expr *e) {
    switch (e->kind) {
    case E_VAR: {
        const char *gsym = global_sym(cg, e->gidx);
        /* rip-relative: there is no frame to be relative to, and .data or .bss
         * may sit further than any absolute form this target offers. */
        if (gsym != NULL) {
            buf_printf(cg->out, "  lea rax, [rip + %s]\n", gsym);
            return;
        }
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
    }
    case E_DEREF:
        /* The pointer operand *is* the address, so it is checked for null here
         * before any load or store goes through it. Both reads and writes reach
         * this through gen_addr. */
        gen_expr(cg, e->lhs);
        emit_null_check(cg);
        break;
    case E_INDEX: {
        /* A string element is a byte, so its address is base + index with no
         * scaling. The length lives in the header, so there is a bound to check
         * it against exactly as an array has one at ptr[-8]. */
        int is_str = is_kind(e->lhs->type, TK_STRING);
        int esz = is_str ? 1 : type_size(e->lhs->type->base);
        int t = temp_alloc(cg);
        gen_expr(cg, e->lhs); /* base pointer */
        /* A null array or string base would otherwise fault reading its length
         * header at ptr[-8] (or [-16]) with no message. Same flag as `*`. */
        emit_null_check(cg);
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
         * pointer value is the address -- and a null one is a null dereference,
         * checked here under the same flag as `*` (off with --no-bounds). */
        gen_expr(cg, e->lhs);
        if (e->lhs->type != NULL &&
            (is_kind(e->lhs->type, TK_PTR) || is_kind(e->lhs->type, TK_ARRAY)))
            emit_null_check(cg);
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
        if (cg->tname_cap > INT_MAX / 2)
            die_oom();
        int ncap = cg->tname_cap == 0 ? 8 : cg->tname_cap * 2;
        if ((size_t)ncap > SIZE_MAX / sizeof(char *))
            die_oom();
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
    /* A method pointer returning a struct is refused by the parser, so a result
     * buffer and a *bound* receiver cannot both want rdi here. A closure and an
     * interface are not refused, and they can. This comment used to claim they
     * were, which is why the combination was never exercised. */
    int iface = is_kind(e->lhs->type, TK_IFACE);
    int bound = is_kind(e->lhs->type, TK_MPTR);
    /* A closure's cell is { code, env }, the same two words in the same order as
     * a bound method's, so the hidden first argument is the environment for one
     * and the receiver for the other. */
    int closure = is_kind(e->lhs->type, TK_CLOSURE);
    int hidden_env = bound || closure || iface;
    int tf = temp_alloc(cg);
    gen_expr(cg, e->lhs); /* the callable */
    store_temp(cg, tf);
    int base = cg->temp_top;
    cg->temp_top += e->nargs;
    if (cg->temp_top > cg->temp_high)
        cg->temp_high = cg->temp_top;
    gen_args_stage(cg, e, base);
    /* A bound pointer's receiver, or a closure's environment, is a hidden argument
     * of its own, and so is a struct result buffer. When both are present there
     * are *two* leading integer-class arguments, not one: the buffer takes `rdi`
     * and the receiver or environment takes `rsi`, and the declared arguments
     * start at `rdx`. Counting one is what put the declared arguments a register
     * too early, and what made the receiver load below land on top of the buffer.
     *
     * The vector sequence is unaffected either way. The ABI numbers the integer
     * and vector registers independently, so a leading integer argument does not
     * push the first declared float out of `xmm0`. */
    ArgAssign aa;
    args_prologue(cg, e, base, (sret ? 1 : 0) + (hidden_env ? 1 : 0), &aa);
    if (sret) {
        buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
    }
    load_temp(cg, tf, "r11");
    /* Where the receiver or environment goes depends on whether a result buffer took
     * the register below it. */
    const char *envreg = ARG_REGS[sret ? 1 : 0];
    if (iface) {
        /* An interface cell is { itab, receiver }. The code address is not in the
         * cell -- the cell holds one itab for the whole interface -- so it comes
         * from the slot this method's name resolved to at parse time. */
        buf_printf(cg->out, "  mov %s, QWORD PTR [r11 + 8]\n", envreg);
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n");
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11 + %d]\n", e->vtable_index * 8);
    } else if (bound) {
        /* r11 is the binding cell: { code, receiver }. */
        buf_printf(cg->out, "  mov %s, QWORD PTR [r11 + 8]\n", envreg);
        buf_printf(cg->out, "  mov r11, QWORD PTR [r11]\n");
    } else if (closure) {
        /* { code, env }: the environment becomes the hidden first argument, after
         * the result buffer if there is one, and the declared arguments were placed
         * above both, which is where the hoisted function expects to find them. */
        buf_printf(cg->out, "  mov %s, QWORD PTR [r11 + 8]\n", envreg);
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
        /* The result buffer goes in rdi, after the arguments have been placed and
         * before the call.
         *
         * `args_prologue` was told there is a hidden argument, so the first
         * declared one went to rsi, which is right, and nothing then put the
         * buffer in the register it had vacated. The callee wrote the struct
         * through whatever rdi happened to hold and the caller read a buffer
         * nothing had written, so a virtual method returning a struct answered
         * with whatever was in the frame, or wrote far enough through the stray
         * pointer to take the process down.
         *
         * The `lea` that did this was emitted, but below the `return` that ends
         * this path, which made it dead code. It is here now and the tail it was
         * in is gone. */
        if (sret) {
            buf_printf(cg->out, "  lea rdi, [rbp - %d]\n", rt_addr);
        }
        load_temp(cg, vt, "r11");
        cg->temp_top = vt;
        buf_printf(cg->out, "  call r11\n");
        args_epilogue(cg, &aa);
        if (sret) {
            /* The result value is the address of the buffer, and the buffer stays
             * reserved for the rest of the statement so a later nested call does
             * not clobber it. The argument temps above it are freed. */
            buf_printf(cg->out, "  lea rax, [rbp - %d]\n", rt_addr);
            cg->temp_top = rt_base + rt_nt;
        } else {
            cg->temp_top = vt;
        }
        return;
    }
}

/* True if `e` is a left-leaning arithmetic accumulation chain rooted at a read
 * of `slot` (e.g. `sum + a + b - c` for slot=sum), where every right operand is
 * a constant or a simple leaf. Such a chain can be applied directly in the
 * register already holding `slot`'s value (three-address style).
 *
 * The *type* is the first thing checked, and it has to be. A string has the same
 * shape: `s + t` is an addition rooted at a read of `s` with a variable on the
 * right, which is everything below looks at, and nothing in the shape says the
 * addition is the machine's. So a string chain was applied as integer arithmetic
 * on the pointer, `s = s + t` produced an address rather than a string, and
 * reading it back read whatever that address held. The test said 139 and nothing
 * else, because the address was not a string to begin with.
 *
 * An int and a bool qualify: the value in the register is the number, and `add`
 * is the operation. A float does not, and has its own in-place path above the
 * call site, where the value is in a vector register. A string has to go through
 * the runtime's concatenation, which allocates and therefore cannot be folded
 * into an integer add. A pointer has no arithmetic here, and an aggregate is not
 * a scalar at all. */
static int is_accum_chain(CG *cg, Expr *e, int slot) {
    if (e == NULL || (e->type != NULL && !is_kind(e->type, TK_INT) && !is_kind(e->type, TK_BOOL)))
        return 0;
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
