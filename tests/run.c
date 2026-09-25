/* Golden + diagnostic test runner for the Vela compiler.
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
    const char *vela = argv[1];
    const char *case_dir = "tests/cases";
    const char *err_dir = "tests/errors";
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
        snprintf(cmd, sizeof cmd, "%s build %s -o /tmp/vela_test_bin 2>&1", vela, path);
        char build_err[1 << 16];
        int rc = run_cmd_capture(cmd, build_err, sizeof build_err);
        if (rc != 0) {
            fprintf(stderr, "FAIL %s (compile failed)\n%s\n", cases[i], build_err);
            fail++;
            continue;
        }
        char actual[1 << 16];
        int prc = run_cmd_capture("/tmp/vela_test_bin", actual, sizeof actual);
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
        snprintf(cmd, sizeof cmd, "%s build %s 2>&1", vela, path);
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

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
