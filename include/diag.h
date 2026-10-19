#ifndef Z_DIAG_H
#define Z_DIAG_H

#include "source.h"

/* Registers a file name and its full text so diagnostics can render a source
 * excerpt with a caret under the offending token. A compilation unit may span
 * several files once `import` is used, so sources are looked up per span
 * rather than held in a single slot. */
void diag_set_source(const char *file, const char *text);

/* Number of errors reported so far; the driver uses this as its exit code. */
int diag_error_count(void);

/* ---- output shape ----
 *
 * Three renderings of the same diagnostic, chosen with --error-format. The
 * default is for a person reading a terminal; `gcc` is the one line per problem
 * an editor's error parser already understands; `json` carries a stable code
 * and an explicit span so a tool can put a squiggle under the right word
 * without scraping prose. */
typedef enum { DIAG_FMT_HUMAN, DIAG_FMT_GCC, DIAG_FMT_JSON } DiagFormat;

void diag_set_format(DiagFormat fmt);
DiagFormat diag_get_format(void);
/* Parses the argument of --error-format. Returns 0 for an unrecognised name, so
 * the driver can report it as a usage error rather than forwarding it on. */
int diag_set_format_flag(const char *value);

/* Colour. On by default when stderr is a terminal, off when it is redirected or
 * piped, and forced either way with --color=always / --color=never. NO_COLOR
 * disables it whatever the terminal says, per the convention every other tool
 * follows. */
void diag_set_color(int on);
int diag_color_enabled(void);
int diag_set_color_flag(const char *value);

/* ---- where we are ----
 *
 * A diagnostic in a hundred-line function is hard to act on without knowing
 * which function it is in, and a diagnostic inside a monomorphized generic
 * points at a span in the *template*, which the reader never wrote at the place
 * the error appeared. The compiler pushes a frame as it enters each of those
 * and the first diagnostic raised inside it picks up a note naming it, so both
 * facts are in the message without every call site having to know them. */
typedef enum {
    DIAG_CTX_FUNC,     /* a function body */
    DIAG_CTX_METHOD,   /* a struct or class method body */
    DIAG_CTX_LAMBDA,   /* a lambda body: the span is generated text */
    DIAG_CTX_INSTANCE, /* a generic instance: the span is the template's */
} DiagCtxKind;

/* `label` names the thing ("main", "Vec.push", "Vec<int>.push") and `origin` is
 * where the reader wrote it, which is the line an instantiated generic was
 * asked for at. `origin` may be a zero span when there is no separate place. */
void diag_push_ctx(DiagCtxKind kind, const char *label, Span origin);
void diag_pop_ctx(void);

/* ---- notes ----
 *
 * A note is extra guidance attached to the next error or warning: the
 * constraint that was violated, a declaration to look at, a name that is one
 * character away. Notes are *queued* and drained by the next diagnostic, so a
 * caller raises them before the error they belong to. That is the natural
 * direction anyway -- the note is derived from the same data as the message --
 * and it is what lets the renderer put them after the source frame where they
 * belong instead of before the error they explain. */
void diag_note(const char *fmt, ...);
/* A note with its own location, for pointing at a declaration elsewhere. */
void diag_note_at(Span span, const char *fmt, ...);
/* Drops queued notes. For a call site that decided not to report after all. */
void diag_clear_notes(void);

/* ---- suggestions ----
 *
 * "undefined variable 'gt'" tells the reader nothing about the fact that `get`
 * is three characters away. `diag_suggest` picks the closest name by edit
 * distance over whatever candidate list the caller already has -- the symbol
 * table for a name lookup, the method list for a member lookup -- and returns
 * NULL when nothing is close enough to be worth saying. */
const char *diag_suggest(const char *name, const char *const *candidates, int n);
int diag_edit_distance(const char *a, const char *b);
/* Queues a "did you mean" note if a candidate is close enough. `what` names the
 * kind of thing ("variable", "function", "method", "type", "field",
 * "variant"), so the note reads as a sentence rather than a diff. */
void diag_suggest_note(const char *what, const char *name, const char *const *candidates, int n);

/* ---- reporting ---- */

/* Reports an error at `span`. The message is a complete sentence with no
 * trailing period; the renderer adds the location, the severity word and, for
 * DIAG_FMT_HUMAN, the source excerpt and any queued notes:
 *     file:line:col: error: message
 *         <source line>
 *         <caret>^
 *         note: ... */
void diag_error(Span span, const char *fmt, ...);

/* As diag_error, with a stable machine-readable code (e.g. "undefined_name")
 * that --error-format=json carries through and an editor can key on. The code
 * is not part of the human message. */
void diag_error_code(Span span, const char *code, const char *fmt, ...);

/* The warnings the compiler knows how to emit. Each has a stable short name
 * used by -Wno-<name>, and each can be turned into an error by -Werror=<name>.
 * A warning that is off is not merely quieter: the check that produces it does
 * not run at all, so a suppressed warning costs nothing. */
typedef enum {
    W_UNUSED_LOCAL,   /* a local is declared and never read */
    W_SHADOWED_LOCAL, /* a local redeclares an enclosing one */
    W_UNREACHABLE,    /* statements follow a jump out of the block */
    W_UNUSED_IMPORT,  /* an import that contributes nothing to the unit */
    W_COUNT
} WarnKind;

/* Reports a warning at `span`, rendering:
 *     file:line:col: warning: message
 *         <source line>
 *         <caret>^
 * A warning never fails the build on its own, but -Werror turns every enabled
 * warning into an error. */
void diag_warn(WarnKind kind, Span span, const char *fmt, ...);

/* Warnings reported so far, whether or not they were promoted to errors.
 * The driver reports this in its summary line. */
int diag_warning_count(void);

/* Applies one command-line warning flag: "-w" silences everything, "-Werror"
 * promotes every enabled warning, "-Werror=<name>" promotes one, "-Wno-<name>"
 * silences one, and "-W<name>" re-enables one explicitly. Returns 0 for an
 * unrecognised flag, which the driver treats as a usage error rather than
 * passing the flag on to the assembler. */
int diag_set_warn_flag(const char *flag);

/* The -Wname of a warning, for diagnostics about the warning system itself. */
const char *warn_kind_name(WarnKind kind);

/* True when a warning of this kind is currently enabled. */
int warn_enabled(WarnKind kind);

#endif /* Z_DIAG_H */
