#ifndef Z_LEXER_H
#define Z_LEXER_H

#include "arena.h"
#include "token.h"

/* Deduplicated interned string literals. Identical literals share an id so
 * codegen emits one .rodata entry per distinct string.
 *
 * `lens` is the byte length of each literal, kept beside `items` because a
 * literal may contain an embedded zero: the items are NUL-terminated C strings
 * for the convenience of everything that only wants to print them, so the
 * length cannot be recovered with strlen and has to be stored. It is what ends
 * up in the header of every string value the program sees. */
typedef struct {
    Arena *arena;
    char **items;
    int *lens;
    int count;
    int cap;
} StringTable;

/* Initializes an empty string table backed by `arena`. */
void string_table_init(StringTable *table, Arena *arena);

/* Interns a byte range (not necessarily NUL-terminated) and returns its id. */
int string_intern(StringTable *table, const char *bytes, int len);

/* Tokenizes the whole file up front. Returns a NULL-terminated (by count)
 * arena array of tokens; `out_count` receives the token count. */
Token *lex_all(Arena *arena, const char *src, int len, StringTable *strings, int *out_count);

/* As lex_all, but stamps `file` onto every span so diagnostics name the right
 * file. Used for each file of a multi-file compilation unit. */
Token *lex_all_file(Arena *arena, const char *src, int len, StringTable *strings, int *out_count,
                    const char *file);

#endif /* Z_LEXER_H */
