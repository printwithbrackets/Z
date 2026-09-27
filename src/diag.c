#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* ---- output shape and colour ---- */

static DiagFormat g_format = DIAG_FMT_HUMAN;

/* -1 until something decides: then it is 0 or 1. `isatty` alone would turn
 * colour on under a CI log that is not a terminal, which is exactly where
 * escape codes are noise. */
static int g_color = -1;

void diag_set_format(DiagFormat fmt) { g_format = fmt; }
DiagFormat diag_get_format(void) { return g_format; }

int diag_set_format_flag(const char *value) {
    if (strcmp(value, "human") == 0 || strcmp(value, "short") == 0) {
        g_format = DIAG_FMT_HUMAN;
        return 1;
    }
    if (strcmp(value, "gcc") == 0 || strcmp(value, "gnu") == 0) {
        g_format = DIAG_FMT_GCC;
        return 1;
    }
    if (strcmp(value, "json") == 0) {
        g_format = DIAG_FMT_JSON;
        return 1;
    }
    return 0;
}

void diag_set_color(int on) { g_color = on ? 1 : 0; }

int diag_color_enabled(void) {
    if (g_color < 0) {
        /* NO_COLOR wins over everything, as it is meant to: asking for plain
         * output in the environment outranks asking for colour on a flag. */
        const char *no = getenv("NO_COLOR");
        if (no != NULL && *no != '\0')
            g_color = 0;
        else
            g_color = isatty(2) ? 1 : 0;
    }
    return g_color;
}

int diag_set_color_flag(const char *value) {
    if (strcmp(value, "always") == 0) {
        g_color = 1;
        return 1;
    }
    if (strcmp(value, "never") == 0) {
        g_color = 0;
        return 1;
    }
    if (strcmp(value, "auto") == 0) {
        g_color = -1;
        return 1;
    }
    return 0;
}

/* ---- context frames ---- */

typedef struct DiagCtx {
    DiagCtxKind kind;
    const char *label;
    Span origin;
    int reported; /* the first diagnostic inside it already named it */
    struct DiagCtx *next;
} DiagCtx;

static DiagCtx *g_ctx = NULL;

/* The parser runs deep enough that a diagnostic raised hundreds of frames down
 * should still be attributed, but not so deep that the chain is unbounded if
 * something goes wrong. 32 nested lambdas is far past what the language allows. */
#define MAX_CTX 32
static DiagCtx ctx_pool[MAX_CTX];
static int ctx_pool_used = 0;

void diag_push_ctx(DiagCtxKind kind, const char *label, Span origin) {
    if (ctx_pool_used >= MAX_CTX)
        return;
    DiagCtx *c = &ctx_pool[ctx_pool_used++];
    c->kind = kind;
    c->label = label;
    c->origin = origin;
    c->reported = 0;
    c->next = g_ctx;
    g_ctx = c;
}

void diag_pop_ctx(void) {
    if (g_ctx == NULL)
        return;
    DiagCtx *dead = g_ctx;
    g_ctx = dead->next;
    if (dead == &ctx_pool[ctx_pool_used - 1])
        ctx_pool_used--;
}

/* ---- notes ----
 *
 * Queued and drained by the next diagnostic, so a caller raises them before the
 * error they explain. Bounded because a note without a following error would
 * otherwise accumulate across a long compile. */
#define MAX_NOTES 16
typedef struct {
    int has_span;
    Span span;
    char text[512];
} Note;

static Note g_notes[MAX_NOTES];
static int g_nnotes = 0;

void diag_clear_notes(void) { g_nnotes = 0; }

static void queue_note(int has_span, Span span, const char *fmt, va_list ap) {
    if (g_nnotes >= MAX_NOTES)
        return;
    Note *n = &g_notes[g_nnotes++];
    n->has_span = has_span;
    n->span = span;
    vsnprintf(n->text, sizeof n->text, fmt, ap);
}

void diag_note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    queue_note(0, (Span){0}, fmt, ap);
    va_end(ap);
}

void diag_note_at(Span span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    queue_note(1, span, fmt, ap);
    va_end(ap);
}

