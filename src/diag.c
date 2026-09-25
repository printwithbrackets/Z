#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *g_file = "<unknown>";
static const char *g_text = NULL;
static int g_errors = 0;

void diag_set_source(const char *file, const char *text) {
    g_file = file;
    g_text = text;
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

    if (g_text == NULL)
        return;

    /* Locate the physical line containing span.start so we can echo it. */
    int start = span.start;
    if (start < 0)
        return;
    int len = (int)strlen(g_text);
    if (start > len)
        start = len;
    int line_begin = start;
    while (line_begin > 0 && g_text[line_begin - 1] != '\n')
        line_begin--;
    int line_end = start;
    while (line_end < len && g_text[line_end] != '\n')
        line_end++;

    fprintf(stderr, "    %.*s\n", line_end - line_begin, g_text + line_begin);

    /* Caret line: spaces up to the token, then one or more '^'. */
    int caret_col = start - line_begin;
    for (int i = 0; i < caret_col; i++)
        fputc(' ', stderr);
    int carets = span.len < 1 ? 1 : span.len;
    for (int i = 0; i < carets; i++)
        fputc('^', stderr);
    fputc('\n', stderr);
}
