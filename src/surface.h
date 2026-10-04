#ifndef Z_SURFACE_H
#define Z_SURFACE_H

#include <stddef.h>

#include "arena.h"

/* ---- surface names ----
 *
 * The spelling a program writes for a global is data, not syntax. A surface name
 * is what a project calls something; the internal name is what the compiler, the
 * runtime and the emitted symbols call it. `Console.WriteLog` and `TextBuffer`
 * are surface names; `print` and `StringBuilder` are the internal ones.
 *
 * The point is that a codebase can be written in its own dialect. A project that
 * says `TextBuffer = StringBuilder` in its manifest gets a library that reads the
 * way it wants to read, and nothing in the compiler, the runtime or the emitted
 * x86-64 changes -- the rename happens while names are being resolved, so a
 * surface name is an ordinary global by the time code generation sees it.
 *
 * Two rules keep that from being a source of surprise:
 *
 *   - A surface name is a *fallback*. Ordinary resolution is tried first, so a
 *     project that declares its own `TextBuffer` gets its own, and a local
 *     variable named after a surface name is never renamed out from under the
 *     code that reads it.
 *   - Matching ignores case, so a name can be written the way a house style
 *     writes it. Only for surface names: Z's own identifiers stay
 *     case-sensitive, or two of them would stop being different.
 */

typedef struct {
    const char *surface;  /* what a program writes */
    const char *internal; /* what the compiler calls it */
} SurfaceEntry;

typedef struct Surface {
    Arena *arena;
    SurfaceEntry *entries;
    int n;
    int cap;
} Surface;

/* A surface with the built-in names in it: the ones Z ships. A project manifest
 * adds to these and may replace them, so a dialect is a set of overrides rather
 * than a replacement of the whole table. */
Surface *surface_new(Arena *arena);

/* Adds or replaces one mapping. A later `add` of the same surface name wins,
 * which is what lets a project's manifest override a default. Returns 0 on
 * success, -1 if the strings do not fit or memory ran out. */
int surface_add(Surface *s, const char *surface, const char *internal);

/* The internal name for `name`, or NULL if it is not a surface name. `n` is the
 * length of `name`; the name need not be NUL-terminated. */
const char *surface_lookup(const Surface *s, const char *name, size_t n);

/* The internal name for `name`, or `name` itself when it is not a surface name.
 * The returned pointer is either the argument or arena-owned; both outlive the
 * parse. */
const char *surface_name(const Surface *s, const char *name);

/* Reads one manifest. The format is one mapping per line:
 *
 *     # comment
 *     Console.WriteLog = print
 *     TextBuffer       = StringBuilder
 *
 * Blank lines and lines whose first non-space character is `#` are ignored. A
 * line with no `=`, or with an empty side, is an error naming the file and the
 * line: a manifest that is silently half-read is a dialect that is half in
 * effect, which is worse than one that refuses to compile.
 *
 * Returns 0 on success, or -1 after reporting. */
int surface_load(Surface *s, const char *path);

/* Finds the manifest that governs `src_path` and loads it, if there is one.
 *
 * The search starts at the directory holding `src_path` and walks up to the
 * filesystem root, taking the first `z.surface` it finds. Taking the *nearest*
 * one is what makes a dialect a property of a subtree: a project vendored inside
 * another keeps its own. One manifest governs the whole compilation rather than
 * one per file, because `import` splices rather than isolates -- a file has no
 * existence at run time and, under the language's own model, no independent
 * identity at compile time either. A library imported into a project is
 * therefore read in that project's dialect, and that is the same rule `import`
 * already follows.
 *
 * Returns 1 if a manifest was loaded, 0 if there was none, -1 on error. */
int surface_discover(Surface *s, const char *src_path);

/* The file name a manifest is looked for under. */
#define SURFACE_FILE "z.surface"

#endif /* Z_SURFACE_H */