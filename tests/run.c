/* Golden + diagnostic test runner for the Z compiler.
 *
 * Each tests/cases/<name>.z is compiled and run; its stdout must match the
 * sibling <name>.expected byte for byte. Each tests/errors/<name>.z must fail
 * to compile, and every non-empty line in the sibling <name>.expected must
 * appear in the compiler's stderr (a substring check, so we are not brittle
 * about caret rendering or exact column numbers). */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char buf[1 << 20];

/* `z asm` output for one file. The standard library is spliced into every
 * compilation unit, so this is much larger than a diagnostic would be. File
 * scope because a megabyte on the stack is not something to add to main's
 * frame. */
static char asm_buf[1 << 20];

static int slurp(const char *path, char *out, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int n = (int)fread(out, 1, (size_t)cap - 1, f);
    out[n] = 0;
    fclose(f);
    return 1;
}

/* When Z_TEST_OPT is set (e.g. -O2) every compiler invocation in this harness
 * is run at that level, so the whole suite can be checked against each rung of
 * the optimization ladder. */
static const char *opt_flag(void) {
    const char *o = getenv("Z_TEST_OPT");
    return (o != NULL && *o != '\0') ? o : "";
}

static int run_cmd_capture(const char *cmd, char *out, int cap) {
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    int n = (int)fread(out, 1, (size_t)cap - 1, p);
    out[n] = 0;
    /* fread stops at the buffer limit, but the child is still writing. Draining
     * the rest of the pipe is what keeps it from taking SIGPIPE, which would
     * otherwise surface as a nonzero exit and a spurious "compile failed" for a
     * build that succeeded. A test that emits more output than `cap` (one with
     * hundreds of warnings, say) is exactly that case. */
    int c;
    while ((c = fgetc(p)) != EOF)
        ;
    int status = pclose(p);
    if (status == -1)
        return -1;
    return (status >> 8) & 0xff;
}

