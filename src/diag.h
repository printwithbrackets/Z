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

/* Reports an error at `span`, rendering:
 *     file:line:col: error: message
 *         <source line>
 *         <caret>^ */
void diag_error(Span span, const char *fmt, ...);

/* The warnings the compiler knows how to emit. Each has a stable short name
 * used by -Wno-<name>, and each can be turned into an error by -Werror=<name>.
 * A warning that is off is not merely quieter: the check that produces it does
 * not run at all, so a suppressed warning costs nothing. */
typedef enum {
    W_UNUSED_LOCAL,  /* a local is declared and never read */
    W_SHADOWED_LOCAL, /* a local redeclares an enclosing one */
    W_UNREACHABLE,   /* statements follow a jump out of the block */
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
