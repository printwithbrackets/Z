#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *g_file = "<unknown>";
static const char *g_text = NULL;
static int g_errors = 0;

/* Every source seen this compilation, so a span from an imported file still
 * renders its own excerpt. Bounded because a program only ever has as many
 * files as it imports. */
#define MAX_SOURCES 256
static const char *s_files[MAX_SOURCES];
static const char *s_texts[MAX_SOURCES];
static int n_sources = 0;

void diag_set_source(const char *file, const char *text) {
    g_file = file;
    g_text = text;
    for (int i = 0; i < n_sources; i++) {
        if (strcmp(s_files[i], file) == 0) {
            s_texts[i] = text;
            return;
        }
    }
    if (n_sources < MAX_SOURCES) {
        s_files[n_sources] = file;
        s_texts[n_sources] = text;
        n_sources++;
    }
}

/* The text a span points into, or NULL when the file was never registered. */
static const char *text_for(Span span) {
    if (span.file != NULL) {
        for (int i = 0; i < n_sources; i++)
            if (strcmp(s_files[i], span.file) == 0)
                return s_texts[i];
        return NULL;
    }
    return g_text;
}

int diag_error_count(void) { return g_errors; }

/* The rendered body shared by errors and warnings: echo the physical line
 * containing span.start, then a caret run under the token. */
static void show_excerpt(Span span) {
    const char *text = text_for(span);
    if (text == NULL)
        return;

    /* Locate the physical line containing span.start so we can echo it. */
    int start = span.start;
    if (start < 0)
        return;
    int len = (int)strlen(text);
    if (start > len)
        start = len;
    int line_begin = start;
    while (line_begin > 0 && text[line_begin - 1] != '\n')
        line_begin--;
    int line_end = start;
    while (line_end < len && text[line_end] != '\n')
        line_end++;

    fprintf(stderr, "    %.*s\n", line_end - line_begin, text + line_begin);

    /* Caret line: spaces up to the token, then one or more '^'. */
    int caret_col = start - line_begin;
    for (int i = 0; i < caret_col; i++)
        fputc(' ', stderr);
    int carets = span.len < 1 ? 1 : span.len;
    for (int i = 0; i < carets; i++)
        fputc('^', stderr);
    fputc('\n', stderr);
}

void diag_error(Span span, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    g_errors++;
    fprintf(stderr, "%s:%d:%d: error: %s\n", span.file ? span.file : g_file, span.line, span.col,
            msg);
    show_excerpt(span);
}

/* ---- warnings ----
 *
 * Every warning is on by default: a language whose type checker is strict is
 * only worth the strictness if it also says something about the code that
 * compiles. -w silences all of them, -Wno-<name> silences one, and -Werror
 * turns them into errors so a build can insist on a clean compile.
 */

static const char *const warn_names[W_COUNT] = {
    [W_UNUSED_LOCAL] = "unused-local",
    [W_SHADOWED_LOCAL] = "shadowed-local",
    [W_UNREACHABLE] = "unreachable",
};

/* Warnings default on; -w clears every bit, -Werror does not touch them. */
static unsigned warn_enabled_mask = (1u << W_COUNT) - 1u;
/* Kinds promoted to errors by -Werror=<name>. */
static unsigned warn_error_mask = 0;
/* Set by -Werror with no name. */
static int warn_all_errors = 0;
/* Set by -w, which outranks everything and also skips the checks themselves. */
static int warn_silenced = 0;

static int g_warnings = 0;

const char *warn_kind_name(WarnKind kind) {
    if (kind < 0 || kind >= W_COUNT)
        return "?";
    return warn_names[kind];
}

int warn_enabled(WarnKind kind) {
    if (warn_silenced || kind < 0 || kind >= W_COUNT)
        return 0;
    return (warn_enabled_mask >> kind) & 1u;
}

int diag_warning_count(void) { return g_warnings; }

void diag_warn(WarnKind kind, Span span, const char *fmt, ...) {
    if (!warn_enabled(kind))
        return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    g_warnings++;

    /* A promoted warning is an error in everything but its name, so that
     * -Werror=unused-local reads as the error it is while -Wno-unused-local
     * still matches the same name. */
    int as_error = warn_all_errors || ((warn_error_mask >> kind) & 1u);
    if (as_error)
        g_errors++;

    fprintf(stderr, "%s:%d:%d: %s: %s\n", span.file ? span.file : g_file, span.line, span.col,
            as_error ? "error" : "warning", msg);
    show_excerpt(span);
}

/* Resolves a -W name to its kind, or -1 when the name is not one of ours. */
static int warn_kind_by_name(const char *name, size_t len) {
    for (int i = 0; i < W_COUNT; i++) {
        if (strlen(warn_names[i]) == len && strncmp(warn_names[i], name, len) == 0)
            return i;
    }
    return -1;
}

int diag_set_warn_flag(const char *flag) {
    if (strcmp(flag, "-w") == 0) {
        warn_silenced = 1;
        return 1;
    }
    if (strcmp(flag, "-Werror") == 0) {
        warn_all_errors = 1;
        return 1;
    }
    if (strncmp(flag, "-Werror=", 8) == 0) {
        int k = warn_kind_by_name(flag + 8, strlen(flag + 8));
        if (k < 0)
            return 0;
        /* Promoting a warning also un-silences it, so -w -Werror=x still
         * reports x: asking for it by name is a request to see it. */
        warn_error_mask |= 1u << k;
        return 1;
    }
    if (strncmp(flag, "-Wno-", 5) == 0) {
        int k = warn_kind_by_name(flag + 5, strlen(flag + 5));
        if (k < 0)
            return 0;
        warn_enabled_mask &= ~(1u << k);
        return 1;
    }
    if (strncmp(flag, "-W", 2) == 0) {
        int k = warn_kind_by_name(flag + 2, strlen(flag + 2));
        if (k < 0)
            return 0;
        warn_enabled_mask |= 1u << k;
        return 1;
    }
    return 0;
}
