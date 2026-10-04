/* Z runtime: the allocator, the array/string helpers, and the library the
 * compiler emits calls to.
 *
 * There is no collector. Memory comes from `malloc` and goes back when the
 * owning value's scope ends, which is what the destructors are for: the
 * compiler decides which values own something and calls the destructor at
 * every exit from the scope that declared it. That trade is deliberate -- a
 * tracing collector has to guess what is live from a conservative scan of the
 * machine stack, and a wrong guess is silent corruption, while a destructor
 * either runs or does not and the compiler can see which. */

/* What this file needs from the host, stated rather than inherited.
 *
 * `open`, `read`, `write` and `close` are POSIX, and so is `nanosleep`, which
 * `hold` needs. Under a strict -std=c11 the glibc headers hide nanosleep behind
 * a feature test that nothing has asked for yet, so an AddressSanitizer build of
 * this file with the compiler's own flags, which pass a strict -std=c11, failed
 * on an implicit declaration while the plain `cc` that `z build` shells out to
 * got it by default. Asking for POSIX 2008 explicitly makes both the same, and
 * the definition has to come before any header. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The allocator every managed allocation goes through.
 *
 * A failed allocation is fatal with a message rather than a null return: every
 * caller in here dereferences the result immediately, and threading a null
 * check through the string library to reach a program that cannot report it
 * would be a check nothing on the path could act on. */
void *z_alloc(size_t size) {
    if (size == 0)
        size = 1;
    void *p = malloc(size);
    if (p == NULL) {
        fprintf(stderr, "z: out of memory\n");
        exit(1);
    }
    return p;
}

/* Releases an allocation from z_alloc. `p` is the payload pointer, so a
 * block with a header is freed by subtracting that header first -- see
 * `z_str_free` and `z_array_free`, which are the two the compiler calls. */
void z_free(void *p) { free(p); }

/* ---- compiler-facing runtime API ---- */

/* ---- fixed-point trigonometry ----
 *
 * Z has no floating point type, so trig is integer-only and fully
 * deterministic: the same angle gives the same result on every machine, which
 * floating point could not promise. A full turn is TRIG_TURN units and results
 * are Q30, so sin(TRIG_TURN/4) is exactly 1 << 30. Angles outside one turn are
 * reduced, so any int is a valid angle.
 *
 * CORDIC rotation mode: each step rotates by atan(2^-i) and shifts the other
 * axis, so it needs no multiplies. TRIG_KQ pre-divides by the rotation gain so
 * the result comes out in Q30 directly. The shifts round rather than truncate,
 * which halves the accumulated bias. */
#define TRIG_TURN (1L << 30)
#define TRIG_QUARTER (1L << 28)
/* atan(2^-i) in Q30 */
static const long trig_atan[32] = {
    843314857L, 497837829L, 263043837L, 133525159L, 67021687L, 33543516L,
    16775851L, 8388437L, 4194283L, 2097149L, 1048576L, 524288L,
    262144L, 131072L, 65536L, 32768L, 16384L, 8192L,
    4096L, 2048L, 1024L, 512L, 256L, 128L,
    64L, 32L, 16L, 8L, 4L, 2L,
    1L, 0L,
};
#define TRIG_KQ 652032874L

/* Turns a Q30 angle in [0, pi/2) into its Q30 sine and cosine. */
static void trig_cordic(long zq, long *sinq, long *cosq) {
    long x = TRIG_KQ, y = 0;
    for (int i = 0; i < 32; i++) {
        /* Rounded right shift; at i == 0 the shift is a no-op. */
        long xa = (i > 0) ? ((x + (1L << (i - 1))) >> i) : x;
        long ya = (i > 0) ? ((y + (1L << (i - 1))) >> i) : y;
        if (zq >= 0) {
            long nx = x - ya;
            y = y + xa;
            x = nx;
            zq -= trig_atan[i];
        } else {
            long nx = x + ya;
            y = y - xa;
            x = nx;
            zq += trig_atan[i];
        }
    }
    *sinq = y;
    *cosq = x;
}

/* `want_cos` selects cos over sin. One runtime entry serves both so the two
 * intrinsics share the argument reduction. */
long z_trig(long a, int want_cos) {
    long t = a % TRIG_TURN;
    if (t < 0)
        t += TRIG_TURN;
    long q = t >> 28;            /* which quarter turn */
    long r = t & (TRIG_QUARTER - 1);
    /* One turn unit is 2*pi/2^30 radians, so a Q30 radian angle is r * 2*pi. */
    long rq = (r * 6746518852L) >> 30;
    long s, c;
    trig_cordic(rq, &s, &c);
    switch (q) {
    case 0:
        return want_cos ? c : s;
    case 1:
        return want_cos ? -s : c;
    case 2:
        return want_cos ? -c : -s;
    default:
        return want_cos ? s : -c;
    }
}

/* Boxes one value on the heap and returns a pointer to it.
 *
 * A variable captured by a closure cannot live in the frame that declared it: the
 * closure may outlive the call, and a frame slot is gone the moment that call
 * returns. So a captured variable is moved into a heap cell and both the
 * enclosing function and the closure reach it through that pointer. That is what
 * makes assignment through a closure visible to the enclosing function, rather
 * than writing to a copy nobody else can see. */
void *z_box(long v) {
    long *cell = (long *)z_alloc(sizeof(long));
    *cell = v;
    return cell;
}

