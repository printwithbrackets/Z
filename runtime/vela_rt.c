/* Vela runtime: a conservative mark-sweep tracing garbage collector plus the
 * array/string helpers the compiler emits calls to.
 *
 * The collector is *conservative*: it scans the machine stack and the spilled
 * register set for words that look like pointers into the managed heap. This
 * means the compiler needs no shadow-stack bookkeeping — every live Vela
 * pointer is, by construction, a machine word somewhere on the stack or in a
 * callee-saved register. It is linked into every compiled program. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct GcBlock {
    struct GcBlock *next;
    size_t size;        /* payload bytes */
    unsigned char mark; /* 1 = reachable this cycle */
} GcBlock;

/* Each managed allocation is a GcBlock header followed by `size` payload
 * bytes. The payload pointer handed to the program is header+1. */
#define PAYLOAD(b) ((void *)((char *)(b) + sizeof(GcBlock)))
#define BLOCK_OF(p) ((GcBlock *)((char *)(p) - sizeof(GcBlock)))

static GcBlock *gc_head = NULL;
static size_t gc_bytes = 0;
static size_t gc_threshold = 1u << 20; /* collect after ~1 MiB live */
static char *gc_stack_base = NULL;     /* high-water mark of the C stack */
static int gc_enabled = 1;

static void gc_mark(void *p);

static int gc_is_heap_ptr(void *p) {
    for (GcBlock *b = gc_head; b != NULL; b = b->next) {
        void *lo = PAYLOAD(b);
        if ((char *)p >= (char *)lo && (char *)p < (char *)lo + b->size)
            return 1;
    }
    return 0;
}

static void gc_mark_ptrs_in(const void *lo, const void *hi) {
    const char *c = (const char *)lo;
    c = (const char *)((uintptr_t)c & ~(uintptr_t)7); /* align to 8 */
    for (; c + 8 <= (const char *)hi; c += 8) {
        void *candidate;
        memcpy(&candidate, c, sizeof candidate);
        if (candidate != NULL && gc_is_heap_ptr(candidate))
            gc_mark(candidate);
    }
}

static void gc_mark(void *p) {
    if (gc_is_heap_ptr(p) == 0)
        return;
    GcBlock *b = BLOCK_OF(p);
    if (b->mark)
        return;
    b->mark = 1;
    /* Trace interior references: an object may point to other objects. */
    gc_mark_ptrs_in(PAYLOAD(b), (char *)PAYLOAD(b) + b->size);
}

/* Conservative root scan: spilled registers + the live C stack region. */
static void gc_mark_roots(void) {
    jmp_buf regs;
    setjmp(regs); /* forces live registers into the jmp_buf */
    gc_mark_ptrs_in(&regs, (char *)&regs + sizeof regs);

    char here;
    const char *lo = &here;
    const char *hi = gc_stack_base ? gc_stack_base : lo;
    if (hi > lo) {
        gc_mark_ptrs_in(lo, hi);
    }
}

static void gc_collect(void) {
    if (gc_head == NULL)
        return;
    for (GcBlock *b = gc_head; b != NULL; b = b->next)
        b->mark = 0;
    gc_mark_roots();
    GcBlock **link = &gc_head;
    size_t live = 0;
    while (*link != NULL) {
        GcBlock *b = *link;
        if (b->mark) {
            live += b->size;
            link = &b->next;
        } else {
            *link = b->next;
            gc_bytes -= b->size;
            free(b);
        }
    }
    gc_threshold = live * 2;
    if (gc_threshold < (1u << 20))
        gc_threshold = 1u << 20;
}

/* Records the high-water mark of the C stack. The compiler calls this at
 * program entry (in the generated entry function) so the conservative root
 * scan covers every live Vela frame, not just the deepest allocation site. */
void vela_gc_init(void) {
    char probe;
    gc_stack_base = &probe;
}

static void *gc_alloc(size_t size) {
    if (size == 0)
        size = 1;
    if (gc_stack_base == NULL) {
        char probe;
        gc_stack_base = &probe; /* fallback if init was not called */
    }
    if (gc_enabled && gc_bytes + size > gc_threshold)
        gc_collect();
    GcBlock *b = (GcBlock *)malloc(sizeof(GcBlock) + size);
    if (b == NULL) {
        fprintf(stderr, "vela: out of memory\n");
        exit(1);
    }
    b->size = size;
    b->mark = 0;
    b->next = gc_head;
    gc_head = b;
    gc_bytes += size;
    return PAYLOAD(b);
}

/* ---- compiler-facing runtime API ---- */

/* Heap array with an 8-byte length header; the returned pointer points at the
 * first element and the element count lives at ptr[-8]. */
void *vela_newarray(long count, long elemsize) {
    if (count < 0)
        count = 0;
    unsigned char *base =
        (unsigned char *)gc_alloc(sizeof(long) + (size_t)count * (size_t)elemsize);
    *(long *)base = count;
    return base + sizeof(long);
}

/* Allocates a zero-initialized class object of `size` bytes (GC-managed). */
void *vela_newobj(long size) {
    if (size < 0)
        size = 0;
    unsigned char *p = (unsigned char *)gc_alloc((size_t)size);
    for (long i = 0; i < size; i++)
        p[i] = 0;
    return p;
}

char *vela_concat(char *a, char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *out = (char *)gc_alloc(la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

char *vela_itoa(long v) {
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
    char *out = (char *)gc_alloc((size_t)n + 1);
    for (int i = 0; i < n; i++)
        out[i] = buf[n - 1 - i];
    out[n] = '\0';
    return out;
}

/* Explicit collection hook (the collector also runs automatically on growth). */
void vela_gc(void) { gc_collect(); }
