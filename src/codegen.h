#ifndef Z_CODEGEN_H
#define Z_CODEGEN_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"

/* Lowers a checked program to x86-64 assembly text (Intel syntax, System V
 * AMD64 ABI). Returns a malloc'd NUL-terminated string that the caller writes
 * to a .s file and hands to the system assembler. */
typedef struct {
    /* Emit a runtime range check on every array index. Off by default: the
     * check costs a load of the length header plus two branches per access. */
    int bounds_checks;
} CodegenOptions;

/* `opts` may be NULL, which selects the defaults. */
char *codegen_emit_opts(Arena *arena, Stmt *program, StringTable *strings,
                        const CodegenOptions *opts);
char *codegen_emit(Arena *arena, Stmt *program, StringTable *strings);

#endif /* Z_CODEGEN_H */
