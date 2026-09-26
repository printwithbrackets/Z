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

void diag_error(Span span, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    g_errors++;
    fprintf(stderr, "%s:%d:%d: error: %s\n", span.file ? span.file : g_file, span.line, span.col,
            msg);

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
