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
    /* Emit DWARF debugging information: a line table for stepping and
     * breakpoints, and a symbol table naming every function, parameter and
     * frame-resident local. Off by default, since the sections are dead weight
     * in a shipped binary. */
    int debug_info;
    /* Optimization level, spelled the way gcc spells it so the two are
     * directly comparable. Z_OPT_LEVELS is the default, which is what the
     * compiler has always done.
     *
     *   0  naive: no register allocation, no constant folding or propagation,
     *      no strength reduction. Every local lives in memory and division
     *      is a real idiv. A baseline to measure the optimizer against, and
     *      the level to debug codegen with.
     *   1  the default: constant folding and propagation, a liveness-based
     *      local register allocator, leaf and immediate operand selection,
     *      branch-on-flags conditions, in-place compound assignment, and
     *      constant division/modulo strength reduction.
     *   2  adds loop-invariant code motion.
     *   3  adds loop unrolling on top of level 2.
     */
    int opt_level;
} CodegenOptions;

/* The level used when the caller passes NULL options. */
#define Z_OPT_DEFAULT 1
#define Z_OPT_MIN 0
#define Z_OPT_MAX 3

/* `opts` may be NULL, which selects the defaults. */
char *codegen_emit_opts(Arena *arena, Stmt *program, StringTable *strings,
                        const CodegenOptions *opts);
char *codegen_emit(Arena *arena, Stmt *program, StringTable *strings);

#endif /* Z_CODEGEN_H */
