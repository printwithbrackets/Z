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
    if (cg->dbg_file_count >= MAX_DBG_FILES) {
        /* Debug-only degradation: attribute further files to file 1 and warn
         * once, rather than silently misattributing or asserting. The program
         * itself is unaffected. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "z: warning: too many debug files (max %d), keeping first %d\n",
                    MAX_DBG_FILES, MAX_DBG_FILES);
        }
        return 1;
    }
    cg->dbg_file_names[cg->dbg_file_count] = path;
    cg->dbg_file_count++;
    /* The path is escaped, because a file name is not under the program's
     * control and a quote in it would otherwise end the string early and let
     * the rest of the name be read as assembler directives. A name with a quote
     * and a newline in it is legal on a Unix filesystem, and git and tar both
     * permit one, so the name is data the compiler did not write. */
    buf_printf(cg->out, "  .file %d \"", cg->dbg_file_count);
    emit_escaped_n(cg, path, (int)strlen(path));
    buf_puts(cg->out, "\"\n");
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
 * described honestly. Past DBUG_MAX_VARS the first 256 are kept and one
 * warning is emitted: debug-only degradation, never a silent truncation of the
 * program itself. */
static void dbg_add_var(CG *cg, DbgFunc *f, const char *name, Span span, int slot, int reg,
                        int type_code) {
    if (f->nvars >= DBUG_MAX_VARS) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "z: warning: too many debug variables (max %d), keeping first %d\n",
                    DBUG_MAX_VARS, DBUG_MAX_VARS);
        }
        return;
    }
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

/* The symbol a global is emitted under. Separate from z_sym only in spelling --
 * `z$g_` rather than `z$` -- so a global can never collide with a function of
 * the same name, which matters because both are program-global symbols and a
 * collision would be the assembler's problem rather than the compiler's. */
char *codegen_global_sym(Arena *arena, const char *name) {
    static const char pre[] = Z_SYM_PREFIX "g_";
    size_t n = strlen(name);
    char *out = arena_alloc(arena, n + sizeof pre);
    memcpy(out, pre, sizeof pre);
    memcpy(out + sizeof pre - 1, name, n + 1);
    return out;
}

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
            if (cg->dbg_funcs_cap > INT_MAX / 2)
                die_oom();
            int ncap = cg->dbg_funcs_cap == 0 ? 32 : cg->dbg_funcs_cap * 2;
            if ((size_t)ncap > SIZE_MAX / sizeof(DbgFunc))
                die_oom();
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
        /* Every parameter is spilled first, and only then is any of them boxed.
         *
         * Boxing is a call, and the call is `z_box`, which calls `malloc`, and
         * `malloc` clobbers every caller-saved register including `rsi`. Doing
         * both in one pass in parameter order meant a captured parameter in `rdi`
         * was boxed before `rsi` was read, so every parameter after the first
         * captured one was stored from a register the allocator had already
         * overwritten:
         *
         *     int adder(int n, int x, int bias) {          // n is captured
         *         var f = (int y) => y + n + bias;
         *         return applyTwice(f, x);
         *     }
         *
         * `x` and `bias` came out as whatever `malloc` left in their registers.
         * A function whose only parameter is the captured one is unaffected, which
         * is why `makeAdder` in closures.z passes: its capture is the `rdi` one.
         *
         * The result buffer and a float do not change the argument, they only
         * change which registers are named. */
        for (int i = 0; i < fn->nparams; i++) {
            if (is_kind(fn->params[i]->type, TK_F64) && aa.reg[i] != NULL) {
                buf_printf(cg->out, "  movsd QWORD PTR [rbp - %d], %s\n", fn->params[i]->slot,
                           aa.reg[i]);
            } else if (aa.reg[i] == NULL) {
                buf_printf(cg->out, "  mov r11, QWORD PTR [rbp + %d]\n",
                           16 + 8 * npool_used + aa.soff[i]);
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], r11\n", fn->params[i]->slot);
            } else {
                buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], %s\n", fn->params[i]->slot,
                           aa.reg[i]);
            }
        }
        for (int i = 0; i < fn->nparams; i++) {
            /* A captured parameter: the slot must hold the box, not the value, or
             * a closure reading the environment would find an integer where it
             * expects a pointer. Reading the spilled slot rather than the incoming
             * register is also what lets this be a second pass at all.
             *
             * The selection matches the one pass this replaced, branch for branch:
             * a float was stored raw there too, and a parameter that arrived on the
             * stack rather than in a register was never boxed. Which parameters are
             * boxed is a separate question from spilling them safely. */
            if (!fn->params[i]->boxed)
                continue;
            if (is_kind(fn->params[i]->type, TK_F64))
                continue;
            if (aa.reg[i] == NULL)
                continue;
            buf_printf(cg->out, "  mov rdi, QWORD PTR [rbp - %d]\n", fn->params[i]->slot);
            buf_printf(cg->out, "  call z_box\n");
            buf_printf(cg->out, "  mov QWORD PTR [rbp - %d], rax\n", fn->params[i]->slot);
        }
    }
    gen_block_items(cg, fn->fbody);
    /* `int main` that runs off its closing brace returns 0, as C defines. The
     * parser records the fall-through and exempts main from the missing-return
     * error, so this is the one function whose fall-off leaves a defined value:
     * without it the process exited with whatever the last call happened to
     * leave in rax. */
    if (fn->falls_off)
        buf_puts(cg->out, "  xor eax, eax\n");
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
