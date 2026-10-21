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

/* Records that this pair is needed. Fails the compilation with an explicit
 * error when the table is full, rather than emitting a reference to a table
 * that was never written (which would fail later at link time with no
 * Z-level message) or silently calling the wrong entry. */
static int iface_note_itab(CG *cg, StructDef *impl, IfaceDef *idef) {
    for (int i = 0; i < cg->nitabs; i++)
        if (cg->itabs[i].impl == impl && cg->itabs[i].idef == idef)
            return 1;
    if (cg->nitabs >= MAX_ITABS) {
        fprintf(stderr, "z: too many distinct interface conversions (max %d)\n", MAX_ITABS);
        exit(1);
    }
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
        if (cg->pin_cap > INT_MAX / 2)
            die_oom();
        int ncap = cg->pin_cap == 0 ? 8 : cg->pin_cap * 2;
        if ((size_t)ncap > SIZE_MAX / sizeof(int))
            die_oom();
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
        /* A global, before anything that consults a slot. It has none: `slot` is
         * -1, which the constant-propagation lookup below would treat as a
         * local and the fast path would read as `[rbp - -1]` -- the saved frame
         * pointer and the return address, so a global read returned whatever the
         * call chain happened to leave there. */
        const char *gsym = global_sym(cg, e->gidx);
        if (gsym != NULL) {
            if (is_kind(e->type, TK_F64))
                buf_printf(cg->out, "  movsd %s, QWORD PTR [rip + %s]\n", XMM_ACC, gsym);
            else if (!is_aggregate(e->type))
                buf_printf(cg->out, "  mov rax, QWORD PTR [rip + %s]\n", gsym);
            else
                buf_printf(cg->out, "  lea rax, [rip + %s]\n", gsym);
            break;
        }
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
        const char *lreg = (e->lhs->kind == E_VAR && e->lhs->gidx < 0 && !e->lhs->agg_param)
                               ? local_reg(cg, e->lhs->slot)
                               : NULL;
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
         * this is what `len(s)` was not before the string had a header. A null
         * string would fault at ptr[-16] with no message, so it is checked
         * under the same flag as `*` (off with --no-bounds). */
        gen_expr(cg, e->lhs);
        emit_null_check(cg);
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
            /* Array .length lives in the 8 bytes before the data. A null array
             * would fault at ptr[-8] with no message, so it is checked under
             * the same flag as `*` (off with --no-bounds). */
            gen_expr(cg, e->lhs);
            emit_null_check(cg);
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
        buf_printf(cg->out, "  lea rdi, [rip + %s]\n", iface_itab_symbol(cg->arena, id, impl));
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
         * sitting in.
         *
         * The destination's *address* is computed once, here, and reused by the
         * store below. Computing it twice ran the index expression twice, so
         * `a[i++] = "x"` freed the value in `a[i]` and wrote to `a[i + 1]`, and
         * left `i` one higher than the program wrote. */
        int t_old = -1;
        int t_dst = -1;
        if (e->frees_old) {
            t_old = temp_alloc(cg);
            if (e->lhs != NULL && e->lhs->kind == E_VAR && e->lhs->gidx < 0 && !e->lhs->agg_param &&
                !e->lhs->boxed) {
                const char *oreg = local_reg(cg, e->lhs->slot);
                if (oreg != NULL)
                    buf_printf(cg->out, "  mov %s, %s\n", "rax", oreg);
                else
                    buf_printf(cg->out, "  mov rax, QWORD PTR [rbp - %d]\n", e->lhs->slot);
            } else {
                /* The address is kept rather than only the value at it, so that
                 * the store below writes where the release read. */
                t_dst = temp_alloc(cg);
                gen_addr(cg, e->lhs);
                store_temp(cg, t_dst);
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
            load_temp(cg, tb, "r11"); /* r11 = the box pointer */
            if (is_kind(e->type, TK_F64)) {
                /* A compound assignment reads the value it is adding to, and this
                 * path did not: it was written as a plain store, the `compound`
                 * flag was never read here at all, and `f += 2.5` over a captured
                 * `f` stored 2.5. The other three assignment forms have always done
                 * this read-modify-write.
                 *
                 * The accumulator is `XMM_ACC` in both halves, and the addend goes
                 * in the scratch beside it. Loading the addend into `XMM_ACC` and
                 * storing `XMM_ACC` is right for the plain case only, and the two
                 * branches have to be written so the register that is read is the
                 * register that is stored. */
                if (e->compound) {
                    buf_printf(cg->out, "  movsd %s, QWORD PTR [r11]\n", XMM_ACC);
                    load_temp_x(cg, tr, XMM_SCRATCH[1]);
                    static const char *const fops[] = {"addsd", "subsd", "mulsd", "divsd"};
                    static const TokenKind fkinds[] = {T_PLUS, T_MINUS, T_STAR, T_SLASH};
                    for (size_t i = 0; i < sizeof fops / sizeof(*fops); i++)
                        if (e->op == fkinds[i])
                            buf_printf(cg->out, "  %s %s, %s\n", fops[i], XMM_ACC, XMM_SCRATCH[1]);
                } else {
                    load_temp_x(cg, tr, XMM_ACC);
                }
                buf_printf(cg->out, "  movsd QWORD PTR [r11], %s\n", XMM_ACC);
            } else {
                load_temp(cg, tb, "rax"); /* rax = the box pointer */
                if (e->compound) {
                    /* Read what is already in the cell and apply the operator to
                     * it, which is what `emit_binop_op` expects: rax on the left,
                     * r11 on the right. The plain form skips the dereference
                     * entirely, and that is the whole difference.
                     *
                     * `r11` is loaded with the right operand *inside* this branch
                     * and not before it. It already holds the box pointer, and
                     * loading over it left the store below writing the value into
                     * itself -- which silently turned every plain assignment to a
                     * captured variable into a no-op. */
                    load_temp(cg, tr, "r11");
                    buf_printf(cg->out, "  mov rax, QWORD PTR [rax]\n");
                    emit_binop_op(cg, e->op);
                    load_temp(cg, tb, "r11"); /* the box pointer again, to store */
                } else {
                    load_temp(cg, tr, "rax");
                }
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
        if (e->lhs != NULL && e->lhs->kind == E_VAR && e->lhs->gidx < 0 && !is_aggregate(e->type) &&
            !e->lhs->agg_param)
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
        if (t_dst >= 0) {
            /* The address was already computed above, to read the old value
             * out of it. Reusing it keeps the index expression from running
             * twice. */
            load_temp(cg, t_dst, "rax");
        } else {
            gen_addr(cg, e->lhs);
        }
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
    /* A string comparison cannot take the branch-on-flags path below either:
     * that one emits an integer `cmp` of the two pointers, where the value
     * path calls z_strcmp, so two equal strings in different allocations read
     * as different. Evaluating the condition normally leaves a 0 or 1 in rax,
     * which is what the shared tail branches on. */
    if (cond != NULL && cond->kind == E_BINARY && is_compare_op(cond->op) &&
        (is_kind(cond->lhs->type, TK_STRING) || is_kind(cond->rhs->type, TK_STRING))) {
        gen_expr(cg, cond);
        buf_printf(cg->out, "  cmp rax, 0\n  je .L%d\n", false_label);
        return;
    }
    if (cond != NULL && cond->kind == E_BINARY && is_compare_op(cond->op) &&
        is_pure_expr(cond->lhs) && is_pure_expr(cond->rhs)) {
        long long cr;
        int lhs_direct = 0;
        const char *lreg = NULL;
        if (cond->lhs->kind == E_VAR && cond->lhs->gidx < 0 && !cond->lhs->agg_param &&
            !is_aggregate(cond->lhs->type))
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
    /* The in-place forms below read and write the register the destination slot
     * was allocated, so a captured destination is not one of them: its slot holds
     * a box pointer, and `b += 10` added 10 to the address of the cell rather than
     * to the value inside it. Those go to gen_expr, which dereferences.
     *
     * `frees_old` is the same kind of exclusion: an assignment that replaces a
     * string slot has to release the allocation it held, and every in-place form
     * below writes the destination without ever reading or freeing the old value.
     * The release lives in the E_ASSIGN arm of gen_expr, so a plain assignment
     * that owns its destination's old value is handed there rather than folded
     * into a bare `mov`. Skipping that was a per-iteration leak: `row = row + "."`
     * as a statement kept `row` in a register, took the in-place path, and leaked
     * every older concatenation (the nested-loop `z_concat` leak). */
    if (e != NULL && e->kind == E_ASSIGN && !e->frees_old && e->lhs != NULL &&
        e->lhs->kind == E_VAR && !is_aggregate(e->type) && !e->lhs->agg_param && !e->lhs->boxed) {
        /* Never a register for a global: it has no slot, so this is NULL and the
         * path below falls through to an address-and-store, which is what a
         * global needs. */
        const char *lreg = e->lhs->gidx < 0 ? local_reg(cg, e->lhs->slot) : NULL;
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
