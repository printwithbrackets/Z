#include "path.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *path_strdup(const char *s) {
    if (s == NULL)
        return NULL;
    size_t n = strlen(s) + 1;
    /* n > 0 always (strlen + 1); overflow impossible short of SIZE_MAX. */
    assert(n > 0);
    char *out = malloc(n);
    if (out == NULL)
        return NULL;
    memcpy(out, s, n);
    return out;
}

char *path_join(const char *dir, const char *name) {
    if (dir == NULL || name == NULL)
        return NULL;
    size_t ld = strlen(dir);
    size_t ln = strlen(name);
    /* dir + '/' + name + NUL; checked so a huge pair cannot wrap around. */
    if (ld > SIZE_MAX - ln - 2)
        return NULL;
    size_t need = ld + 1 + ln + 1;
    char *out = malloc(need);
    if (out == NULL)
        return NULL;
    /* snprintf with the exact budget: ld + 1 + ln + 1 == need, so the write
     * (ld + 1 + ln chars plus NUL) always fits. */
    int w = snprintf(out, need, "%s/%s", dir, name);
    assert(w >= 0 && (size_t)w + 1 == need);
    (void)w;
    return out;
}

char *path_dir_of(const char *path) {
    if (path == NULL)
        return NULL;
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        return path_strdup(".");
    size_t n = (size_t)(slash - path);
    if (n == 0)
        n = 1; /* "/foo" -> "/" */
    assert(n <= strlen(path));
    char *d = malloc(n + 1);
    if (d == NULL)
        return NULL;
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

char *path_normalize(const char *path) {
    if (path == NULL)
        return NULL;
    size_t plen = strlen(path);
    /* One slot per byte plus slack: a segment needs at least one byte, so
     * depth can never exceed plen. +2 covers the empty path and the NUL. */
    if (plen > SIZE_MAX - 2)
        return NULL;
    size_t cap = plen + 2;
    if (cap > SIZE_MAX / sizeof(size_t) / 2)
        return NULL;
    size_t *starts = malloc(cap * sizeof *starts);
    size_t *lens = malloc(cap * sizeof *lens);
    if (starts == NULL || lens == NULL) {
        free(starts);
        free(lens);
        return NULL;
    }

    size_t depth = 0;
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *seg = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t seglen = (size_t)(p - seg);
        assert(seglen > 0);
        assert(depth <= cap);
        if (seglen == 1 && seg[0] == '.')
            continue;
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            /* Ascend. On an absolute path the root stops this from emptying
             * the list; on a relative one depth may already be 0, which
             * stays 0 (the ".." above the implied root is dropped). */
            if (depth > 0)
                depth--;
            continue;
        }
        assert(depth < cap);
        starts[depth] = (size_t)(seg - path);
        lens[depth] = seglen;
        depth++;
    }

    /* "/" when empty, so an absolute path stays absolute. The +1 past the
     * separators is the NUL: depth == 0 yields "/" (1 char + NUL = 2), and
     * every component adds len + 1 (itself plus one separator/slack), which
     * over-allocates by exactly one byte when depth > 0. The old code used
     * total = 1 + sum(len + 1) with malloc(total), writing '/' + NUL past
     * the end when depth == 0 -- a 1-byte heap overflow for "/". */
    size_t total = 2;
    for (size_t i = 0; i < depth; i++) {
        assert(lens[i] <= plen);
        if (lens[i] > SIZE_MAX - total - 1) {
            free(starts);
            free(lens);
            return NULL;
        }
        total += lens[i] + 1;
    }
    char *out = malloc(total);
    if (out == NULL) {
        free(starts);
        free(lens);
        return NULL;
    }
    size_t at = 0;
    assert(total >= 2);
    out[at++] = '/';
    for (size_t i = 0; i < depth; i++) {
        assert(at + lens[i] < total);
        memcpy(out + at, path + starts[i], lens[i]);
        at += lens[i];
        if (i + 1 < depth) {
            assert(at + 1 < total);
            out[at++] = '/';
        }
    }
    assert(at < total);
    out[at] = '\0';
    free(starts);
    free(lens);
    return out;
}

char *path_abs(const char *src) {
    if (src == NULL)
        return NULL;
    char *raw;
    if (src[0] == '/') {
        raw = path_strdup(src);
        if (raw == NULL)
            return NULL;
    } else {
        char *cwd = getcwd(NULL, 0);
        if (cwd == NULL)
            return NULL;
        raw = path_join(cwd, src);
        free(cwd);
        if (raw == NULL)
            return NULL;
    }
    char *out = path_normalize(raw);
    free(raw);
    return out;
}

char *path_parent(const char *dir) {
    if (dir == NULL)
        return NULL;
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')
        len--;
    if (len <= 1)
        return path_strdup("/");
    size_t cut = len;
    while (cut > 0 && dir[cut - 1] != '/')
        cut--;
    /* cut == 0 means no slash in the first len bytes: treat as root. */
    if (cut == 0)
        return path_strdup("/");
    size_t n = cut - 1;
    if (n == 0)
        return path_strdup("/");
    assert(n < len);
    char *out = malloc(n + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, dir, n);
    out[n] = '\0';
    return out;
}
