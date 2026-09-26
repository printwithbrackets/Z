#ifndef Z_ARENA_H
#define Z_ARENA_H

#include <stddef.h>

/* Bump allocator. The compiler allocates almost everything (tokens, AST nodes,
 * strings) from one arena and frees it in a single call at exit. This keeps
 * ownership trivial: no per-node free, no leaks, no use-after-free. */

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

/* Initializes an empty arena. */
void arena_init(Arena *arena);

/* Allocates `size` bytes, 16-byte aligned and zeroed. Returns NULL if size is 0. */
void *arena_alloc(Arena *arena, size_t size);

/* Allocates `count * size` bytes, 16-byte aligned and zeroed. Overflow-safe. */
void *arena_alloc_array(Arena *arena, size_t count, size_t size);

/* Copies a NUL-terminated string into the arena and returns the copy. */
char *arena_strdup(Arena *arena, const char *s);

/* Copies at most `n` bytes of `s` into the arena, NUL-terminated. */
char *arena_strndup(Arena *arena, const char *s, size_t n);

/* Frees all memory owned by the arena, resetting it to empty. */
void arena_free(Arena *arena);

/* Total bytes currently held by the arena (diagnostics only). */
size_t arena_bytes_used(const Arena *arena);

#endif /* Z_ARENA_H */
