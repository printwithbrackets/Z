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
/* One `gvar` as codegen needs it. The parser owns the declaration and the
 * diagnostic span; codegen needs a symbol and to know whether the initializer
 * fits in an instruction. Kept in the public header because the driver builds
 * the table and hands it to codegen, and a function returning it would only move
 * the same struct somewhere else. */
typedef struct {
    const char *name;
    const char *sym;
    Type *type;
    Expr *init;
    Span span;
    int is_const;
    long long ival;
    double dval;
} ParsedGlobal;

Stmt *parse_program(Arena *arena, Token *toks, int ntoks, StringTable *strings, int opt_level,
                    Surface *surf);

/* The gvars the program declared, copied into `out` in declaration order, and
 * the number written. `out` may be NULL to ask only how many there are, which is
 * how the driver sizes the array in one pass. */
int parse_globals(Arena *arena, Stmt *program, ParsedGlobal *out, int cap);

#endif /* Z_PARSER_H */
