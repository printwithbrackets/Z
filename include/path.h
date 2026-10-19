#ifndef Z_PATH_H
#define Z_PATH_H

#include <stddef.h>

/* ---- shared path helpers ----
 *
 * Single home for the path logic previously duplicated between main.c
 * (dir_of / join_under / normalize_path / abs_path / dir_parent, ~234-444)
 * and surface.c (dir_of into a fixed buffer + the walk in surface_discover,
 * ~196-242).
 *
 * Both files join directories with names, take a path's parent, and lexically
 * normalize absolute paths; surface.c did so with 1024-byte stack buffers
 * that truncated long paths, while main.c did so with malloc'd strings.
 * Everything here returns a fresh malloc'd string (NULL on OOM or bad input)
 * and never truncates: the caller owns the result and frees it. Callers that
 * cannot proceed without the path die or report; this module never exits.
 *
 * All paths produced by path_normalize / path_abs are absolute and free of
 * "." / ".." components (lexical only, never realpath: symlinks are not
 * resolved so the unit does not depend on which link was followed).
 */

#define PATH_MAX_DEPTH 64

/* strdup that returns NULL on OOM (NULL input gives NULL). */
char *path_strdup(const char *s);

/* `dir` and `name` joined with one '/', as a fresh string. */
char *path_join(const char *dir, const char *name);

/* Directory part of `path`, as a fresh string ("." when there is none,
 * "/" for "/foo"). */
char *path_dir_of(const char *path);

/* `path` with "." / ".." resolved and slashes collapsed, as a fresh string.
 * Always absolute (a relative input is treated as relative to an implied
 * root, so "a/b" becomes "/a/b"); "/" when empty. */
char *path_normalize(const char *path);

/* `src` as an absolute normalized path, or NULL if the working directory is
 * unreadable or memory ran out. */
char *path_abs(const char *src);

/* The directory one level up, as a fresh string. "/" for the root, so a
 * caller walking up stops rather than looping. `dir` should be absolute and
 * normalized (true of what find_project_root / surface_discover build). */
char *path_parent(const char *dir);

#endif /* Z_PATH_H */
