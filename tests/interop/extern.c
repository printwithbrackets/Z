/* C side of the interop test. Z's `int` is a 64-bit value, so it maps onto C's
 * `long`; Z's `string` is a pointer to a { len, cap } header followed by the
 * bytes, and what the program holds points at the bytes -- which are NUL-
 * terminated, so a Z string is a valid `const char *` and needs no conversion
 * in either direction.
 *
 * That last part is the whole reason the string type is a pointer with a header
 * rather than a two-word struct. A `string` stays a scalar, so it still travels
 * in a register, and `extern` and `export` still mean what they meant: C gets
 * `const char *` and gives back one. The cost is that the boundary still stops
 * at an embedded zero, which is a property of C's strings and not something this
 * file can fix -- so the two functions below that need the length read it out of
 * the header, the way a C caller wanting Z's semantics should. */
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

/* Reads Z's length out of the header 16 bytes before the bytes. A C function
 * that wants to see a Z string whole -- embedded zero and all -- does this
 * rather than calling strlen, which cannot. */
long c_zlen(const char *s) {
    const long *header = (const long *)s;
    return header[-2];
}

/* Writes `n` bytes from `s`, per Z's length, so a string containing a zero
 * arrives intact. */
long c_zprint(const char *s) {
    long n = ((const long *)s)[-2];
    fwrite(s, 1, (size_t)n, stdout);
    fputc('\n', stdout);
    return n;
}

/* Writes into a caller-supplied buffer, exercising a pointer parameter. */
long c_fill(long *out, long n) {
    for (long i = 0; i < n; i++)
        out[i] = i * i;
    return n;
}