/* A one-word box for a float, which arrives in xmm0 rather than a general
 * register. */
void *z_box_f(double v) {
    double *cell = (double *)z_alloc(sizeof(double));
    *cell = v;
    return cell;
}

/* A box of `n` bytes, for a captured struct. The caller fills it in place. */
void *z_box_n(long n) {
    if (n < 8)
        n = 8;
    return z_alloc((size_t)n);
}

/* Allocates the cell a bound method pointer refers to: the code address and
 * the receiver, side by side. GC-managed, so the receiver is traced and stays
 * alive for as long as the pointer does. */
void *z_newbinding(void *code, void *recv) {
    void **cell = (void **)z_alloc(2 * sizeof(void *));
    cell[0] = code;
    cell[1] = recv;
    return cell;
}

/* The same two-word cell an interface value holds, under a name that says so.
 * Kept separate rather than reusing z_newbinding because the two words mean
 * different things: here they are an itab and a receiver, not a code address and
 * a receiver. The layout, and so the collector's tracing of it, is identical. */
void *z_newiface(void *itab, void *recv) {
    void **cell = (void **)z_alloc(2 * sizeof(void *));
    cell[0] = itab;
    cell[1] = recv;
    return cell;
}

/* Floor of the integer square root. A doubling loop brackets the root and a
 * binary search closes in: exact for every n, and all integer arithmetic so
 * the result does not depend on the host's floating point. */
