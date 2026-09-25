#ifndef VELA_DIAG_H
#define VELA_DIAG_H

#include "source.h"

/* Registers the file name and full text so diagnostics can render a source
 * excerpt with a caret under the offending token. */
void diag_set_source(const char *file, const char *text);

/* Number of errors reported so far; the driver uses this as its exit code. */
int diag_error_count(void);

/* Reports an error at `span`, rendering:
 *     file:line:col: error: message
 *         <source line>
 *         <caret>^ */
void diag_error(Span span, const char *fmt, ...);

#endif /* VELA_DIAG_H */
