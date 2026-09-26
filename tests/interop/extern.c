/* C side of the interop test. Z's `int` is a 64-bit value, so it maps onto C's
 * `long`; Z's `string` is a NUL-terminated `const char *`. */
#include <stdio.h>

long c_add(long a, long b) { return a + b; }
long c_scale(long v, long k) { return v * k; }
long c_strlen(const char *s) {
    long n = 0;
    while (s[n])
        n++;
    return n;
}
void c_print(const char *s) { fputs(s, stdout); }

/* Writes into a caller-supplied buffer, exercising a pointer parameter. */
long c_fill(long *out, long n) {
    for (long i = 0; i < n; i++)
        out[i] = i * i;
    return n;
}