long z_isqrt(long n) {
    if (n < 2)
        return n;
    long hi = 1;
    while (hi * hi < n)
        hi += hi;
    long lo = hi / 2;
    while (lo < hi) {
        long mid = lo + (hi - lo + 1) / 2;
        if (mid * mid <= n)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* ---- the string representation ----
 *
 * A Z `string` is a pointer to a sixteen-byte header followed by the bytes:
 *
 *     +------------------+------------------+---------+----------+
 *     | int64 len        | int64 cap        | bytes   | NUL      |
 *     +------------------+------------------+---------+----------+
 *     ^ the value                                                 ^ value+len
 *
 * The value the program holds points at the first byte, so a `string` is one
 * word, travels in a register, and is a scalar everywhere a scalar is allowed.
 *
 * Why a header rather than a bare `char *`, and why not a two-word struct
 * passed in two registers:
 *
 *   - A bare `char *` has no length, so `len(s)` is a scan, a string cannot hold
 *     an embedded zero, and every comparison has to stop at one. The header
 *     fixes all three.
 *   - A two-word struct would be 16 bytes and would have to be classified,
 *     copied, stored and returned by every path in the code generator --
 *     including the ones that cross the C boundary, where Z's convention and the
 *     System V one do not agree. A `string` that cannot cross the C boundary is
 *     not a string in a language with C interoperability, and a language whose
 *     string type needs a special case at every ABI is one that will get the
 *     special case wrong somewhere. The header keeps `string` a scalar, so
 *     `extern` and `export` keep meaning what they meant.
 *
 * The trailing NUL is maintained on every string even though nothing in Z needs
 * it, and that is the point: it is what makes a Z string valid to hand to a C
 * function taking `const char *` with no conversion and no copy. The C boundary
 * still stops at an embedded zero, because C has no way to express the length --
 * but that is a property of the boundary, not a bug in the string.
 *
 * `cap` is the allocated byte count, which is >= `len`. It is what makes a
 * builder's append amortized: a buffer that has room takes the bytes where they
 * are, and only reallocates when it is full. `cap` is also why `+` cannot grow
 * in place -- see z_concat.
 *
 * `cap == 0` means the bytes are *static*: the string is a literal, laid out in
 * the executable's .rodata by the code generator, and there is nothing to free.
 * Every heap string therefore allocates at least one byte, which is what keeps
 * the two cases apart without a flag word. A field that is only "not zero when
 * static" is the kind of implicit invariant that rots the first time someone
 * adds a path that builds an empty string on the heap, so the allocation floor
 * is here rather than spread across every constructor.
 *
 * That is also the property that makes a literal safe to hand around: a literal
 * can be stored in any number of values, copied, and returned from a function,
 * and none of those owns anything. A heap string that reaches a variable is
 * owned by exactly one, and `z_str_free` is what makes that a rule rather than
 * an intention.
 */
typedef struct ZStrHdr {
    long len;
    long cap;
} ZStrHdr;

#define ZH(b) ((ZStrHdr *)((char *)(b) - sizeof(ZStrHdr)))

/* Releases a string's bytes, if it has any.
 *
 * The compiler calls this from the destructor of every `string`-typed value when
 * its scope ends. A literal has `cap == 0` and is skipped, so storing one in a
 * local costs nothing at run time and cannot fault on the way out. */
void z_str_free(char *s) {
    if (s == NULL || ZH(s)->cap == 0)
        return;
    z_free((char *)s - sizeof(ZStrHdr));
}

/* The length in bytes. A NULL string is the empty string, so `len(null)` is 0
 * rather than a crash -- the same reading every other function here takes. */
static long zlen(const char *b) { return b != NULL ? ZH(b)->len : 0; }

/* A string with room for `cap` bytes, length 0.
 *
 * Exported below, under the name the `str_buf_new` builtin lowers to: a
 * StringBuilder in Z is a class holding one of these, and the growth policy
 * belongs next to the allocation because a string is opaque bytes to Z -- there
 * is no way to write past a string's length from Z, which is the one thing a
 * builder needs and the one thing the type deliberately does not offer. */
char *z_str_buf_new(long cap);

static char *zstr_alloc_impl(long cap) {
    if (cap < 0)
        cap = 0;
    /* One byte minimum, so `cap` is never 0 on the heap and a heap string is
     * always distinguishable from a literal. See the header comment. */
    if (cap < 1)
        cap = 1;
    /* The header goes in the block, not in front of it. It used to be written
     * sixteen bytes *before* whatever the allocator returned, which was only in
     * bounds because the collector's own 24-byte block header left room for it:
     * the header of one string was stored in the slack of the block before it.
     * A plain allocator has no slack, and the string header became a write of
     * up to sixteen bytes outside the allocation. `split` was the first program
     * to notice -- the empty piece between two separators is a fresh
     * allocation right next to its neighbour, and the neighbour's length came
     * back as garbage. */
    char *base = (char *)z_alloc(sizeof(ZStrHdr) + (size_t)cap + 1);
    ZStrHdr *h = (ZStrHdr *)base;
    h->len = 0;
    h->cap = cap;
    char *p = base + sizeof(ZStrHdr);
    p[0] = '\0';
    return p;
}

/* A fresh string holding a copy of `count` bytes of `src`. */
static char *zstr_copy(const char *src, size_t count) {
    char *out = zstr_alloc_impl((long)count);
    if (count > 0)
        memcpy(out, src, count);
    out[count] = '\0';
    ZH(out)->len = (long)count;
    return out;
}

/* Lexicographic three-way string compare, the ordering Z's <, <=, > and >=
 * use on strings.
 *
 * By (length-limited bytes, then length), which is not the same as C's strcmp:
 * strcmp stops at the first difference or first NUL, so it cannot order two
 * strings that differ only after an embedded zero, and it reports "abc" and
 * "abd" as different for the right reason but "ab" and "ab\0c" as equal for
 * the wrong one. Comparing the shared prefix and falling back to the length is
 * the ordering a reader means by "less than" when a string is a value with a
 * length. */
long z_strcmp(const char *a, const char *b) {
    long la = zlen(a), lb = zlen(b);
    long n = la < lb ? la : lb;
    if (n > 0) {
        int c = memcmp(a, b, (size_t)n);
        if (c != 0)
            return c;
    }
    if (la < lb)
        return -1;
    return la > lb ? 1 : 0;
}

/* Reports an out-of-range access and aborts. Only reachable from code compiled
 * with bounds checking enabled, so a Z program cannot scribble past the end of a
 * heap array -- and, since the string header carries a length, cannot read past
 * the end of a string either.
 *
 * `what` names the kind, because "array index 9 out of bounds (length 5)" is a
 * confusing way to be told that a string index was wrong. */
static void bounds_fail(const char *what, long idx, long len) {
    fflush(stdout);
    fprintf(stderr, "runtime error: %s index %ld out of bounds (length %ld)\n", what, idx, len);
    fflush(stderr);
    abort();
}

void z_bounds_fail(long idx, long len) { bounds_fail("array", idx, len); }

void z_str_bounds_fail(long idx, long len) { bounds_fail("string", idx, len); }

/* Heap array with an 8-byte length header; the returned pointer points at the
 * first element and the element count lives at ptr[-8].
 *
 * The elements are zeroed. `new T[n]` is documented and used as "n elements,
 * all zero" -- `foreach` over a fresh array is expected to yield n zeroes, and
 * every growable collection starts by handing out a `new T[cap]` and filling in
 * what it needs.
 *
 * The count is stored in the block rather than in front of it, for the reason
 * `zstr_alloc_impl` gives: writing it before the allocator's own pointer put it
 * outside the allocation, and only the collector's block header had been hiding
 * that. */
void *z_newarray(long count, long elemsize) {
    if (count < 0)
        count = 0;
    size_t payload = (size_t)count * (size_t)elemsize;
    unsigned char *base = (unsigned char *)z_alloc(sizeof(long) + payload);
    *(long *)base = count;
    memset(base + sizeof(long), 0, payload);
    return base + sizeof(long);
}

/* Releases a heap array from `z_newarray`. The payload pointer is what the
 * program holds, so the length header comes off first. */
void z_array_free(void *p) {
    if (p == NULL)
        return;
    z_free((char *)p - sizeof(long));
}

/* Allocates a zero-initialized class object of `size` bytes. */
void *z_newobj(long size) {
    if (size < 0)
        size = 0;
    unsigned char *p = (unsigned char *)z_alloc((size_t)size);
    for (long i = 0; i < size; i++)
        p[i] = 0;
    return p;
}

char *z_concat(char *a, char *b) {
    long la = zlen(a), lb = zlen(b);
    char *out = zstr_alloc_impl(la + lb);
    if (la > 0)
        memcpy(out, a, (size_t)la);
    if (lb > 0)
        memcpy(out + la, b, (size_t)lb);
    out[la + lb] = '\0';
    ZH(out)->len = la + lb;
    return out;
}

/* Appends to a buffer the caller *owns*, growing it geometrically.
 *
 * This is the one place a string is written in place, and it is safe only
 * because of who owns the buffer: a StringBuilder's, which nothing else can
 * reach. `z_concat` deliberately does not do this. Growing in place there would
 * make `s = s + "x"` write through to every other name bound to `s`, so a
 * language whose structs copy and whose classes share would quietly gain a
 * third set of semantics -- mutable strings that look like values. The cost is
 * one copy per `+`, which is the price of `s` meaning one thing, and the answer
 * to "appending in a loop is slow" is a builder rather than a lie.
 *
 * Geometric growth by 1.5x with a floor of 8 bytes: a doubling would be
 * equivalent amortized and wastes up to half the buffer; the 1.5x copy is cheap
 * and keeps a builder's memory closer to what it holds. */
char *z_str_buf_new(long cap) { return zstr_alloc_impl(cap); }

/* A copy of `s` with no spare capacity.
 *
 * Distinct from appending an empty string to a buffer, which is a no-op that
 * hands the buffer straight back -- correct for a builder, which owns what it
 * holds, and wrong for anything that publishes the result, because the buffer
 * would still be the builder's to grow. `cap == len` is what makes the copy
 * inert: the in-place append path can never pick it, so the copy cannot change
 * under anyone who is holding it. */
char *z_str_dup(const char *s) { return zstr_copy(s, (size_t)zlen(s)); }

char *z_str_buf_append(char *buf, const char *s) {
    long lb = zlen(s);
    if (buf == NULL)
        return zstr_copy(s, (size_t)lb);
    ZStrHdr *h = ZH(buf);
    if (h->cap - h->len < lb) {
        long need = h->len + lb;
        long cap = h->cap + h->cap / 2;
        if (cap < need)
            cap = need;
        if (cap < 8)
            cap = 8;
        char *out = zstr_alloc_impl(cap);
        memcpy(out, buf, (size_t)h->len);
        ZH(out)->len = h->len;
        buf = out;
        h = ZH(buf);
    }
    memcpy(buf + h->len, s, (size_t)lb);
    h->len += lb;
    buf[h->len] = '\0';
    return buf;
}

/* A one-byte string holding `b`, truncated to a byte. The counterpart of
 * char_at: Z has no character type, so a byte is an int in an expression and a
 * one-byte string when it has to go into a buffer. */
/* Ends the program with `code`, after flushing. There is no way for a Z program to
 * fail and say why -- there is no exception and no process exit yet -- so this is
 * what a library reaches for when it cannot continue, and what a program uses to
 * return a non-zero status. */
void z_die(long code) {
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
}

void z_exit(long code) { z_die(code); }

/* Waits for `seconds` and returns nothing.
 *
 * The deadline is absolute and taken from CLOCK_REALTIME once, before the first
 * sleep, and every slice is measured against it. A countdown re-derived from each
 * nanosleep would drift by whatever the kernel rounded each one by, and a clock
 * stepped forwards mid-wait -- which an NTP correction does routinely -- would
 * silently shorten the wait to nothing, because the sleep would be interrupted
 * with nothing left on the clock. Asking for a wait and getting a shorter one is
 * the failure that matters, so nothing here reads the clock as an authority on how
 * much is left.
 *
 * nanosleep rather than sleep, for two reasons that both matter here: sleep takes
 * an unsigned int, so a wait longer than 49 days is not expressible and a negative
 * one wraps to an enormous positive one, and sleep cannot be resumed after a signal.
 *
 * The wait is taken in slices for the same overflow reason one size down. tv_sec is
 * a time_t, which is 32 bits on plenty of targets, and a single nanosleep whose
 * tv_sec does not fit fails with EINVAL instead of waiting -- so a caller asking to
 * hold for a year would get an instant return on one of those targets and no error
 * at all. Slicing keeps every call inside what any time_t can hold.
 *
 * A signal does not shorten the wait. nanosleep rewrites the timespec it was given
 * with the part it did not sleep, so resuming from that is exact rather than
 * approximate, and it is what a caller means by asking to hold: returning early
 * would make a retry loop spin instead of waiting.
 *
 * An argument that is not a wait at all returns at once rather than reporting
 * anything. Zero is a reasonable thing to compute and pass -- `hold(0)` from an
 * empty queue is not a mistake -- and a negative one has no meaning to report,
 * since there is nothing the caller could do differently with the answer. NaN and
 * an infinity are the same: both are what dividing by a zero that was not meant to
 * be one leaves behind, and neither is a length of time. An infinity in particular
 * has to be caught here rather than treated as the very longest wait, or a
 * `hold(1.0 / 0.0)` on a value that was never meant to reach it would hang for
 * good instead of returning.
 *
 * A finite wait past what the clock can show is capped rather than refused. Such a
 * caller has made an arithmetic mistake, but one that cannot be reported usefully
 * -- `hold` returns nothing, so there is no error to hand back -- and a wait that
 * ends is closer to what was meant than an instant return. */
#define Z_HOLD_MAX_SECONDS 315576000000.0 /* ten thousand years, in seconds */

static void z_hold_until(double seconds) {
    /* Written to exclude NaN along with zero and the negatives, which is what
     * makes this the only test those need. */
    if (!(seconds > 0.0))
        return;
    /* DBL_MAX is the largest finite double there is, so being above it is exactly
     * being an infinity. Checked before the cap rather than folded into it,
     * because an infinity is not the longest wait, it is not a wait at all. */
    if (seconds > DBL_MAX)
        return;
    if (seconds > Z_HOLD_MAX_SECONDS)
        seconds = Z_HOLD_MAX_SECONDS;

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return; /* no clock to measure against, so no wait it can honour */
    long whole = (long)seconds;
    struct timespec deadline;
    deadline.tv_sec = now.tv_sec + (time_t)whole;
    deadline.tv_nsec = now.tv_nsec + (long)((seconds - (double)whole) * 1e9);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }

    for (;;) {
        if (clock_gettime(CLOCK_REALTIME, &now) != 0)
            return;
        /* What is left of the wait, as a double so that the sub-second part of the
         * deadline survives the subtraction. */
        double left = (double)(deadline.tv_sec - now.tv_sec) +
                      (double)(deadline.tv_nsec - now.tv_nsec) / 1e9;
        if (left <= 0.0)
            return;
        double slice = left > 3600.0 ? 3600.0 : left;
        long slice_whole = (long)slice;
        struct timespec ts;
        ts.tv_sec = (time_t)slice_whole;
        ts.tv_nsec = (long)((slice - (double)slice_whole) * 1e9);
        /* The truncation above can land a nanosecond outside the range nanosleep
         * accepts, and it answers that with EINVAL rather than sleeping. */
        if (ts.tv_nsec < 0)
            ts.tv_nsec = 0;
        else if (ts.tv_nsec > 999999999L)
            ts.tv_nsec = 999999999L;
        while (nanosleep(&ts, &ts) != 0) {
            if (errno != EINTR)
                return; /* nothing useful to do, and spinning would be worse */
        }
    }
}

/* A whole number of seconds. `hold(2h21m37)` arrives here as 8497, and takes the
 * same path as `hold(500ms)` rather than a second implementation of the same wait
 * that would have to be tested separately. */
void z_hold(long seconds) { z_hold_until((double)seconds); }

/* A fractional number of seconds, for a duration literal with a sub-second part. */
void z_holdf(double seconds) { z_hold_until(seconds); }

char *z_char_str(long b) {
    char *out = zstr_alloc_impl(1);
    out[0] = (char)(b & 0xff);
    out[1] = '\0';
    ZH(out)->len = 1;
    return out;
}

char *z_itoa(long v) {
    char buf[24];
    int n = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    if (u == 0)
        buf[n++] = '0';
    while (u > 0) {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg)
        buf[n++] = '-';
    char *out = zstr_alloc_impl(n);
    for (int i = 0; i < n; i++)
        out[i] = buf[n - 1 - i];
    out[n] = '\0';
    ZH(out)->len = n;
    return out;
}

/* Prints a float, the way Z's `Console.WriteLog` does for one.
 *
 * This exists so the compiler never has to make a variadic call itself. Calling
 * printf with a floating-point argument means setting %al to the number of
 * vector registers used and reserving 176 bytes of stack for the callee's
 * register save area -- two rules of the System V ABI that are easy to get
 * subtly wrong from hand-written assembly, and that a C compiler gets right for
 * free. Passing the value in xmm0 and letting C handle the rest keeps that
 * knowledge in one place.
 *
 * %g rather than %f: a float should print as the shortest thing that reads back
 * as the same number, so 1.5 is "1.5" and not "1.500000". Six significant
 * digits, so 0.1 + 0.2 prints as 0.3 -- a deliberate trade of exact digits for
 * readable ones, and the same choice every language makes by default. */
void z_print_f(double v) { printf("%g\n", v); }

/* Whether the last `z_read_byte` hit the end of the file rather than failing. A
 * separate flag because `read` reports both as -1 and the two mean opposite things
 * to a loop: one ends, the other needs reporting. */
static int z_io_last_eof = 0;

/* ---- files ----
 *
 * Descriptors, not `FILE *`. A `FILE *` is a pointer the runtime would own and the
 * program could not close, which is the ownership question this runtime does not
 * answer yet, and an opaque handle in a struct is a struct field holding a pointer
 * nobody can account for. An int has neither problem.
 *
 * These are the raw syscalls rather than stdio, because stdio buffers on one side
 * of the boundary this language cares about: a `readLine` that used `fgetc` would
 * have pulled the rest of the file into a buffer the program cannot see, and a
 * following `seek` would be wrong in a way nothing in the output would show. One
 * byte per `read` is slow and it is honest about where the buffering decision has
 * not been made yet. */

long z_open(const char *path, const char *mode) {
    /* Only the three modes a program actually asks for, spelled out rather than
     * passed through, so a mode string is never a format string. */
    int flags;
    if (strcmp(mode, "r") == 0)
        flags = O_RDONLY;
    else if (strcmp(mode, "w") == 0)
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a") == 0)
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else {
        errno = EINVAL;
        return -1;
    }
    return (long)open(path, flags, 0666);
}

long z_close(long fd) { return (long)close((int)fd); }

/* One byte, or -1 at end of file or on error. `z_io_errno` tells the two apart,
 * and `z_io_eof` says which happened, because "the read failed" and "the file
 * ended" are different answers to "keep reading". */
long z_read_byte(long fd) {
    unsigned char b = 0;
    ssize_t n = read((int)fd, &b, 1);
    if (n == 1)
        return (long)b;
    z_io_last_eof = n == 0;
    return -1;
}

long z_write(long fd, const char *s) {
    size_t n = zlen(s);
    ssize_t w = write((int)fd, s, n);
    return (long)w;
}

long z_io_errno(void) { return (long)errno; }

long z_io_eof(void) { return z_io_last_eof; }

/* ---- string library ----
 *
 * Every function here takes a Z string -- a pointer to the bytes, with the
 * length in the header -- and returns one. Four rules hold throughout:
 *
 *   - A NULL argument reads as the empty string, so `len(null)` is 0 rather than
 *     a crash, and `contains(null, "x")` is false.
 *   - Every returned string is heap-allocated, so the caller owns it and the
 *     owning value's destructor releases it. A literal is not heap-allocated and
 *     has no destructor -- see `z_str_free` for how the two are told apart.
 *   - A length is measured in bytes, not characters. Z has no character type, so
 *     a multi-byte UTF-8 sequence is three bytes, `s[0]` is its first byte, and
 *     `s.length` counts three. That is the same rule indexing has always had;
 *     what is new is that the length is stored rather than scanned for.
 *   - Out-of-range arguments clamp rather than trap: a substring request is a
 *     question about a string, and the answer to "past the end" is the empty
 *     string rather than a runtime error. The one exception is char_at, which
 *     returns -1 so that a caller walking to the end can tell it happened.
 */

/* Prints a string. Length-aware, so a string containing a zero byte prints all
 * of it -- which `puts` cannot do, since it stops at the first one. The compiler
 * calls this rather than puts so the newline is written here too and the whole
 * of `print` stays in one place. */
void z_print_str(const char *s) {
    long n = zlen(s);
    if (n > 0)
        fwrite(s, 1, (size_t)n, stdout);
    fputc('\n', stdout);
}

long z_strlen(const char *s) { return zlen(s); }

char *z_sub(const char *s, long start, long count) {
    long n = zlen(s);
    /* A negative start counts back from the end, so sub(s, -3, 3) is the last
     * three bytes. This is the one convenience worth having, because "the tail
     * of this" is otherwise the most common thing to get wrong. */
    if (start < 0)
        start += n;
    if (start < 0)
        start = 0;
    if (count < 0)
        count = 0;
    if (start > n)
        start = n;
    if (start + count > n)
        count = n - start;
    return zstr_copy(s + start, (size_t)count);
}

/* The range `s[a..b]`, with the same clamping as sub: `b` may run past the end,
 * a negative `a` counts from the end, and either end past the string gives the
 * empty string. Half-open, so the length of the result is b - a and
 * `s[0..len(s)]` is the whole string. */
char *z_slice(const char *s, long a, long b) {
    long n = zlen(s);
    if (a < 0)
        a += n;
    if (b < 0)
        b += n;
    if (a < 0)
        a = 0;
    if (b > n)
        b = n;
    if (b < a)
        b = a;
    return zstr_copy(s + a, (size_t)(b - a));
}

/* The first index at which `needle` occurs in `hay`, or -1.
 *
 * Not strstr: that stops at a zero byte, and a needle that legitimately contains
 * one is exactly the case the header exists to make expressible. A zero-length
 * needle matches at 0, which is what `indexOf(s, "")` should say. */
long z_index_of(const char *hay, const char *needle) {
    long lh = zlen(hay), ln = zlen(needle);
    if (ln == 0)
        return 0;
    if (ln > lh)
        return -1;
    long limit = lh - ln;
    for (long i = 0; i <= limit; i++) {
        if (memcmp(hay + i, needle, (size_t)ln) == 0)
            return i;
    }
    return -1;
}

long z_contains(const char *hay, const char *needle) {
    return z_index_of(hay, needle) >= 0;
}

long z_starts_with(const char *s, const char *prefix) {
    long ls = zlen(s), lp = zlen(prefix);
    return lp <= ls && memcmp(s, prefix, (size_t)lp) == 0;
}

long z_ends_with(const char *s, const char *suffix) {
    long ls = zlen(s), lx = zlen(suffix);
    return lx <= ls && memcmp(s + (ls - lx), suffix, (size_t)lx) == 0;
}

/* The byte at `i`, or -1 past the end. */
long z_char_at(const char *s, long i) {
    if (i < 0 || i >= zlen(s))
        return -1;
    /* Zero-extended, so a byte above 127 comes back as 128..255 rather than a
     * negative value. */
    return (long)(unsigned char)s[i];
}

/* The index of the first byte equal to `b`, or -1. */
long z_index_of_byte(const char *s, long b) {
    long n = zlen(s);
    for (long i = 0; i < n; i++)
        if ((long)(unsigned char)s[i] == b)
            return i;
    return -1;
}

/* The index just past the last byte equal to `b`, or -1 if there is none. */
long z_last_index_of_byte(const char *s, long b) {
    long n = zlen(s);
    for (long i = n - 1; i >= 0; i--)
        if ((long)(unsigned char)s[i] == b)
            return i + 1;
    return -1;
}

static int is_space_byte(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

char *z_trim(const char *s) {
    long n = zlen(s);
    long a = 0, b = n;
    while (a < b && is_space_byte(s[a]))
        a++;
    while (b > a && is_space_byte(s[b - 1]))
        b--;
    return zstr_copy(s + a, (size_t)(b - a));
}

char *z_trim_start(const char *s) {
    long n = zlen(s), a = 0;
    while (a < n && is_space_byte(s[a]))
        a++;
    return zstr_copy(s + a, (size_t)(n - a));
}

char *z_trim_end(const char *s) {
    long n = zlen(s), b = n;
    while (b > 0 && is_space_byte(s[b - 1]))
        b--;
    return zstr_copy(s, (size_t)b);
}

/* ASCII case mapping, deliberately not locale-aware: a program's output must not
 * depend on the environment it runs in. Bytes above 127 are left alone, since in
 * UTF-8 they are continuation bytes and case-mapping them one at a time would
 * corrupt the sequence. */
char *z_upper(const char *s) {
    long n = zlen(s);
    char *out = zstr_copy(s, (size_t)n);
    for (long i = 0; i < n; i++)
        if (out[i] >= 'a' && out[i] <= 'z')
            out[i] = (char)(out[i] - 32);
    return out;
}

char *z_lower(const char *s) {
    long n = zlen(s);
    char *out = zstr_copy(s, (size_t)n);
    for (long i = 0; i < n; i++)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] + 32);
    return out;
}

