#ifndef Z_PARSER_H
#define Z_PARSER_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include "token.h"

/* Parses, name-resolves, and type-checks a token stream into a program whose
 * root is a top-level S_BLOCK item. Returns NULL only if allocation fails;
 * recoverable syntax/type errors are reported through diag_error and cause a
 * non-zero diag_error_count(). */
Stmt *parse_program(Arena *arena, Token *toks, int ntoks, StringTable *strings);

#endif /* Z_PARSER_H */
