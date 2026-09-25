#include "arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN 16
#define ARENA_MIN_BLOCK (64 * 1024)

struct ArenaBlock {
    struct ArenaBlock *next;
    size_t capacity; /* usable bytes following the header */
    size_t used;     /* bytes consumed from this block */
};

/* Rounds n up to the next multiple of ARENA_ALIGN. */
static size_t align_up(size_t n) { return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1); }

/* Byte size of the header including its trailing alignment padding. */
static size_t header_size(void) { return align_up(sizeof(ArenaBlock)); }

void arena_init(Arena *arena) { arena->head = NULL; }

void *arena_alloc(Arena *arena, size_t size) {
    if (size == 0)
        return NULL;
    size_t need = align_up(size);

    ArenaBlock *b = arena->head;
    if (b == NULL || b->capacity - b->used < need) {
        size_t block = need > ARENA_MIN_BLOCK ? need : ARENA_MIN_BLOCK;
        ArenaBlock *nb = malloc(sizeof *nb + header_size() + block);
        if (nb == NULL) {
            fprintf(stderr, "vela: out of memory\n");
            exit(1);
        }
        nb->next = arena->head;
        nb->capacity = block;
        nb->used = 0;
        arena->head = nb;
        b = nb;
    }

    char *p = (char *)b + header_size() + b->used;
    b->used += need;
    memset(p, 0, need);
    return p;
}

void *arena_alloc_array(Arena *arena, size_t count, size_t size) {
    if (count == 0 || size == 0)
        return NULL;
    if (count > SIZE_MAX / size) {
        fprintf(stderr, "vela: allocation size overflow\n");
        exit(1);
    }
    return arena_alloc(arena, count * size);
}

char *arena_strdup(Arena *arena, const char *s) { return arena_strndup(arena, s, strlen(s)); }

char *arena_strndup(Arena *arena, const char *s, size_t n) {
    char *p = arena_alloc(arena, n + 1);
    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void arena_free(Arena *arena) {
    ArenaBlock *b = arena->head;
    while (b != NULL) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    arena->head = NULL;
}

size_t arena_bytes_used(const Arena *arena) {
    size_t total = 0;
    for (const ArenaBlock *b = arena->head; b != NULL; b = b->next) {
        total += b->used;
    }
    return total;
}