/* Replaces every occurrence of `from` with `to`. An empty `from` would match at
 * every position, so it returns the string unchanged instead of looping. */
char *z_replace(const char *s, const char *from, const char *to) {
    long lf = zlen(from), lt = zlen(to), n = zlen(s);
    if (lf == 0)
        return zstr_copy(s, (size_t)n);
    /* Count first, then allocate exactly: the collector can run during either,
     * and sizing the result up front keeps the second pass allocation-free. */
    long count = 0;
    for (long i = 0; i + lf <= n;) {
        if (memcmp(s + i, from, (size_t)lf) == 0) {
            count++;
            i += lf;
        } else {
            i++;
        }
    }
    char *out = zstr_alloc_impl(n + count * lt - count * lf);
    char *w = out;
    for (long i = 0; i < n;) {
        if (i + lf <= n && memcmp(s + i, from, (size_t)lf) == 0) {
            if (lt > 0)
                memcpy(w, to, (size_t)lt);
            w += lt;
            i += lf;
        } else {
            *w++ = s[i++];
        }
    }
    *w = '\0';
    ZH(out)->len = (long)(w - out);
    return out;
}

char *z_repeat(const char *s, long n) {
    long ls = zlen(s);
    if (n < 0)
        n = 0;
    char *out = zstr_alloc_impl(ls * n);
    for (long i = 0; i < n; i++) {
        if (ls > 0)
            memcpy(out + ls * i, s, (size_t)ls);
    }
    out[ls * n] = '\0';
    ZH(out)->len = ls * n;
    return out;
}

