#include "surface.h"

#include <stdio.h>
#include <string.h>

#include <unistd.h>

/* ASCII-only case folding. Deliberately not `strncasecmp`, for two reasons that
 * point the same way. The runtime's `upper` and `lower` are ASCII-only because a
 * byte above 127 starts a UTF-8 sequence and folding it alone corrupts the
 * character it belongs to. And a name table is read before a single line of the
 * program is diagnosed, so making it answer in the locale's terms would make a
 * build depend on the environment it ran in -- the last thing that should be
 * locale-sensitive is what decides whether `print` is even spelled right. */
static int eq_ci(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return 1;
}

static char *dup_n(Arena *arena, const char *s, size_t n) {
    char *out = arena_alloc(arena, n + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

int surface_add(Surface *s, const char *surface, const char *internal) {
    size_t ns = strlen(surface), ni = strlen(internal);
    if (ns == 0 || ni == 0)
        return -1;
    /* A later mapping of the same spelling replaces the earlier one, matched
     * without regard to case so that a project can override a default by
     * changing only the spelling it cares about. */
    for (int i = 0; i < s->n; i++) {
        if (strlen(s->entries[i].surface) == ns && eq_ci(s->entries[i].surface, surface, ns)) {
            char *copy = dup_n(s->arena, internal, ni);
            if (copy == NULL)
                return -1;
            s->entries[i].internal = copy;
            return 0;
        }
    }
    if (s->n == s->cap) {
        int cap = s->cap == 0 ? 16 : s->cap * 2;
        SurfaceEntry *ne = arena_alloc_array(s->arena, (size_t)cap, sizeof(SurfaceEntry));
        if (ne == NULL)
            return -1;
        if (s->n > 0)
            memcpy(ne, s->entries, (size_t)s->n * sizeof(SurfaceEntry));
        s->entries = ne;
        s->cap = cap;
    }
    char *sf = dup_n(s->arena, surface, ns);
    char *in = dup_n(s->arena, internal, ni);
    if (sf == NULL || in == NULL)
        return -1;
    s->entries[s->n].surface = sf;
    s->entries[s->n].internal = in;
    s->n++;
    return 0;
}

/* One step: what `name` maps to, or NULL if it maps to nothing. */
static const char *lookup_once(const Surface *s, const char *name, size_t n) {
    for (int i = 0; i < s->n; i++) {
        if (strlen(s->entries[i].surface) == n && eq_ci(s->entries[i].surface, name, n))
            return s->entries[i].internal;
    }
    return NULL;
}

const char *surface_lookup(const Surface *s, const char *name, size_t n) {
    if (s == NULL)
        return NULL;
    const char *hit = lookup_once(s, name, n);
    if (hit == NULL)
        return NULL;

    /* Follow the chain to a name nothing maps further.
     *
     * A mapping's right-hand side is written the way a person would write it, and
     * they reach for the name they already aliased: `say = console.writelog`
     * when `Console.WriteLog = print` is in the same file. Resolving one step and
     * stopping leaves that reading a name no code has, so `say(...)` fails with
     * "undefined function 'console.writelog'" -- pointing at the alias as though
     * the program had asked for one by that name.
     *
     * Bounded by the entry count, which is also what makes a cycle terminate:
     * `aa = bb` and `bb = aa` cannot both be honoured, and returning the entry
     * that started it is the one outcome that cannot be wrong about what the
     * project asked for. It still fails to resolve, which is the honest result
     * for a mapping that has no meaning.
     */
    for (int steps = 0; steps < s->n; steps++) {
        const char *next = lookup_once(s, hit, strlen(hit));
        if (next == NULL)
            break;
        if (strcmp(next, name) == 0)
            break; /* back where it started: a cycle */
        hit = next;
    }
    return hit;
}

const char *surface_name(const Surface *s, const char *name) {
    const char *hit = surface_lookup(s, name, strlen(name));
    return hit != NULL ? hit : name;
}

Surface *surface_new(Arena *arena) {
    Surface *s = arena_alloc(arena, sizeof *s);
    if (s == NULL)
        return NULL;
    s->arena = arena;
    /* The names Z ships. `print` is reachable only as `Console.WriteLog`, and
     * both word orders are listed because `WriteLog` and `LogWrite` are two
     * words rather than two spellings of one, so folding case does not join
     * them. */
    static const struct {
        const char *surface;
        const char *internal;
    } defaults[] = {
        {"Console.WriteLog", "print"},
        {"Console.LogWrite", "print"},
    };
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++)
        (void)surface_add(s, defaults[i].surface, defaults[i].internal);
    return s;
}

/* ---- the manifest ---- */

static char *skip_space(char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    return p;
}

int surface_load(Surface *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[1024];
    int lineno = 0;
    int rc = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        lineno++;
        char *p = skip_space(line);
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;
        char *eq = strchr(p, '=');
        if (eq == NULL) {
            fprintf(stderr, "%s:%d: expected 'surface = internal'\n", path, lineno);
            rc = -1;
            break;
        }
        *eq = '\0';
        char *lhs = skip_space(p);
        char *rhs = skip_space(eq + 1);
        /* Trim the trailing newline and any space, so the right-hand side is not
         * `print ` -- which would become a surface name of its own. */
        size_t rl = strlen(rhs);
        while (rl > 0 && (rhs[rl - 1] == '\n' || rhs[rl - 1] == '\r' || rhs[rl - 1] == ' ' ||
                          rhs[rl - 1] == '\t'))
            rhs[--rl] = '\0';
        size_t ll = strlen(lhs);
        while (ll > 0 && (lhs[ll - 1] == ' ' || lhs[ll - 1] == '\t'))
            lhs[--ll] = '\0';
        if (ll == 0 || rl == 0) {
            fprintf(stderr, "%s:%d: a mapping needs a name on both sides of '='\n", path, lineno);
            rc = -1;
            break;
        }
        if (surface_add(s, lhs, rhs) != 0) {
            fprintf(stderr, "%s:%d: out of memory\n", path, lineno);
            rc = -1;
            break;
        }
    }
    fclose(f);
    return rc;
}

/* The directory part of `path`, into `out`. "." when there is none, so a bare
 * file name is looked for beside the current directory rather than in it. */
static void dir_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0)
        n = 1; /* the root directory */
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

int surface_discover(Surface *s, const char *src_path) {
    char dir[1024];
    dir_of(src_path, dir, sizeof dir);
    /* A bare file name is a file in the current directory, so the walk starts
     * there -- and "there" has to be the real path, not the two-character ".", or
     * the walk has nowhere to go and stops before it has looked at anything. This
     * is the `z run main.z` case, which is the common one. */
    if (strcmp(dir, ".") == 0 && getcwd(dir, sizeof dir) == NULL)
        return 0;
    /* Walk up. Bounded by the path depth rather than trusted to terminate on a
     * path that eventually becomes "/", which a caller could pass. */
    for (int depth = 0; depth < 64; depth++) {
        char cand[1200];
        snprintf(cand, sizeof cand, "%s/%s", dir, SURFACE_FILE);
        FILE *probe = fopen(cand, "r");
        if (probe != NULL) {
            fclose(probe);
            /* Empty is fine. It is a project with the shipped names, and loading
             * it costs one open of a file with no lines. Rejecting it would mean
             * a project could not be marked without inventing a setting. */
            return surface_load(s, cand) == 0 ? 1 : -1;
        }
        if (strcmp(dir, "/") == 0)
            break;
        char up[1024];
        dir_of(dir, up, sizeof up);
        if (strcmp(up, dir) == 0)
            break;
        memcpy(dir, up, strlen(up) + 1);
    }
    return 0;
}