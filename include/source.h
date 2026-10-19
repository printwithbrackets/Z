#ifndef Z_SOURCE_H
#define Z_SOURCE_H

/* A location in a source file. `line` and `col` are 1-based for display;
 * `start` and `len` are byte offsets used to render the source excerpt. */
typedef struct {
    const char *file;
    int line;
    int col;
    int start;
    int len;
} Span;

#endif /* Z_SOURCE_H */
