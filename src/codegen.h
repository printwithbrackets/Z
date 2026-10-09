#ifndef Z_CODEGEN_H
#define Z_CODEGEN_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"

/* One `gvar`: a project-wide variable and how its storage is to be filled.
 *
 * The parser owns the declarations; this is the shape codegen needs, which is
 * not the parser's shape because codegen does not care where a global was
 * declared, only what symbol to emit and whether its initializer is something
 * an instruction can hold.
 *
 *   sym        the emitted symbol, mangled into the Z namespace so a global
 *              cannot collide with the C runtime's
 *   type       the declared type, for the store width and the zero-fill
 *   init       the initializer expression, evaluated in z_ginit. NULL when
 *              `is_const` and the value went into .data instead
 *   is_const   the initializer is a literal, so it is emitted as data rather
 *              than as code -- `gint n = 5;` is one instruction's worth of
 *              constant and does not need a runtime store at all
 *   ival/dval  the literal, for the .data case
 */
typedef struct {
    const char *name;
    const char *sym;
    Type *type;
    Expr *init;
    int is_const;
    long long ival;
    double dval;
} GlobalTable;

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

    /* The program's `gvar`s, in declaration order. NULL when there are none,
     * which is every program that does not use them, and is why this costs
     * nothing when the feature is unused. */
    const GlobalTable *globals;
    int nglobals;
} CodegenOptions;

/* The level used when the caller passes NULL options. */
#define Z_OPT_DEFAULT 1
#define Z_OPT_MIN 0
#define Z_OPT_MAX 3

/* `opts` may be NULL, which selects the defaults. */
char *codegen_emit_opts(Arena *arena, Stmt *program, StringTable *strings,
                        const CodegenOptions *opts);
char *codegen_emit(Arena *arena, Stmt *program, StringTable *strings);

/* The symbol a global is emitted under, on the caller's arena. Exposed because
 * the driver builds the global table and has to name each entry, and the
 * mangling has to be the one codegen would have used. */
char *codegen_global_sym(Arena *arena, const char *name);

#endif /* Z_CODEGEN_H */