/* Reverses the bytes. */
char *z_reverse(const char *s) {
    long n = zlen(s);
    char *out = zstr_copy(s, (size_t)n);
    for (long i = 0; i < n / 2; i++) {
        char t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }
    return out;
}

/* Pads to `width` with `pad`, on the left for a positive width and the right
 * for a negative one. Longer than `width` is returned unchanged rather than
 * truncated: padding is a formatting request, and silently dropping the end of
 * someone's data is not what "make this 20 wide" means. */
static char *z_pad_signed(const char *s, long width, char pad) {
    long n = zlen(s);
    long target = width < 0 ? -width : width;
    if (target <= n)
        return zstr_copy(s, (size_t)n);
    long fill = target - n;
    char *out = zstr_alloc_impl(target);
    char *w = out;
    if (width > 0)
        for (long i = 0; i < fill; i++)
            *w++ = pad;
    if (n > 0)
        memcpy(w, s, (size_t)n);
    w += n;
    if (width < 0)
        for (long i = 0; i < fill; i++)
            *w++ = pad;
    *w = '\0';
    ZH(out)->len = target;
    return out;
}

/* The two spellings, rather than one function and a flag: the flag would be a
 * boolean in a signature that is otherwise all strings and numbers, and a
 * negative width reads as a mistake at every call site. */
