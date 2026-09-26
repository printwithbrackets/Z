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

static int slurp(const char *path, char *out, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int n = (int)fread(out, 1, (size_t)cap - 1, f);
    out[n] = 0;
    fclose(f);
    return 1;
}

static int run_cmd_capture(const char *cmd, char *out, int cap) {
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    int n = (int)fread(out, 1, (size_t)cap - 1, p);
    out[n] = 0;
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
    int pass = 0, fail = 0;

    /* Golden cases. */
    char path[512], exp_path[512], cmd[2048];
    static const char *cases[] = {
        "hello",    "arith",      "vars",      "bools",    "ifelse",         "loops",
        "funcs",    "recursion",  "strings",   "compound", "mainfunc",       "nested_calls",
        "manyargs", "scopes",     "shadows",   "mainret",  "forloop",        "pointers",
        "arrays",   "foreach",    "strconcat", "structs",  "structs_nested", "interp",
        "methods",  "props",      "ext",       "fatarrow", "opoverload",     "tern",
        "gc",       "enum_match", "constfold", "regalloc", "divmod",         "boolstr",
        "generics", "generics2",  "classes",   "classes2", "integration",
        "frame_layout", "bitwise",      "consts",         "breakcontinue", "nested_loops", "null", "strcmp", "intrinsics", "trig", "symnames", "bigconst", "fnptr", "methodptr",
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
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin 2>&1", zc, path);
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
        "undefined_var", "type_mismatch",       "missing_semi", "unknown_func", "bad_condition",
        "arity",         "unterminated_string", "stray_char",   "return_type",  "undefined_type",
        "too_many_params", "too_many_method_params", "bad_const", "dup_const",
        "bitwise_on_string", "break_outside_loop", "continue_outside_loop", "intrinsic_arity", "intrinsic_arg_type", "compare_mixed", "extern_with_body", "extern_arity", "extern_argtype", "export_no_body", "fnptr_argcount", "fnptr_argtype", "fnptr_signature", "fnptr_order", "fnptr_arity_mismatch", "methodptr_struct_recv", "methodptr_no_method", "methodptr_struct_return",
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
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/z_test_bin 2>&1", zc, path);
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

    /* Runtime-abort cases: compiled with --bounds, the program must exit
     * nonzero and every non-empty line of the sibling .expected must appear in
     * its combined output. */
    static const char *aborts[] = {
        "bounds_read", "bounds_write", "bounds_negative", "bounds_runtime_len",
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
        snprintf(cmd, sizeof cmd, "./%s run %s --bounds 2>&1", zc, path);
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
        "extern", "export",
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
        if (run_cmd_capture(link, actual, sizeof actual) != 0 ||
            strcmp(expected, actual) != 0) {
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
        snprintf(cmd, sizeof cmd, "./%s run %s 2>&1", zc, path);
        char actual[1 << 16];
        if (run_cmd_capture(cmd, actual, sizeof actual) != 0 ||
            strcmp(expected, actual) != 0) {
            fprintf(stderr, "FAIL %s (output mismatch)\n--- expected ---\n%s--- actual ---\n%s\n",
                    modules[i], expected, actual);
            fail++;
            continue;
        }
        printf("ok   %s (modules)\n", modules[i]);
        pass++;
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
