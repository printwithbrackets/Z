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
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

/* Quotes a value for the harness's own shell. The driver tests hand the
 * compiler paths containing an apostrophe and a semicolon, and those have to
 * arrive as one argument rather than as shell syntax, so the harness cannot
 * paste them into a command string raw. An embedded apostrophe ends the quoted
 * run, contributes an escaped one, and opens a new run. */
static void shell_quote(const char *s, char *out, int cap) {
    int n = 0;
    if (cap < 4) {
        if (cap > 0)
            out[0] = '\0';
        return;
    }
    out[n++] = '\'';
    /* The bound leaves room for the closing quote and the terminator even when
     * the loop stops on it rather than on the end of the string. */
    for (; *s != '\0' && n + 8 < cap; s++) {
        if (*s == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *s;
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
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
    /* A child killed by a signal is not an exit code, and folding the raw wait
     * status into `(status >> 8) & 0xff` turns a crash into a success: a
     * program that died on SIGSEGV reads as 0 here. That is why a golden case
     * could segfault and still be reported `ok`, because its output matched and
     * the status was never consulted.
     *
     * Reported the way a shell reports it, 128 plus the signal, so a caller that
     * already checks for nonzero sees the crash without having to ask again. */
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return (status >> 8) & 0xff;
}

/* Whether AddressSanitizer can be built into and run from a program here.
 *
 * Probed rather than assumed, because the leak cases are the only ones that need
 * it and a toolchain without it should report them as skipped rather than fail
 * the suite for a reason that has nothing to do with the compiler.
 *
 * The probe deliberately leaks a string, so a build where the flag was accepted
 * but silently ignored does not read as available: the run has to produce a
 * LeakSanitizer report, which means the instrumentation is linked, the leak
 * detector is on, and this process can execute the result. */
/* Takes the quoted path, not the raw one, so it builds its command the same way
 * every other group does. */
static int asan_available(const char *zq) {
    char src[256], bin[256], cmd[2048], out[1 << 16];
    snprintf(src, sizeof src, "/tmp/z_asan_probe_%ld.z", (long)getpid());
    snprintf(bin, sizeof bin, "/tmp/z_asan_probe_%ld", (long)getpid());
    FILE *f = fopen(src, "w");
    if (f == NULL)
        return 0;
    /* The temporary in the return expression is never released, so the string
     * allocator shows up in the leak report if and only if it is instrumented.
     * A temporary that is stored into a local would not do: the compiler
     * releases that one when the scope ends. */
    fputs("int main() { return len(\"probe \" + int_to_string(42)) > 0 ? 0 : 1; }\n", f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "%s build %s -o %s -fsanitize=address -g >/dev/null 2>&1", zq, src,
             bin);
    int ok = run_cmd_capture(cmd, out, sizeof out) == 0;
    if (ok) {
        /* LeakSanitizer makes the process exit nonzero when it reports, so the
         * status says nothing here and the report is the assertion. */
        snprintf(cmd, sizeof cmd, "ASAN_OPTIONS=detect_leaks=1 %s 2>&1", bin);
        run_cmd_capture(cmd, out, sizeof out);
        ok = strstr(out, "LeakSanitizer") != NULL && strstr(out, "zstr_alloc_impl") != NULL;
    }
    remove(src);
    remove(bin);
    return ok;
}

/* The two artifacts every group builds.
 *
 * Suffixed with the pid because the names used to be fixed. Two suites running at
 * once, which is what `make test` in one shell and `make test-all` in another is,
 * both write /tmp/z_test_bin, so whichever got there second replaced the binary
 * the first was halfway through running. The symptom is a golden case failing
 * with output from a different program, which reads as a compiler bug and is not
 * one. The names stay predictable so `make clean` can remove them. */
static char test_bin[128];
static char test_dbg[128];

/* The compiler path, quoted once.
 *
 * Every command this harness builds goes through a shell, because it pipes and
 * redirects, so the path has to survive one. `shell_quote` is the helper for
 * that and it was applied in exactly one place, so a path with a space or an
 * apostrophe in it worked there and nowhere else.
 *
 * Quoting it once here rather than at seventeen call sites is what keeps the
 * seventeen from disagreeing. It is also why nothing prepends `./`: that turned
 * an absolute path into `.//home/.../z`, which does not exist, so passing one
 * broke the runtime-abort, interop and module groups and nothing else. The path
 * is used verbatim instead, which works for a relative path and an absolute one
 * alike. */
/* Sized to leave room inside the command buffers this harness builds, which are
 * 2 KiB each. A longer path would be truncated, and `shell_quote` closes its
 * quote when it stops, so that produces a command naming a file that does not
 * exist rather than anything unsafe. No real path is close. */
static char zq[1024];

int main(int argc, char **argv) {
    (void)argc;
    const char *zc = argv[1];
    shell_quote(zc, zq, (int)sizeof zq);
    snprintf(test_bin, sizeof test_bin, "/tmp/z_test_bin_%ld", (long)getpid());
    snprintf(test_dbg, sizeof test_dbg, "/tmp/z_test_dbg_%ld", (long)getpid());
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
        "hello",         "arith",          "vars",         "bools",       "ifelse",
        "loops",         "funcs",          "recursion",    "strings",     "compound",
        "mainfunc",      "nested_calls",   "manyargs",     "scopes",      "shadows",
        "forloop",       "pointers",       "arrays",       "foreach",     "strconcat",
        "structs",       "structs_nested", "interp",       "methods",     "props",
        "ext",           "fatarrow",       "opoverload",   "tern",        "ownership",
        "files",         "strings_owned",  "temporaries",  "enum_match",  "match_arms",
        "constfold",     "regalloc",       "divmod",       "boolstr",     "generics",
        "generics2",     "classes",        "classes2",     "integration", "strings2",
        "collections",   "collections2",   "frame_layout", "bitwise",     "consts",
        "breakcontinue", "nested_loops",   "null",         "strcmp",      "intrinsics",
        "trig",          "symnames",       "bigconst",     "fnptr",       "methodptr",
        "literals",      "stdlib",         "floats",       "closures",    "closures_toplevel",
        "nested_fn",     "typed_locals",   "local_types",  "unroll",      "unroll_dep",
        "onearg_call",   "durations",      "stackargs",    "closedform",  "licm_bigframe",
        "inliner",       "result",         "interfaces",   "cxxsyntax",   "rangefor",
        "addrof_field",  "destructors",    "strreturn",    "stdin",       "surface",
        "typeof",        "vcall_struct",
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
        /* A case with a sibling <name>.dialect is compiled in that dialect, named
         * with --surface. Without this the manifest would have to sit beside the
         * source, and one beside a whole directory would apply to every case in
         * it -- which is a shared global setting a test suite cannot have. */
        char dialect_path[512];
        snprintf(dialect_path, sizeof dialect_path, "%s/%s.dialect", case_dir, cases[i]);
        char dialect_flag[600] = "";
        if (access(dialect_path, R_OK) == 0)
            snprintf(dialect_flag, sizeof dialect_flag, "--surface=%s", dialect_path);
        snprintf(cmd, sizeof cmd, "%s build %s -o %s %s %s 2>&1", zq, path, test_bin, dialect_flag,
                 opt_flag());
        char build_err[1 << 16];
        int rc = run_cmd_capture(cmd, build_err, sizeof build_err);
        if (rc != 0) {
            fprintf(stderr, "FAIL %s (compile failed)\n%s\n", cases[i], build_err);
            fail++;
            continue;
        }
        char actual[1 << 16];
        /* A case whose program reads stdin gets it from a sibling <name>.stdin,
         * since popen hands the child whatever this process has and there is no
         * terminal here to be typed at. A case with no such sibling runs as
         * before, so nothing else changes. */
        char stdin_path[512];
        snprintf(stdin_path, sizeof stdin_path, "%s/%s.stdin", case_dir, cases[i]);
        char run_cmd[1024];
        if (access(stdin_path, R_OK) == 0)
            snprintf(run_cmd, sizeof run_cmd, "%s < %s", test_bin, stdin_path);
        else
            snprintf(run_cmd, sizeof run_cmd, "%s", test_bin);
        int prc = run_cmd_capture(run_cmd, actual, sizeof actual);
        /* The program's own exit status is asserted, not just its output. A case
         * that printed the right thing and then died is a failing case, and
         * until this was checked nothing in the suite could tell the difference
         * between a program that finished and a program that crashed halfway
         * through printing.
         *
         * 139 is SIGSEGV and 134 is SIGABRT, so the two common ones read as
         * themselves rather than as a generic failure. */
        if (prc != 0) {
            fprintf(stderr, "FAIL %s (exited %d)\n--- expected ---\n%s--- actual ---\n%s\n",
                    cases[i], prc, expected, actual);
            fail++;
            continue;
        }
        if (strcmp(expected, actual) != 0) {
            fprintf(stderr, "FAIL %s (output mismatch)\n--- expected ---\n%s--- actual ---\n%s\n",
                    cases[i], expected, actual);
            fail++;
        } else {
            printf("ok   %s\n", cases[i]);
            pass++;
        }
    }

    /* Exit-status cases: the sibling .expected holds the expected exit code on
     * its first line and the expected stdout below it.
     *
     * A separate group because the golden cases all require status zero, and
     * these are the two things that cannot be expressed as "printed the right
     * thing and finished". `mainret` returns a value out of main, which is a
     * thing the language has to get right. `stack_overflow` dies on SIGSEGV,
     * which is the case that made the golden group unsafe: it printed exactly
     * what its golden file said, and the old harness reported it `ok`, because
     * the status was never read. Keeping it here rather than deleting it is what
     * stops that fix from being undone silently, because a harness that folded a
     * signal back into a success would fail this case and nothing else. */
    static const char *exits[] = {"mainret", "stack_overflow"};
    const char *ex_dir = "tests/exits";
    char exit_out[256];
    for (size_t i = 0; i < sizeof exits / sizeof *exits; i++) {
        snprintf(path, sizeof path, "%s/%s.z", ex_dir, exits[i]);
        snprintf(exp_path, sizeof exp_path, "%s/%s.expected", ex_dir, exits[i]);
        char expected[1 << 16];
        if (!slurp(path, buf, sizeof buf) || !slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL %s (missing test file or .expected)\n", exits[i]);
            fail++;
            continue;
        }
        snprintf(exit_out, sizeof exit_out, "/tmp/z_exit_%ld_%s", (long)getpid(), exits[i]);
        snprintf(cmd, sizeof cmd, "%s build %s -o %s %s 2>&1", zq, path, exit_out, opt_flag());
        char build_err[1 << 16];
        if (run_cmd_capture(cmd, build_err, sizeof build_err) != 0) {
            fprintf(stderr, "FAIL %s (compile failed)\n%s\n", exits[i], build_err);
            fail++;
            continue;
        }
        /* The first line is the code, and the rest is the output. Split on the
         * first newline rather than the last, so the code is the one number the
         * case is about. */
        char *nl = strchr(expected, '\n');
        if (nl == NULL) {
            fprintf(stderr, "FAIL %s (.expected has no exit code line)\n", exits[i]);
            fail++;
            continue;
        }
        int want = atoi(expected);
        char *want_out = nl + 1;
        char actual[1 << 16];
        snprintf(cmd, sizeof cmd, "%s 2>&1", exit_out);
        int got = run_cmd_capture(cmd, actual, sizeof actual);
        if (got != want) {
            fprintf(stderr, "FAIL %s (exited %d, expected %d)\n--- actual ---\n%s\n", exits[i], got,
                    want, actual);
            fail++;
            continue;
        }
        if (strcmp(want_out, actual) != 0) {
            fprintf(stderr, "FAIL %s (output mismatch)\n--- expected ---\n%s--- actual ---\n%s\n",
                    exits[i], want_out, actual);
            fail++;
            continue;
        }
        printf("ok   %s (exit %d)\n", exits[i], got);
        pass++;
        remove(exit_out);
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
        snprintf(cmd, sizeof cmd, "%s build %s -o %s %s 2>&1", zq, path, test_bin, opt_flag());
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
        snprintf(cmd, sizeof cmd, "%s build %s -o %s %s 2>&1", zq, path, test_bin, opt_flag());
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
        snprintf(cmd, sizeof cmd, "%s build %s -o %s %s -g 2>&1", zq, path, test_dbg, opt_flag());
        char actual[1 << 16];
        int rc = run_cmd_capture(cmd, actual, sizeof actual);
        if (rc != 0) {
            fprintf(stderr, "FAIL debuginfo (build with -g failed)\n%s\n", actual);
            fail++;
        } else {
            static const struct {
                const char *label;
                const char *reader;
                const char *args;
                const char *must_contain;
            } checks[] = {
                {"has .debug_info", "readelf", "-S", ".debug_info"},
                {"has .debug_line", "readelf", "-S", ".debug_line"},
                {"has .debug_abbrev", "readelf", "-S", ".debug_abbrev"},
                {"unit is well-formed", "readelf", "--debug-dump=info", "DW_TAG_compile_unit"},
                {"names a function", "readelf", "--debug-dump=info", "DW_TAG_subprogram"},
                {"locates a function", "readelf", "--debug-dump=info", "DW_AT_low_pc"},
                {"has a line table", "objdump", "--dwarf=decodedline", "line"},
            };
            int ok = 1;
            for (size_t i = 0; i < sizeof checks / sizeof(*checks); i++) {
                char out[1 << 16];
                snprintf(cmd, sizeof cmd, "%s %s %s 2>&1", checks[i].reader, checks[i].args,
                         test_dbg);
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
                snprintf(cmd, sizeof cmd, "%s 2>&1", test_dbg);
                run_cmd_capture(cmd, out, sizeof out);
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
                snprintf(cmd, sizeof cmd, "readelf --debug-dump=info %s 2>&1", test_dbg);
                run_cmd_capture(cmd, out, sizeof out);
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

    /* A wait has to actually wait, which is the one property of `hold` that no
     * golden output can show: a program that ignored its argument would print
     * exactly what the durations case prints and be otherwise indistinguishable
     * from one that waited.
     *
     * The bound is a lower one on purpose. The runtime promises never to return
     * early -- that is what resuming an interrupted nanosleep is for -- so
     * "at least this long" cannot be flaky however busy the machine is, while
     * "at most this long" could be. The upper bound is there only to catch a wait
     * that overshoots by orders of magnitude, and is loose enough not to care
     * about scheduling.
     *
     * The wait is sub-second because the fraction is the part with the edges: a
     * double converted to a whole count of seconds would turn 200ms into nothing,
     * and this is what would notice. */
    {
        static const char src[] = "int main() { hold(200ms); return 0; }\n";
        /* Pid-suffixed like every other path this harness builds. The binary
         * used to be a fixed name, so two suites running at once had one
         * replacing the file the other was about to exec, and the half-written
         * executable is what made the run fail -- which then reported itself as
         * a wait that returned after -1ms rather than as the collision it was. */
        char hold_z[128], hold_bin[128];
        snprintf(hold_z, sizeof hold_z, "/tmp/z_test_hold_%ld.z", (long)getpid());
        snprintf(hold_bin, sizeof hold_bin, "/tmp/z_test_hold_%ld", (long)getpid());
        snprintf(path, sizeof path, "%s", hold_z);
        FILE *hf = fopen(path, "wb");
        if (hf == NULL || fwrite(src, 1, sizeof src - 1, hf) != sizeof src - 1) {
            if (hf)
                fclose(hf);
            fprintf(stderr, "FAIL hold (cannot write the test source)\n");
            fail++;
        } else {
            fclose(hf);
            snprintf(cmd, sizeof cmd, "%s build %s -o %s %s 2>&1", zq, path, hold_bin, opt_flag());
            char actual[1 << 16];
            int rc = run_cmd_capture(cmd, actual, sizeof actual);
            struct timespec t0, t1;
            long long ms = -1;
            int run_rc = -1;
            if (rc == 0 && clock_gettime(CLOCK_MONOTONIC, &t0) == 0) {
                snprintf(cmd, sizeof cmd, "%s 2>&1", hold_bin);
                run_rc = run_cmd_capture(cmd, actual, sizeof actual);
                if (run_rc == 0 && clock_gettime(CLOCK_MONOTONIC, &t1) == 0)
                    ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 +
                         (t1.tv_nsec - t0.tv_nsec) / 1000000;
            }
            int ok = 1;
            if (rc != 0) {
                fprintf(stderr, "FAIL hold (build failed)\n%s\n", actual);
                ok = 0;
            } else if (run_rc != 0) {
                /* Reported as what it is. Folded into the timing arm it used to
                 * print "returned after -1ms", which reads as a wait that was too
                 * short rather than as a program that did not run. */
                fprintf(stderr, "FAIL hold (the program exited %d)\n%s\n", run_rc, actual);
                ok = 0;
            } else if (ms < 190) {
                fprintf(stderr,
                        "FAIL hold (200ms returned after %lldms: the fraction of a "
                        "duration was dropped)\n",
                        ms);
                ok = 0;
            } else if (ms > 3000) {
                fprintf(stderr, "FAIL hold (200ms took %lldms)\n", ms);
                ok = 0;
            }
            if (ok) {
                printf("ok   hold (waited %lldms for 200ms)\n", ms);
                pass++;
            } else {
                fail++;
            }
            remove(hold_z);
            remove(hold_bin);
        }
    }

    /* A file with nothing to complain about must produce no warning at all.
     * This is the guard on the compiler's own machinery: a desugared loop
     * temporary or a hidden result buffer showing up here means a binding the
     * user never wrote is being reported as their code. */
    {
        snprintf(path, sizeof path, "%s/clean.z", warn_dir);
        snprintf(cmd, sizeof cmd, "%s build %s -o %s %s 2>&1", zq, path, test_bin, opt_flag());
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
            snprintf(cmd, sizeof cmd, "%s build %s -o %s %s %s 2>&1", zq, path, test_bin,
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
            snprintf(cmd, sizeof cmd, "%s build %s -o /dev/null %s %s 2>&1", zq, path, opt_flag(),
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

    /* The output path and any extra linker arguments reach the C toolchain as
     * data, never as shell syntax.
     *
     * The driver used to interpolate both into one single-quoted shell command
     * string, so a value containing an apostrophe closed the quote and the rest
     * of the value was read as a command. Each is checked here on its own,
     * because one broken value makes the whole command a syntax error and hides
     * what the other one would have done.
     *
     * Both carry an apostrophe, a semicolon and a `touch` of a marker file that
     * nothing else removes. Read as data, the link succeeds, the executable
     * appears at exactly the path that was asked for and prints 42. Read as
     * shell syntax, the marker appears and nothing is built. */
    {
        /* Both the directory and the markers carry the pid, for the same reason
         * the built binaries do. Two suites running at once shared this
         * directory, so one removed the other's markers and the other's payloads
         * then had nothing to create, which failed as "the payload was not
         * injected" -- a pass, for the wrong reason. */
        char dir[128];
        snprintf(dir, sizeof dir, "/tmp/z_test_shell_%ld", (long)getpid());
        /* The markers are bare names in the working directory rather than paths
         * under `dir`, because a slash inside the payload would make it an
         * invalid file name component and the payload could never be written in
         * the first place. Both are removed before and after. */
        char marker_out[64], marker_arg[64];
        snprintf(marker_out, sizeof marker_out, "z_injected_out_%ld", (long)getpid());
        snprintf(marker_arg, sizeof marker_arg, "z_injected_arg_%ld", (long)getpid());
        char out_path[256], plain_out[256], helper[256], plain_z[256], main_z[256];
        snprintf(out_path, sizeof out_path, "%s/o'; touch %s; '.out", dir, marker_out);
        snprintf(plain_out, sizeof plain_out, "%s/plain.out", dir);
        /* The trailing argument's payload carries an even number of apostrophes
         * and ends in a comment, because an odd number would leave the shell
         * with an unterminated quote and it would refuse the whole line without
         * running anything. A payload that has to be balanced to get as far as
         * executing still proves the point: the marker appears and the driver
         * reports success, having linked nothing at all. The name ends in .c
         * because cc picks the compiler by extension, so anything else would be
         * handed to the linker as a script and fail for the wrong reason. */
        snprintf(helper, sizeof helper, "%s/a'b';touch %s;#x'.c", dir, marker_arg);
        snprintf(plain_z, sizeof plain_z, "%s/plain.z", dir);
        snprintf(main_z, sizeof main_z, "%s/main.z", dir);
        remove(marker_out);
        remove(marker_arg);
        mkdir(dir, 0700);

        static const char plain_src[] = "int main() { Console.WriteLog(42); return 0; }\n";
        static const char link_src[] =
            "extern int c_scale(int v, int k);\n"
            "int main() { Console.WriteLog(c_scale(7, 6)); return 0; }\n";
        static const char csrc[] = "long c_scale(long v, long k) { return v * k; }\n";
        remove(plain_out);
        remove(out_path);
        FILE *pf = fopen(plain_z, "wb");
        FILE *sf = fopen(main_z, "wb");
        FILE *cf = fopen(helper, "wb");
        if (pf == NULL || sf == NULL || cf == NULL ||
            fwrite(plain_src, 1, sizeof plain_src - 1, pf) != sizeof plain_src - 1 ||
            fwrite(link_src, 1, sizeof link_src - 1, sf) != sizeof link_src - 1 ||
            fwrite(csrc, 1, sizeof csrc - 1, cf) != sizeof csrc - 1) {
            if (pf)
                fclose(pf);
            if (sf)
                fclose(sf);
            if (cf)
                fclose(cf);
            fprintf(stderr, "FAIL drivershell (cannot write the test sources)\n");
            fail++;
        } else {
            fclose(pf);
            fclose(sf);
            fclose(cf);

            /* label, output path, trailing argument, source, marker. The marker
             * is checked before the exit status because a shell that misread a
             * value reports a syntax error rather than a build failure, so the
             * status alone would not say why. */
            const struct {
                const char *label;
                const char *out;
                const char *arg;
                const char *src;
                const char *marker;
            } dcases[] = {
                {"output path", out_path, "", plain_z, marker_out},
                {"linker argument", plain_out, helper, main_z, marker_arg},
            };
            for (size_t i = 0; i < sizeof dcases / sizeof(*dcases); i++) {
                /* Three quoted payloads plus the compiler path do not fit in the
                 * shared 2 KiB command buffer, so this group builds its own. */
                char qout[600], qarg[600], qsrc[600];
                char bcmd[3072];
                shell_quote(dcases[i].out, qout, sizeof qout);
                shell_quote(dcases[i].src, qsrc, sizeof qsrc);
                snprintf(bcmd, sizeof bcmd, "%s build %s -o %s", zq, qsrc, qout);
                if (dcases[i].arg[0] != 0) {
                    shell_quote(dcases[i].arg, qarg, sizeof qarg);
                    strncat(bcmd, " ", sizeof bcmd - strlen(bcmd) - 1);
                    strncat(bcmd, qarg, sizeof bcmd - strlen(bcmd) - 1);
                }
                strncat(bcmd, " ", sizeof bcmd - strlen(bcmd) - 1);
                strncat(bcmd, opt_flag(), sizeof bcmd - strlen(bcmd) - 1);
                strncat(bcmd, " 2>&1", sizeof bcmd - strlen(bcmd) - 1);

                char actual[1 << 16];
                int rc = run_cmd_capture(bcmd, actual, sizeof actual);
                int touched = access(dcases[i].marker, F_OK) == 0;
                int built = access(dcases[i].out, X_OK) == 0;
                char printed[256] = "";
                if (built)
                    /* The quoted path is also what running the executable needs,
                     * because the output path has a space in it. */
                    run_cmd_capture(qout, printed, sizeof printed);
                if (touched) {
                    fprintf(stderr,
                            "FAIL drivershell %s (the value was run as shell syntax: '%s' "
                            "exists)\n",
                            dcases[i].label, dcases[i].marker);
                    fail++;
                } else if (!built) {
                    fprintf(stderr,
                            "FAIL drivershell %s (nothing was written to '%s', rc=%d)\n"
                            "--- actual ---\n%s\n",
                            dcases[i].label, dcases[i].out, rc, actual);
                    fail++;
                } else if (strcmp(printed, "42\n") != 0) {
                    fprintf(stderr,
                            "FAIL drivershell %s (the value did not arrive as one argument)\n"
                            "--- actual ---\n%s\n",
                            dcases[i].label, printed);
                    fail++;
                } else if (rc != 0) {
                    fprintf(stderr, "FAIL drivershell %s (the link reported failure, rc=%d)\n%s\n",
                            dcases[i].label, rc, actual);
                    fail++;
                } else {
                    printf("ok   drivershell %s (an apostrophe and a semicolon stayed data)\n",
                           dcases[i].label);
                    pass++;
                }
            }
        }
        remove(plain_out);
        remove(out_path);
        remove(marker_out);
        remove(marker_arg);
        /* The sources and the C helper went in too. Leaving them is what made
         * the directory survive, since an rmdir only succeeds on an empty one,
         * and a directory left behind per run is a directory left behind per run
         * forever. */
        remove(helper);
        remove(main_z);
        remove(plain_z);
        /* The directory goes too. Every file in it has been removed above, so
         * this only succeeds on a clean run, and it is here because a directory
         * left behind per run is a directory left behind per run forever. A run
         * that died mid-way leaves one, which is what `make clean` is for. */
        rmdir(dir);
    }

    /* Runtime-abort cases: the program must exit nonzero and every non-empty line
     * of the sibling .expected must appear in its combined output.
     *
     * No flag is passed. That is the assertion now: these used to be compiled
     * with `--bounds`, which meant they proved the flag worked rather than that
     * the check happens, and a build with checks off by default passed them
     * either way. */
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
        snprintf(cmd, sizeof cmd, "%s run %s %s 2>&1", zq, path, opt_flag());
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

    /* `--no-bounds` turns the check off, and has to be a real flag to do it.
     *
     * It used to be collected as an unrecognized argument and handed to the link
     * step, where `cc` does not have a `-fno-bounds` for C and warns that the
     * option is "valid for Modula-2 but not for C". The program still got its
     * unchecked read, so the flag appeared to work, and the build printed a
     * warning from a tool the caller never named. So this checks both halves:
     * the read succeeds, and nothing is printed that is not the program's own
     * output. */
    {
        snprintf(path, sizeof path, "%s/bounds_off.z", rt_dir);
        snprintf(exp_path, sizeof exp_path, "%s/bounds_off.expected", rt_dir);
        char expected[1 << 16];
        if (!slurp(exp_path, expected, sizeof expected)) {
            fprintf(stderr, "FAIL bounds_off (missing .expected)\n");
            fail++;
        } else {
            snprintf(cmd, sizeof cmd, "%s run %s %s --no-bounds 2>&1", zq, path, opt_flag());
            char actual[1 << 16];
            if (run_cmd_capture(cmd, actual, sizeof actual) != 0 || strcmp(expected, actual) != 0) {
                fprintf(stderr,
                        "FAIL bounds_off (--no-bounds did not give an unchecked read and a clean "
                        "build)\n--- expected ---\n%s--- actual ---\n%s\n",
                        expected, actual);
                fail++;
            } else {
                printf("ok   bounds_off (--no-bounds)\n");
                pass++;
            }
        }
    }

    /* Leak cases: built with AddressSanitizer and run with leak detection on,
     * and required to report no leaked block that came from the string
     * allocator.
     *
     * The assertion is on the allocation stack rather than on a count or a byte
     * total, because what is being tested is a property rather than a number:
     * "no string allocation was still live at exit". A count would have to
     * change whenever an unrelated allocation moved, and would then fail for a
     * reason that has nothing to do with the thing under test.
     *
     * Asserting on the stack catches the other direction too, which is the one
     * that bites: a destructor that frees an element it does not own shows up as
     * an AddressSanitizer double-free rather than as a leak.
     *
     * A heap array cannot be released from Z yet, so the `new T[n]` behind a
     * container is still live at exit and the report is not empty. Only the
     * string blocks are asserted about, which is the part a container's
     * destructor is responsible for. */
    static const char *leaks[] = {"vec_string", "map_string"};
    const char *lk_dir = "tests/leaks";
    if (asan_available(zq)) {
        char leak_out[256];
        for (size_t i = 0; i < sizeof leaks / sizeof *leaks; i++) {
            snprintf(path, sizeof path, "%s/%s.z", lk_dir, leaks[i]);
            snprintf(leak_out, sizeof leak_out, "/tmp/z_leak_%ld_%s", (long)getpid(), leaks[i]);
            snprintf(cmd, sizeof cmd, "%s build %s -o %s %s -fsanitize=address -g 2>&1", zq, path,
                     leak_out, opt_flag());
            char build_err[1 << 16];
            int rc = run_cmd_capture(cmd, build_err, sizeof build_err);
            if (rc != 0) {
                fprintf(stderr, "FAIL %s (compile failed)\n%s\n", leaks[i], build_err);
                fail++;
                continue;
            }
            char actual[1 << 16];
            snprintf(cmd, sizeof cmd, "ASAN_OPTIONS=detect_leaks=1 %s 2>&1", leak_out);
            rc = run_cmd_capture(cmd, actual, sizeof actual);
            /* Exit 1 is LeakSanitizer's report and is expected, so the status is
             * not the assertion. What matters is whether any leaked block names
             * the string allocator. */
            if (strstr(actual, "zstr_alloc_impl") != NULL) {
                fprintf(stderr, "FAIL %s (a string allocation was not released)\n%s\n", leaks[i],
                        actual);
                fail++;
                continue;
            }
            if (strstr(actual, "ERROR: AddressSanitizer") != NULL) {
                fprintf(stderr, "FAIL %s (AddressSanitizer reported an error)\n%s\n", leaks[i],
                        actual);
                fail++;
                continue;
            }
            if (strstr(actual, "LeakSanitizer") == NULL) {
                fprintf(stderr, "FAIL %s (no leak report at all, so nothing was checked)\n%s\n",
                        leaks[i], actual);
                fail++;
                continue;
            }
            printf("ok   %s (leaks)\n", leaks[i]);
            pass++;
            remove(leak_out);
        }
    } else {
        for (size_t i = 0; i < sizeof leaks / sizeof *leaks; i++)
            printf("skip %s (no AddressSanitizer)\n", leaks[i]);
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
        snprintf(link, sizeof link, "%s run %s %s 2>&1", zq, path, cmd);
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
        snprintf(cmd, sizeof cmd, "%s run %s %s 2>&1", zq, path, opt_flag());
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
                snprintf(cmd, sizeof cmd, "%s asm %s %s 2>&1", zq, path, levels[lvl]);
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