int main(int argc, char **argv) {
    (void)argc;
    const char *zc = argv[1];
    const char *case_dir = "tests/cases";
    const char *err_dir = "tests/errors";
    const char *rt_dir = "tests/runtime";
    const char *io_dir = "tests/interop";
    const char *imp_dir = "tests/imports";
    const char *warn_dir = "tests/warnings";
    int pass = 0, fail = 0;

    /* Golden cases. */
    char path[512], exp_path[512], cmd[2048];
    static const char *cases[] = {
        "hello",
        "arith",
        "vars",
        "bools",
        "ifelse",
        "loops",
        "funcs",
        "recursion",
        "strings",
        "compound",
        "mainfunc",
        "nested_calls",
        "manyargs",
        "scopes",
        "shadows",
        "mainret",
        "forloop",
        "pointers",
        "arrays",
        "foreach",
        "strconcat",
        "structs",
        "structs_nested",
        "interp",
        "methods",
        "props",
        "ext",
        "fatarrow",
        "opoverload",
        "tern",
        "ownership",
        "files",
        "strings_owned",
        "temporaries",
        "enum_match",
        "match_arms",
        "constfold",
        "regalloc",
        "divmod",
        "boolstr",
        "generics",
        "generics2",
        "classes",
        "classes2",
        "integration",
        "strings2",
        "collections",
        "collections2",
        "frame_layout",
        "bitwise",
        "consts",
        "breakcontinue",
        "nested_loops",
        "null",
        "strcmp",
        "intrinsics",
        "trig",
        "symnames",
        "bigconst",
        "fnptr",
        "methodptr",
        "literals",
        "stdlib",
        "floats",
        "closures",
        "closures_toplevel",
        "nested_fn",
        "typed_locals",
        "local_types",
        "unroll",
        "unroll_dep",
        "onearg_call",
        "durations",
        "stackargs",
        "closedform",
        "licm_bigframe",
        "inliner",
        "result",
        "interfaces",
        "cxxsyntax",
        "rangefor",
        "addrof_field",
        "destructors",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        snprintf(path, sizeof path, "%s/%s.z", case_dir, cases[i]);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", case_dir, cases[i]);
        if (!slurp(path, buf, sizeof buf)) {
            fprintf(stderr, "FAIL %s (missing test file)\n", cases[i]);
            fail++;
            continue;
        }
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing .expected)\n", cases[i]);
            fail++;
            continue;
        }
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin %s 2>&1", zc, path, opt_flag());
        char build_err[1 << 16];
        int rc = run_cmd_capture(cmd, build_err, sizeof build_err);
        if (rc != 0) {
            fprintf(stderr, "FAIL %s (compile failed)\n%s\n", cases[i], build_err);
            fail++;
            continue;
        }
        char actual[1 << 16];
        int prc = run_cmd_capture("/tmp/z_test_bin", actual, sizeof actual);
        (void)prc; /* the program's own exit code is not part of golden output */
        if (strcmp(expected, actual) != 0) {
            fprintf(stderr, "FAIL %s (output mismatch)\n--- expected ---\n%s--- actual ---\n%s\n",
                    cases[i], expected, actual);
            fail++;
        } else {
            printf("ok   %s\n", cases[i]);
            pass++;
        }
    }

    /* Diagnostic cases. */
    static const char *errs[] = {
        "undefined_var",
        "type_mismatch",
        "missing_semi",
        "unknown_func",
        "bad_condition",
        "arity",
        "unterminated_string",
        "stray_char",
        "return_type",
        "undefined_type",
        "too_many_params",
        "too_many_method_params",
        "bad_const",
        "dup_const",
        "bitwise_on_string",
        "break_outside_loop",
        "continue_outside_loop",
        "intrinsic_arity",
        "intrinsic_arg_type",
        "compare_mixed",
        "extern_with_body",
        "extern_arity",
        "extern_argtype",
        "export_no_body",
        "fnptr_argcount",
        "fnptr_argtype",
        "fnptr_signature",
        "fnptr_order",
        "fnptr_arity_mismatch",
        "methodptr_struct_recv",
        "methodptr_no_method",
        "methodptr_struct_return",
        "foreach_ptr",
        "foreach_nonarray",
        "bad_escape",
        "int_overflow",
        "empty_radix_literal",
        "digits_then_letters",
        "float_to_int",
        "float_mod",
        "float_bitwise",
        "float_not",
        "float_shift",
        "arrow_on_nonptr",
        "rangefor_wrongtype",
        "rangefor_mismatch",
        "inheritance",
        "use_after_move",
        "move_non_value",
        "base_call",
        "lambda_return_type",
        "lambda_not_int",
        "lambda_fn_return",
        "lambda_in_lambda",
        "nested_fn_capture",
        "semicolon_at_eof",
        "hold_arity",
        "hold_argtype",
        "fn_reads_toplevel_var",
        "nested_fn_extern",
        "nested_fn_no_body",
        "result_mismatch",
        "result_no_ret",
        "result_ambiguous",
        "result_too_big",
        "iface_missing_method",
        "iface_bad_signature",
        "iface_nonvirtual",
        "iface_no_method",
        "suggest_var",
        "suggest_method",
        "suggest_type",
        "nonexhaustive_names",
        "enclosing_fn",
        "sig_note",
        "argtype_note",
    };
    for (size_t i = 0; i < sizeof(errs) / sizeof(*errs); i++) {
        snprintf(path, sizeof path, "%s/%s.z", err_dir, errs[i]);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", err_dir, errs[i]);
        if (!slurp(path, buf, sizeof buf)) {
            fprintf(stderr, "FAIL %s (missing test file)\n", errs[i]);
            fail++;
            continue;
        }
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing .expected)\n", errs[i]);
            fail++;
            continue;
        }
        /* Send the output somewhere outside the tree: without -o the compiler
         * writes an executable named after the test into the repo root. */
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin %s 2>&1", zc, path, opt_flag());
        char actual[1 << 16];
        int rc = run_cmd_capture(cmd, actual, sizeof actual);
        if (rc == 0) {
            fprintf(stderr, "FAIL %s (expected compile error, got success)\n", errs[i]);
            fail++;
            continue;
        }
        int ok = 1;
        char *save = NULL;
        for (char *line = strtok_r(expected, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == 0)
                continue;
            if (!strstr(actual, line)) {
                fprintf(stderr, "FAIL %s (missing expected diagnostic: %s)\n--- actual ---\n%s\n",
                        errs[i], line, actual);
                ok = 0;
                break;
            }
        }
        if (ok) {
            printf("ok   %s (diagnostic)\n", errs[i]);
            pass++;
        } else {
            fail++;
        }
    }

    /* Warning cases. A warning is not an error, so these must compile
     * successfully while their .expected lines all appear in the output. A
     * missing line is a regression; an extra one usually means the compiler is
     * warning about its own desugarings, which `clean` catches. */
    static const char *warns[] = {"all"};
    for (size_t i = 0; i < sizeof warns / sizeof(*warns); i++) {
        snprintf(path, sizeof path, "%s/%s.z", warn_dir, warns[i]);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", warn_dir, warns[i]);
        if (!slurp(path, buf, sizeof buf)) {
            fprintf(stderr, "FAIL %s (missing test file)\n", warns[i]);
            fail++;
            continue;
        }
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing .expected)\n", warns[i]);
            fail++;
            continue;
        }
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin %s 2>&1", zc, path, opt_flag());
        char actual[1 << 16];
        int rc = run_cmd_capture(cmd, actual, sizeof actual);
        if (rc != 0) {
            fprintf(stderr, "FAIL %s (warnings must not fail the build)\n%s\n", warns[i], actual);
            fail++;
            continue;
        }
        int ok = 1;
        char *save = NULL;
        for (char *line = strtok_r(expected, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == 0)
                continue;
            if (!strstr(actual, line)) {
                fprintf(stderr, "FAIL %s (missing expected warning: %s)\n--- actual ---\n%s\n",
                        warns[i], line, actual);
                ok = 0;
                break;
            }
        }
        if (ok) {
            printf("ok   %s (warning)\n", warns[i]);
            pass++;
        } else {
            fail++;
        }
    }

    /* Debug info. `z -g` emits DWARF that no reader here can fully check, but
     * the properties that are easy to get wrong are all checkable with
     * readelf: the sections exist, the unit is well-formed enough not to warn,
     * every function got a DIE with a real address range, and the line table
     * still runs to the end of each function. A break in the encoding shows up
     * here as a "Corrupt unit length" or a missing section rather than as a
     * silently unusable binary. */
    {
        snprintf(path, sizeof path, "%s/debuginfo.z", case_dir);
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_dbg %s -g 2>&1", zc, path,
                 opt_flag());
        char actual[1 << 16];
        int rc = run_cmd_capture(cmd, actual, sizeof actual);
        if (rc != 0) {
            fprintf(stderr, "FAIL debuginfo (build with -g failed)\n%s\n", actual);
            fail++;
        } else {
            static const struct {
                const char *label;
                const char *readelf;
                const char *must_contain;
            } checks[] = {
                {"has .debug_info", "readelf -S /tmp/z_test_dbg", ".debug_info"},
                {"has .debug_line", "readelf -S /tmp/z_test_dbg", ".debug_line"},
                {"has .debug_abbrev", "readelf -S /tmp/z_test_dbg", ".debug_abbrev"},
                {"unit is well-formed", "readelf --debug-dump=info /tmp/z_test_dbg",
                 "DW_TAG_compile_unit"},
                {"names a function", "readelf --debug-dump=info /tmp/z_test_dbg",
                 "DW_TAG_subprogram"},
                {"locates a function", "readelf --debug-dump=info /tmp/z_test_dbg", "DW_AT_low_pc"},
                {"has a line table", "objdump --dwarf=decodedline /tmp/z_test_dbg", "line"},
            };
            int ok = 1;
            for (size_t i = 0; i < sizeof checks / sizeof(*checks); i++) {
                char out[1 << 16];
                snprintf(cmd, sizeof cmd, "%s 2>&1", checks[i].readelf);
                run_cmd_capture(cmd, out, sizeof out);
                if (!strstr(out, checks[i].must_contain)) {
                    fprintf(stderr, "FAIL debuginfo (%s)\n", checks[i].label);
                    ok = 0;
                    break;
                }
                /* A malformed unit makes readelf complain before it prints
                 * anything useful, so the absence of a complaint is the check. */
                if (strstr(out, "Corrupt") || strstr(out, "Malformed")) {
                    fprintf(stderr, "FAIL debuginfo (%s): reader rejected the unit\n%s\n",
                            checks[i].label, out);
                    ok = 0;
                    break;
                }
            }
            /* -g must not change what the program does. */
            if (ok) {
                char out[1 << 16];
                run_cmd_capture("/tmp/z_test_dbg", out, sizeof out);
                if (strstr(out, "debug info: ok") == NULL) {
                    fprintf(stderr, "FAIL debuginfo (-g changed the program's output)\n%s\n", out);
                    ok = 0;
                }
            }
            /* A local must be nameable at every level.
             *
             * It used to be dropped whenever the allocator gave it a register,
             * which meant no locals at all in an optimised build -- the case -g
             * exists for. A promoted local is now described at its register, and
             * one in its frame slot at rbp minus the slot, so both are present and
             * this check no longer has to be pinned to a level. */
            if (ok) {
                char out[1 << 16];
                run_cmd_capture("readelf --debug-dump=info /tmp/z_test_dbg 2>&1", out, sizeof out);
                const char *lvl = opt_flag()[0] ? opt_flag() : "the default level";
                if (strstr(out, "sum") == NULL) {
                    fprintf(stderr,
                            "FAIL debuginfo (a local is missing from the debug info at %s)\n", lvl);
                    ok = 0;
                } else if (strcmp(opt_flag(), "-O0") != 0 && strstr(out, "DW_OP_reg") == NULL) {
                    fprintf(stderr, "FAIL debuginfo (no local is described at a register at %s)\n",
                            lvl);
                    ok = 0;
                }
            }
            if (ok) {
                printf("ok   debuginfo (DWARF)\n");
                pass++;
            } else {
                fail++;
            }
        }
    }

    /* A file with nothing to complain about must produce no warning at all.
     * This is the guard on the compiler's own machinery: a desugared loop
     * temporary or a hidden result buffer showing up here means a binding the
     * user never wrote is being reported as their code. */
    {
        snprintf(path, sizeof path, "%s/clean.z", warn_dir);
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin %s 2>&1", zc, path, opt_flag());
        char actual[1 << 16];
        int rc = run_cmd_capture(cmd, actual, sizeof actual);
        if (rc != 0) {
            fprintf(stderr, "FAIL clean (compile failed)\n%s\n", actual);
            fail++;
        } else if (strstr(actual, "warning:")) {
            fprintf(stderr, "FAIL clean (unexpected warning)\n%s\n", actual);
            fail++;
        } else {
            printf("ok   clean (no warnings)\n");
            pass++;
        }
    }

    /* The warning flags have to actually do something: -w silences everything,
     * -Wno-<name> silences one, and -Werror turns a warning into a failed
     * build. Each is checked by compiling the same file and looking at both the
     * output and the exit status. */
    {
        static const struct {
            const char *label;
            const char *flag;
            int want_rc;      /* 0 = compiles, nonzero = fails */
            int want_warning; /* whether "warning:" may appear */
        } flagcases[] = {
            {"-w silences all", "-w", 0, 0},
            {"-Wno-unused-local silences one", "-Wno-unused-local", 0, 1},
            {"-Wshadowed-local still reports the rest", "-Wshadowed-local", 0, 1},
            /* -Werror relabels, so "warning:" must be gone and "error:" present. */
            {"-Werror fails the build", "-Werror", 1, 0},
        };
        snprintf(path, sizeof path, "%s/all.z", warn_dir);
        for (size_t i = 0; i < sizeof flagcases / sizeof(*flagcases); i++) {
            snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin %s %s 2>&1", zc, path,
                     opt_flag(), flagcases[i].flag);
            char actual[1 << 16];
            int rc = run_cmd_capture(cmd, actual, sizeof actual);
            int got_warning = strstr(actual, "warning:") != NULL;
            int got_error = strstr(actual, "error:") != NULL;
            if ((rc != 0) != flagcases[i].want_rc || got_warning != flagcases[i].want_warning ||
                /* -Werror must relabel, not duplicate. */
                (flagcases[i].want_rc && !got_error)) {
                fprintf(stderr, "FAIL warnflag %s (rc=%d warning=%d error=%d)\n%s\n",
                        flagcases[i].label, rc, got_warning, got_error, actual);
                fail++;
            } else {
                printf("ok   warnflag %s\n", flagcases[i].label);
                pass++;
            }
        }
    }

    /* Diagnostic output shape. The same file is compiled three ways and each
     * rendering is checked for the properties a consumer depends on: `gcc` is
     * one line per problem and per note and nothing else, `json` is one object
     * per line carrying a stable code and an explicit span, and `--color=never`
     * puts no escape sequence in the output when it is not wanted. */
    {
        static const struct {
            const char *label;
            const char *flag;
            const char *must_contain;
            const char *must_not_contain;
        } shape[] = {
            {"gcc is one line per problem", "--error-format=gcc", "note: did you mean", NULL},
            {"gcc drops the source frame", "--error-format=gcc", "undefined variable", "|"},
            {"json carries a code and a span", "--error-format=json",
             "\"code\":\"undefined_variable\"", "\n  "},
            {"json puts the note inside the object", "--error-format=json",
             "\"notes\":[{\"message\":\"did you mean", NULL},
            {"color=never emits no escape codes", "--color=never", "undefined variable", "\033["},
            {"color=always emits them", "--color=always", "\033[", NULL},
            {"an unknown format is a usage error", "--error-format=nope", "unknown --error-format",
             NULL},
        };
        snprintf(path, sizeof path, "%s/suggest_var.z", err_dir);
        for (size_t i = 0; i < sizeof shape / sizeof(*shape); i++) {
            snprintf(cmd, sizeof cmd, "%s build %s -o /dev/null %s %s 2>&1", zc, path, opt_flag(),
                     shape[i].flag);
            char actual[1 << 16];
            int rc = run_cmd_capture(cmd, actual, sizeof actual);
            int ok = strstr(actual, shape[i].must_contain) != NULL;
            if (shape[i].must_not_contain != NULL &&
                strstr(actual, shape[i].must_not_contain) != NULL)
                ok = 0;
            /* A bad --error-format value is a usage error (2), not a compile
             * error (1), because nothing was compiled. */
            int want_rc = strstr(shape[i].label, "usage error") != NULL ? 2 : 1;
            if (!ok || rc != want_rc) {
                fprintf(stderr, "FAIL diagfmt %s (rc=%d)\n--- actual ---\n%s\n", shape[i].label, rc,
                        actual);
                fail++;
            } else {
                printf("ok   diagfmt %s\n", shape[i].label);
                pass++;
            }
        }
    }

    /* Runtime-abort cases: compiled with --bounds, the program must exit
     * nonzero and every non-empty line of the sibling .expected must appear in
     * its combined output. */
    static const char *aborts[] = {
        "bounds_read", "bounds_write", "bounds_negative", "bounds_runtime_len", "bounds_string",
    };
    for (size_t i = 0; i < sizeof aborts / sizeof aborts[0]; i++) {
        snprintf(path, sizeof path, "%s/%s.z", rt_dir, aborts[i]);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", rt_dir, aborts[i]);
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing .expected)\n", aborts[i]);
            fail++;
            continue;
        }
        snprintf(cmd, sizeof cmd, "./%s run %s %s --bounds 2>&1", zc, path, opt_flag());
        char actual[1 << 16];
        int rc = run_cmd_capture(cmd, actual, sizeof actual);
        if (rc == 0) {
            fprintf(stderr, "FAIL %s (expected a runtime abort, got success)\n", aborts[i]);
            fail++;
            continue;
        }
        int ok = 1;
        char *save = NULL;
        for (char *line = strtok_r(expected, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == 0)
                continue;
            if (!strstr(actual, line)) {
                fprintf(stderr, "FAIL %s (missing %s)\n--- actual ---\n%s\n", aborts[i], line,
                        actual);
                ok = 0;
                break;
            }
        }
        if (ok) {
            printf("ok   %s (runtime)\n", aborts[i]);
            pass++;
        } else {
            fail++;
        }
    }

    /* Interop cases: a Z program plus the C file that satisfies its extern
     * declarations, passed on the command line so the link step sees it. The
     * sibling <name>.c is the C side. */
    static const char *interop[] = {
        "extern",
        "export",
    };
    for (size_t i = 0; i < sizeof interop / sizeof interop[0]; i++) {
        snprintf(path, sizeof path, "%s/%s.z", io_dir, interop[i]);
        /* The C side is <name>.c when it exists, else the shared helper.c. */
        snprintf(cmd, sizeof cmd, "%s/%s.c", io_dir, interop[i]);
        if (access(cmd, R_OK) != 0)
            snprintf(cmd, sizeof cmd, "%s/helper.c", io_dir);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", io_dir, interop[i]);
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing .expected)\n", interop[i]);
            fail++;
            continue;
        }
        char link[4096];
        snprintf(link, sizeof link, "./%s run %s %s 2>&1", zc, path, cmd);
        char actual[1 << 16];
        if (run_cmd_capture(link, actual, sizeof actual) != 0 || strcmp(expected, actual) != 0) {
            fprintf(stderr, "FAIL %s (output mismatch)\n--- expected ---\n%s--- actual ---\n%s\n",
                    interop[i], expected, actual);
            fail++;
            continue;
        }
        printf("ok   %s (interop)\n", interop[i]);
        pass++;
    }

    /* Module cases: the harness runs a program that pulls in sibling files, so
     * the import graph is exercised end to end. */
    static const char *modules[] = {
        "import_main",
    };
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++) {
        snprintf(path, sizeof path, "%s/%s.z", imp_dir, modules[i]);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", imp_dir, modules[i]);
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing .expected)\n", modules[i]);
            fail++;
            continue;
        }
        snprintf(cmd, sizeof cmd, "./%s run %s %s 2>&1", zc, path, opt_flag());
        char actual[1 << 16];
        if (run_cmd_capture(cmd, actual, sizeof actual) != 0 || strcmp(expected, actual) != 0) {
            fprintf(stderr, "FAIL %s (output mismatch)\n--- expected ---\n%s--- actual ---\n%s\n",
                    modules[i], expected, actual);
            fail++;
            continue;
        }
        printf("ok   %s (modules)\n", modules[i]);
        pass++;
    }

    /* The -O3 unroll gate.
     *
     * The gate decides which loops get four copies of their body, and no golden
     * output can see that decision: a loop that unrolls has to produce exactly
     * the same answer as one that does not, which is the property the `unroll`
     * and `unroll_dep` cases check. So this reads the emitted assembly instead
     * and compares the instruction count at -O2 against -O3. A body the gate
     * leaves rolled emits the same instructions at both levels; a body it
     * unrolls emits three more copies of itself. Counting instructions rather
     * than comparing text is what makes this robust to label renumbering, and
     * unlike a timing check it cannot flake.
     *
     * Each sibling <name>.expected holds the word `rolled` or `unrolled`. */
    {
        const char *gate_dir = "tests/unroll";
        char expected[1 << 16];
        char *save = NULL;
        static const char *gates[] = {
            "one_chain",    "induction_is_not_a_chain",
            "two_chains",   "three_chains",
            "store_only",   "opaque_call",
            "opaque_store", "big_body",
            "no_condition", "nested_loop_is_opaque",
        };
        for (size_t i = 0; i < sizeof gates / sizeof(*gates); i++) {
            snprintf(path, sizeof path, "%s/%s.z", gate_dir, gates[i]);
            snprintf(exp_path, sizeof exp_path, "%s/%s.expected", gate_dir, gates[i]);
            if (!slurp(exp_path, expected, sizeof expected)) {
                fprintf(stderr, "FAIL unrollgate %s (missing .expected)\n", gates[i]);
                fail++;
                continue;
            }
            expected[strcspn(expected, "\n")] = 0;
            /* Always -O2 against -O3, and never the level the suite is running
             * at. The gate only exists at -O3, so its decisions are the same at
             * every rung of the ladder: comparing -O0 against -O2 here would call
             * every probe unrolled, because -O0 emits more instructions for
             * reasons that have nothing to do with this pass. */
            static const char *levels[2] = {"-O2", "-O3"};
            int insns[2] = {-1, -1};
            for (int lvl = 0; lvl < 2; lvl++) {
                snprintf(cmd, sizeof cmd, "%s asm %s %s 2>&1", zc, path, levels[lvl]);
                if (run_cmd_capture(cmd, asm_buf, sizeof asm_buf) != 0) {
                    fprintf(stderr, "FAIL unrollgate %s (asm failed)\n%s\n", gates[i], asm_buf);
                    insns[0] = insns[1] = -1;
                    break;
                }
                int n = 0;
                /* The rule tools/bench.py counts by: a directive or a label is
                 * not an instruction. */
                for (char *line = strtok_r(asm_buf, "\n", &save); line;
                     line = strtok_r(NULL, "\n", &save)) {
                    char *t = line;
                    while (*t == ' ' || *t == '\t')
                        t++;
                    size_t len = strlen(t);
                    if (len == 0 || t[0] == '.' || t[len - 1] == ':' || t[0] == '#')
                        continue;
                    n++;
                }
                insns[lvl] = n;
            }
            if (insns[0] < 0 || insns[1] < 0)
                continue;
            int got = insns[1] > insns[0];
            int want = strcmp(expected, "unrolled") == 0;
            if (got != want) {
                fprintf(stderr, "FAIL unrollgate %s (expected %s, -O2=%d -O3=%d insns)\n", gates[i],
                        expected, insns[0], insns[1]);
                fail++;
            } else {
                printf("ok   unrollgate %s (%s, %d insns)\n", gates[i], expected,
                       got ? insns[1] : insns[0]);
                pass++;
            }
        }
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