char *z_pad_left(const char *s, long width, char pad) { return z_pad_signed(s, width, pad); }

char *z_pad_right(const char *s, long width, char pad) { return z_pad_signed(s, -width, pad); }

/* Splits on every occurrence of `sep` and returns a heap array of strings, so
 * the result is used like any other array: `parts.length`, `parts[i]`.
 *
 * Splitting an empty string yields no pieces rather than one empty piece, and an
 * empty separator splits into single characters -- both the readings that make
 * `split` composable in a loop. A trailing separator does not produce a trailing
 * empty piece, matching every other split implementation.
 *
 * The array is counted before it is allocated so the element writes cannot be
 * interleaved with a collection. The array pointer is a local throughout, which
 * is what keeps it reachable: the collector is conservative, so a pointer sitting
 * in this frame is found and marked. */
void *z_split(const char *s, const char *sep) {
    long a = zlen(s), ls = zlen(sep);

    long count = 0;
    if (ls == 0) {
        count = a; /* one piece per byte */
    } else {
        count = 1;
        for (long i = 0; i + ls <= a;) {
            if (memcmp(s + i, sep, (size_t)ls) == 0) {
                count++;
                i += ls;
                if (i >= a) {
                    count--; /* no trailing empty piece */
                    break;
                }
            } else {
                i++;
            }
        }
    }

    char **out = (char **)z_newarray(count, (long)sizeof(char *));
    long k = 0;
    if (ls == 0) {
        for (long i = 0; i < a && k < count; i++)
            out[k++] = zstr_copy(s + i, 1);
    } else {
        long i = 0;
        for (;;) {
            long piece = a;
            for (long j = i; j + ls <= a; j++) {
                if (memcmp(s + j, sep, (size_t)ls) == 0) {
                    piece = j;
                    break;
                }
            }
            if (k < count)
                out[k++] = zstr_copy(s + i, (size_t)piece - (size_t)i);
            if (piece == a)
                break;
            i = piece + ls;
            if (i >= a)
                break; /* trailing separator: stop, leaving no empty piece */
        }
    }
    return out;
}

