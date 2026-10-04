#ifndef Z_PARSER_H
#define Z_PARSER_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include "token.h"

#include "surface.h"

/* Parses, name-resolves, and type-checks a token stream into a program whose
 * root is a top-level S_BLOCK item. Returns NULL only if allocation fails;
 * recoverable syntax/type errors are reported through diag_error and cause a
 * non-zero diag_error_count(). */
/* `surf` is the dialect: the names a project writes mapped to the names the
 * compiler knows. NULL means the shipped defaults only, which is what a caller
 * that has not loaded a manifest passes. */
Stmt *parse_program(Arena *arena, Token *toks, int ntoks, StringTable *strings, int opt_level,
                    Surface *surf);

#endif /* Z_PARSER_H */