/* ---- suggestions ----
 *
 * Levenshtein over bounded lengths: a name table is small, and an edit-distance
 * table of MAX_LEN x MAX_LEN with a cap well above any identifier is cheaper
 * than the allocation a variable-length table would need. */
#define MAX_NAME 128

int diag_edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > MAX_NAME)
        la = MAX_NAME;
    if (lb > MAX_NAME)
        lb = MAX_NAME;
    int prev[MAX_NAME + 1], cur[MAX_NAME + 1];
    for (size_t j = 0; j <= lb; j++)
        prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int d = prev[j - 1] + cost;
            if (prev[j] + 1 < d)
                d = prev[j] + 1;
            if (cur[j - 1] + 1 < d)
                d = cur[j - 1] + 1;
            cur[j] = d;
        }
        memcpy(prev, cur, (lb + 1) * sizeof(int));
    }
    return prev[lb];
}

/* True when the two names differ only in case. A wrong-case identifier is the
 * single most common name error and it always deserves a suggestion, even
 * though the edit distance is exactly the length of the differing part. */
static int case_only(const char *a, const char *b) {
    if (strcmp(a, b) == 0)
        return 0;
    size_t n = strlen(a);
    if (n != strlen(b))
        return 0;
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

/* True when the shorter of the two names is a prefix or a suffix of the longer,
 * and accounts for enough of it to be a real relationship.
 *
 * `helper` and `main_helper` are six edits apart, further than the bar, and no
 * human reading the message would call anything but `main_helper` the intended
 * name. An extension or a prefix is a stronger signal than edit distance and
 * outranks it.
 *
 * The length test is what keeps this from firing on a coincidence: a one-letter
 * name turns up inside half the identifiers in a file, and `total_counts`
 * contains the letter `n` but nobody meant to write `n`. */
static int contains_name(const char *hay, const char *needle) {
    size_t lh = strlen(hay), ln = strlen(needle);
    const char *shorter = lh <= ln ? hay : needle;
    const char *longer = lh <= ln ? needle : hay;
    size_t ls = lh <= ln ? lh : ln;
    size_t ll = lh <= ln ? ln : lh;
    if (ls == 0 || ls * 2 < ll)
        return 0;
    if (strncmp(longer, shorter, ls) == 0)
        return 1;
    return longer[ll - ls] != '\0' && strcmp(longer + (ll - ls), shorter) == 0;
}

const char *diag_suggest(const char *name, const char *const *candidates, int n) {
    size_t len = strlen(name);
    /* The bar for "did you mean" scales with the length of the name, because a
     * short name is close to a great many short names and suggesting any of
     * them is noise: `i` is one edit from every one-letter name in the file. */
    int budget = (int)(len / 3) + 1;
    if (budget > 3)
        budget = 3;
    const char *best = NULL;
    /* A score, not a distance: case-only and containment both have to be able to
     * beat a one-edit match, so the three kinds are ranked into one number
     * rather than compared as distances. */
    int best_score = 0;
    for (int i = 0; i < n; i++) {
        if (candidates[i] == NULL)
            continue;
        int score;
        if (case_only(name, candidates[i]))
            score = 0;
        else if (contains_name(candidates[i], name))
            score = 2;
        else {
            int d = diag_edit_distance(name, candidates[i]);
            if (d > budget)
                continue;
            score = 4 + 4 * d;
        }
        if (best == NULL || score < best_score ||
            (score == best_score && strlen(candidates[i]) < strlen(best))) {
            best = candidates[i];
            best_score = score;
        }
    }
    return best;
}

void diag_suggest_note(const char *what, const char *name, const char *const *candidates,
                       int n) {
    const char *s = diag_suggest(name, candidates, n);
    if (s != NULL)
        diag_note("did you mean the %s '%s'?", what, s);
}

/* ---- rendering ---- */

#define CLR_RESET "\033[0m"
#define CLR_BOLD "\033[1m"
#define CLR_DIM "\033[2m"
#define CLR_RED "\033[1;31m"
#define CLR_YEL "\033[1;33m"
#define CLR_GRN "\033[1;32m"
#define CLR_CYN "\033[1;36m"

/* Emits `s` in `clr` when colour is on, or unchanged when it is not. */
static void paint(const char *clr, const char *s) {
    if (diag_color_enabled())
        fprintf(stderr, "%s%s" CLR_RESET, clr, s);
    else
        fputs(s, stderr);
}

static void paint_num(int n) {
    if (diag_color_enabled())
        fprintf(stderr, "%s%d" CLR_RESET, CLR_BOLD, n);
    else
        fprintf(stderr, "%d", n);
}

/* `file:line:col: ` */
static void put_loc(Span span) {
    paint(CLR_CYN, span.file ? span.file : g_file);
    fputc(':', stderr);
    paint_num(span.line);
    fputc(':', stderr);
    paint_num(span.col);
    fputs(": ", stderr);
}

static void put_sev(const char *sev) {
    if (strcmp(sev, "error") == 0)
        paint(CLR_RED, "error");
    else if (strcmp(sev, "warning") == 0)
        paint(CLR_YEL, "warning");
    else
        paint(CLR_GRN, "note");
}

/* Renders the physical line containing span.start and a caret run under the
 * token, gutter-numbered so a reader can see which line is meant. Only the
 * human format does this; `gcc` is one line per problem by definition and
 * `json` carries the offsets. */
static void show_excerpt(Span span) {
    const char *text = text_for(span);
    if (text == NULL)
        return;
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

    /* A fixed-width gutter, so the caret line below lines up with the text
     * above it without having to measure what was printed. */
#define GUTTER_W 8
    char num[24];
    snprintf(num, sizeof num, "%*d | ", GUTTER_W - 3, span.line);
    paint(CLR_DIM, num);
    fprintf(stderr, "%.*s\n", line_end - line_begin, text + line_begin);

    int caret_col = start - line_begin;
    int line_w = line_end - line_begin;
    int carets = span.len < 1 ? 1 : span.len;
    /* A caret run longer than the token is noise on a long literal, and one
     * running past the end of the line is unreadable, so both are clamped. */
    if (carets > 40)
        carets = 40;
    if (caret_col + carets > line_w)
        carets = line_w - caret_col;
    if (carets < 1)
        carets = 1;
    for (int i = 0; i < GUTTER_W + caret_col; i++)
        fputc(' ', stderr);
    if (diag_color_enabled())
        fputs(CLR_RED, stderr);
    for (int i = 0; i < carets; i++)
        fputc('^', stderr);
    if (diag_color_enabled())
        fputs(CLR_RESET, stderr);
    fputc('\n', stderr);
#undef GUTTER_W
}

/* A note, in the human format: an indented `note:` line, plus a second excerpt
 * when the note points somewhere else. */
static void show_note(const Note *n) {
    fputs("      ", stderr);
    paint(CLR_GRN, "note");
    fputs(": ", stderr);
    fputs(n->text, stderr);
    fputc('\n', stderr);
    if (n->has_span)
        show_excerpt(n->span);
}

/* Names the enclosing function/method/instance for a diagnostic raised inside
 * it. A lambda or a generic instance is worth calling out specifically, because
 * the span in the message points at a template the reader did not write where the
 * error appeared -- and that fact is said once per region, since a region
 * generates many messages about the same text. Which function you are inside is
 * different: a reader who has scrolled to the second of five errors needs it
 * said again. */
static void show_context(void) {
    for (DiagCtx *c = g_ctx; c != NULL; c = c->next) {
        switch (c->kind) {
        case DIAG_CTX_FUNC:
            diag_note("in function '%s'", c->label);
            break;
        case DIAG_CTX_METHOD:
            diag_note("in method '%s'", c->label);
            break;
        case DIAG_CTX_LAMBDA:
            if (c->reported)
                break;
            c->reported = 1;
            /* The span in the error points into the lambda's body, so the
             * reader is looking at a line they wrote but cannot see the shape
             * of the enclosing expression. */
            diag_note_at(c->origin, "inside the lambda starting here");
            break;
        case DIAG_CTX_INSTANCE:
            if (c->reported)
                break;
            c->reported = 1;
            diag_note_at(c->origin, "this is the '%s' instance; the error is in the "
                                    "template it was generated from",
                         c->label);
            break;
        }
        /* The innermost frame is the one that matters; its ancestors are
         * implied by it, and listing all of them is the kind of thoroughness
         * that makes a message hard to skim. */
        break;
    }
}

static void json_str(const char *s) {
    fputc('"', stderr);
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            fputs("\\\"", stderr);
            break;
        case '\\':
            fputs("\\\\", stderr);
            break;
        case '\n':
            fputs("\\n", stderr);
            break;
        case '\t':
            fputs("\\t", stderr);
            break;
        case '\r':
            fputs("\\r", stderr);
            break;
        default:
            if (c < 0x20)
                fprintf(stderr, "\\u%04x", c);
            else
                fputc((int)c, stderr);
        }
    }
    fputc('"', stderr);
}