/* The pieces of `parts` joined with `sep`, the inverse of split. `parts` is a Z
 * array with a length header, so the count comes from the header rather than
 * from a terminator -- there is no terminator to find. */
char *z_join(const char *sep, void *parts) {
    long ls = zlen(sep);
    long n = parts != NULL ? *(long *)((char *)parts - 8) : 0;
    long total = 0;
    for (long i = 0; i < n; i++)
        total += zlen(((char **)parts)[i]) + (i > 0 ? ls : 0);
    char *out = zstr_alloc_impl(total);
    char *w = out;
    for (long i = 0; i < n; i++) {
        if (i > 0 && ls > 0) {
            memcpy(w, sep, (size_t)ls);
            w += ls;
        }
        long l = zlen(((char **)parts)[i]);
        if (l > 0) {
            memcpy(w, ((char **)parts)[i], (size_t)l);
            w += l;
        }
    }
    *w = '\0';
    ZH(out)->len = total;
    return out;
}

/* Formats a float as a string, with the same six significant digits `print`
 * uses, so `"x = " + 1.5` and `Console.WriteLog(1.5)` never disagree about how a number
 * looks. */
char *z_ftoa(double v) {
    char buf[40];
    snprintf(buf, sizeof buf, "%g", v);
    return zstr_copy(buf, strlen(buf));
}

