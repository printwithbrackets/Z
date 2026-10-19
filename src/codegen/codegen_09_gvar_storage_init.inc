/* ---- gvar: storage and the init pass ----
 *
 * Two shapes, and which one a global gets is decided by its initializer rather
 * than by its type:
 *
 *   - a literal initializer is emitted as data. `gint n = 5;` is one
 *     instruction's worth of constant, and a `.quad` in .data is both smaller
 *     and impossible to get wrong at run time.
 *   - anything else is zeroed in .bss and filled by z_ginit, which the shim
 *     calls before the entry point. `gvar x = new R(5);` cannot be a constant,
 *     and the alternative -- requiring every global to be a literal -- would
 *     leave out most of what a global is for.
 *
 * Zero rather than left undefined on purpose: a program that reads a global
 * whose initializer has not run yet reads zero, which is diagnosable, where
 * reading whatever was in .bss is not. z_ginit runs before any user code, so
 * that window is only reachable from another initializer.
 */
/* Whether anything needs the run-time init pass at all. Both the emitter and the
 * entry shim ask this, and they have to agree: a program whose globals are all
 * literals emits no z_ginit, so a shim that called it unconditionally would not
 * link. */
static int globals_need_init(CG *cg) {
    for (int i = 0; i < cg->nglobals; i++)
        if (!cg->globals[i].is_const && cg->globals[i].init != NULL)
            return 1;
    return 0;
}

static void emit_global_storage(CG *cg) {
    if (cg->nglobals == 0 || cg->globals == NULL)
        return;

    int any_const = 0, any_zero = 0;
    for (int i = 0; i < cg->nglobals; i++) {
        if (cg->globals[i].is_const)
            any_const = 1;
        else
            any_zero = 1;
    }

    if (any_const) {
        buf_puts(cg->out, "  .section .data\n");
        for (int i = 0; i < cg->nglobals; i++) {
            const GlobalTable *g = &cg->globals[i];
            if (!g->is_const)
                continue;
            buf_printf(cg->out, "  .globl %s\n", g->sym);
            if (is_kind(g->type, TK_F64)) {
                /* The raw 8 bytes of the double, which is what .quad needs.
                 * Going through an assembler-visible float directive would
                 * work too, but this keeps every global one shape to read. */
                unsigned long long bits;
                double d = g->dval;
                memcpy(&bits, &d, sizeof bits);
                buf_printf(cg->out, "%s:\n  .quad %llu\n", g->sym, bits);
            } else if (is_kind(g->type, TK_BOOL)) {
                buf_printf(cg->out, "%s:\n  .byte %d\n", g->sym, g->ival != 0 ? 1 : 0);
            } else if (is_kind(g->type, TK_PTR)) {
                buf_printf(cg->out, "%s:\n  .quad 0\n", g->sym);
            } else {
                buf_printf(cg->out, "%s:\n  .quad %lld\n", g->sym, g->ival);
            }
        }
    }

    if (any_zero) {
        buf_puts(cg->out, "  .bss\n");
        for (int i = 0; i < cg->nglobals; i++) {
            const GlobalTable *g = &cg->globals[i];
            if (g->is_const)
                continue;
            buf_printf(cg->out, "  .globl %s\n  .align 8\n%s:\n  .zero %d\n", g->sym, g->sym,
                       type_size(g->type) > 0 ? type_size(g->type) : 8);
        }
    }
    buf_puts(cg->out, "  .text\n");
}

/* The initializer pass. Emitted only when something actually needs it, so a
 * program with no computed global carries no extra symbol and no extra call.
 *
 * It runs at the default optimization level whatever the command line asked
 * for. These are assignments at run time with no enclosing control flow, so
 * there is nothing for a level to do to them, and emitting it at -O3 would mean
 * the allocator's frame sizing had to be right in a body no other path reaches
 * -- for no gain. */
static void emit_global_init(CG *cg) {
    if (!globals_need_init(cg))
        return;

    /* The body needs the same scratch state a function does, since gen_expr may
     * spill a temporary or want a label. Reset rather than inherited from the
     * last function emitted, so an initializer cannot start inside somebody
     * else's frame. */
    cg->cur_locals_bytes = 0;
    cg->cur_ret_struct = 0;
    cg->cur_ret_slot = 0;
    cg->temp_top = 0;
    cg->temp_high = 0;
    cg->cur_sym = "z_ginit";

    /* Measured first and discarded, exactly as emit_function does: the frame
     * cannot be sized until the body has said how deep it went, and emitting
     * before knowing that would put a prologue in the way of the rollback. */
    size_t saved_len = cg->out->len;
    cg->measuring = 1;
    for (int i = 0; i < cg->nglobals; i++) {
        const GlobalTable *g = &cg->globals[i];
        if (g->is_const || g->init == NULL)
            continue;
        gen_expr(cg, g->init);
    }
    cg->measuring = 0;
    cg->out->len = saved_len;
    if (cg->out->data != NULL)
        cg->out->data[saved_len] = '\0';
    cg->temp_top = 0;

    int frame = align16(8 + cg->cur_locals_bytes + 8 * (cg->temp_high + 1));
    buf_printf(cg->out, "  .globl z_ginit\nz_ginit:\n  push rbp\n  mov rbp, rsp\n");
    buf_printf(cg->out, "  sub rsp, %d\n", frame);
    for (int i = 0; i < cg->nglobals; i++) {
        const GlobalTable *g = &cg->globals[i];
        if (g->is_const || g->init == NULL)
            continue;
        gen_expr(cg, g->init);
        if (is_kind(g->type, TK_F64))
            buf_printf(cg->out, "  movsd QWORD PTR [rip + %s], %s\n", g->sym, XMM_ACC);
        else
            buf_printf(cg->out, "  mov QWORD PTR [rip + %s], rax\n", g->sym);
    }
    buf_puts(cg->out, "  mov rsp, rbp\n  pop rbp\n  ret\n");
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
        cg.globals = opts->globals;
        cg.nglobals = opts->nglobals;
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

    emit_global_storage(&cg);
    emit_global_init(&cg);

    /* The C runtime calls main(); our entry point is renamed z_main so it
     * never collides with the user's own `main`. The `push rbp` realigns the
     * stack for the ABI. */
    if (entry != NULL) {
        buf_puts(&out, "  .globl main\nmain:\n  push rbp\n  mov rbp, rsp\n");
        /* Before the entry point, so every global is initialized by the time any
         * user code can read one. That is the whole reason this is a function
         * rather than a .data initializer: `gvar x = other;` depends on `other`
         * being set first, and only a run-time pass can honour that order. */
        if (globals_need_init(&cg))
            buf_puts(&out, "  call z_ginit\n");
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
                /* The receiver is in the register the real callee will look in,
                 * which is not always `rdi`. A struct is returned through memory,
                 * so a method returning one takes the result buffer in `rdi` and
                 * the receiver in `rsi`. Reading `rdi` here regardless meant the
                 * trampoline dereferenced the *buffer* as though it were the
                 * object and jumped to whatever the length header held. */
                int sret = is_kind(sm->ret, TK_STRUCT) || is_kind(sm->ret, TK_UNION);
                buf_printf(&out, "  mov rax, QWORD PTR [%s]\n", sret ? "rsi" : "rdi");
                buf_printf(&out, "  mov rax, QWORD PTR [rax + %d]\n", sm->vtable_index * 8);
                buf_puts(&out, "  jmp rax\n");
            }
            buf_puts(&out, "  .section .rodata\n");
        }
        buf_printf(&out, "%s:\n", iface_itab_symbol(cg.arena, id, impl));
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
