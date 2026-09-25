#include "ast.h"

/* type_name now lives in types.c and is declared there; ast.c is intentionally
 * minimal — the AST is pure data. Kept so the build has a translation unit for
 * header validation and future AST helpers. */
const char *ast_marker_unused(void);
const char *ast_marker_unused(void) { return "vela-ast"; }