/* ---- value-to-text and value-to-hash ----
 *
 * These are the two questions that depend on a value's *type*, and the code
 * generator asks them with the type in hand: `to_text(x)` lowers to a different
 * runtime call for an int, a bool, a float, a string, an aggregate and a pointer.
 * They exist because a generic function cannot ask the question itself -- it can
 * ask `typeof(x)`, but every branch after the test still has to type-check, and
 * `int_to_string(x)` does not compile when x is a string. So the branch that
 * would have run is exactly the branch that would not compile, and the only
 * place that can answer is where the type is still known.
 *
 * A struct, an array, a union and a void print a name rather than a value: there
 * is no canonical text for an aggregate here, and a pointer address would differ
 * between two runs, which is worse than saying "this was a Point". */

char *z_int_to_text(long v) { return z_itoa(v); }

char *z_bool_to_text(long v) { return zstr_copy(v ? "true" : "false", v ? 4 : 5); }

char *z_ptr_to_text(long v) {
    char buf[32];
    snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
    return zstr_copy(buf, strlen(buf));
}

char *z_agg_to_text(const char *name) { return zstr_copy(name, strlen(name)); }

char *z_void_to_text(void) { return zstr_copy("void", 4); }

char *z_str_to_text(const char *s) { return z_str_dup(s); }

char *z_float_to_text(double v) { return z_ftoa(v); }

/* A hash of a scalar. The multiply-and-shift moves entropy downward into the low
 * bits, which are the only ones a table's mask looks at -- indexing by the raw
 * value puts 1, 2 and 3 in slots 1, 2 and 3, and every probe lands in a cluster.
 *
 * The sign bit is masked off, so -1 and the most negative int share a slot. They
 * are still different keys; they just start looking in the same place, and the
 * probe checks every slot on the run before concluding a key is absent. */
long z_hash_num(long k, long cap) {
    unsigned long v = (unsigned long)k & 0xffffffffUL;
    v = (v * 2654435761UL) & 0xffffffffUL;
    v ^= v >> 15;
    v = (v * 2246822519UL) & 0xffffffffUL;
    v ^= v >> 13;
    return (long)(v & (unsigned long)(cap - 1));
}

/* FNV-1a over the bytes, which is the same hash every language reaches for
 * because it needs nothing but a multiply and an xor. */
long z_hash_str(const char *k, long cap) {
    unsigned long h = 2166136261UL;
    long n = zlen(k);
    for (long i = 0; i < n; i++) {
        h ^= (unsigned long)(unsigned char)k[i];
        h = (h * 16777619UL) & 0x7fffffffffffffffUL;
    }
    return (long)(h & (unsigned long)(cap - 1));
}

/* ---- integer library ----
 *
 * The operations an integer-only language still wants. exp, log and the
 * transcendental functions beyond sin/cos are deliberately absent: with no
 * floating-point type they would have to invent a fixed-point convention that
 * means nothing at the call site, and sin/cos already show how that goes. They
 * belong with a real float type. */

/* Integer exponentiation by squaring. A negative exponent has no integer
 * answer, so it yields 0; a large one wraps the way Z's other arithmetic does. */
long z_pow(long base, long e) {
    if (e < 0)
        return 0;
    long result = 1;
    long b = base;
    while (e > 0) {
        if (e & 1)
            result *= b;
        b *= b;
        e >>= 1;
    }
    return result;
}

long z_gcd(long a, long b) {
    /* Take the absolute value first: gcd of 0 and anything is that thing, and
     * the C99 % keeps the sign of the dividend, which would make the loop
     * oscillate on mixed signs. */
    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    while (b != 0) {
        long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

long z_lcm(long a, long b) {
    long g = z_gcd(a, b);
    if (g == 0)
        return 0;
    long q = a / g * b;
    return q < 0 ? -q : q;
}