static void json_loc(Span span) {
    fprintf(stderr, "{\"file\":");
    json_str(span.file ? span.file : g_file);
    fprintf(stderr, ",\"line\":%d,\"col\":%d,\"offset\":%d,\"length\":%d}", span.line, span.col,
            span.start, span.len < 1 ? 1 : span.len);
}

/* One diagnostic, in whichever shape was asked for. `sev` is "error",
 * "warning" or "note"; `code` may be NULL. */
static void report(Span span, const char *sev, const char *code, const char *msg) {
    switch (g_format) {
    case DIAG_FMT_JSON: {
        fputs("{\"severity\":", stderr);
        json_str(sev);
        if (code != NULL) {
            fputs(",\"code\":", stderr);
            json_str(code);
        }
        fputs(",\"message\":", stderr);
        json_str(msg);
        fputs(",\"span\":", stderr);
        json_loc(span);
        fputs(",\"notes\":[", stderr);
        for (int i = 0; i < g_nnotes; i++) {
            if (i > 0)
                fputc(',', stderr);
            fputs("{\"message\":", stderr);
            json_str(g_notes[i].text);
            if (g_notes[i].has_span) {
                fputs(",\"span\":", stderr);
                json_loc(g_notes[i].span);
            }
            fputc('}', stderr);
        }
        fputs("]}\n", stderr);
        break;
    }
    case DIAG_FMT_GCC:
        /* One line per problem, and one line per note. A note with its own
         * location is reported at that location so an editor can put the
         * squiggle on it; one without is reported at the problem's, since
         * there is nowhere else it could point. Dropping them would lose the
         * suggestion and the declaration, which are the parts a tool wants. */
        put_loc(span);
        put_sev(sev);
        fprintf(stderr, ": %s\n", msg);
        for (int i = 0; i < g_nnotes; i++) {
            put_loc(g_notes[i].has_span ? g_notes[i].span : span);
            put_sev("note");
            fprintf(stderr, ": %s\n", g_notes[i].text);
        }
        break;
    case DIAG_FMT_HUMAN:
    default:
        put_loc(span);
        put_sev(sev);
        fprintf(stderr, ": %s\n", msg);
        show_excerpt(span);
        for (int i = 0; i < g_nnotes; i++)
            show_note(&g_notes[i]);
        break;
    }
    g_nnotes = 0;
}

/* Reports an error, attributing it to the enclosing function first. Shared by
 * the plain and coded entry points. */
static void report_error(Span span, const char *code, const char *fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    g_errors++;
    show_context();
    report(span, "error", code, msg);
}

void diag_error(Span span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report_error(span, NULL, fmt, ap);
    va_end(ap);
}

void diag_error_code(Span span, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report_error(span, code, fmt, ap);
    va_end(ap);
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
    [W_UNUSED_IMPORT] = "unused-import",
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
    int as_error = warn_all_errors || ((warn_error_mask >> kind) & (unsigned)1);
    if (as_error)
        g_errors++;

    show_context();
    report(span, as_error ? "error" : "warning", warn_names[kind], msg);
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
