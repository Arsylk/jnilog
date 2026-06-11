/*
 * jl_alloc.h — freestanding heap on mmap (no libc).
 *
 * Segregated power-of-two free lists (classes 16 B .. 64 KiB) carved from
 * 1 MiB anonymous superblocks; allocations larger than 64 KiB get a dedicated
 * mmap. One global futex mutex guards the lists. Correctness over speed; the
 * bridge's allocations are small, short-lived strings plus a little config.
 *
 * CROSS-ALLOCATOR RULE: a pointer from jl_malloc MUST be released with jl_free,
 * never libc free, and vice versa. The bridge's heap is fully internal except
 * where C strings cross the cgo boundary — those sites free via the C-side
 * c_free()/event_pipe_free_cstr() wrappers (which call jl_free), never Go's
 * C.free. See jl_libc.h and the Go callers.
 *
 * See jl_io.h (jl_mmap_anon/jl_munmap) and jl_lock.h (jl_mutex).
 */
#ifndef JL_ALLOC_H
#define JL_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include "jl_io.h"
#include "jl_lock.h"
#include "jl_mem.h"

#define JL_ALLOC_MIN        16u
#define JL_ALLOC_MAX_CLASS  65536u        /* 2^16; larger => dedicated mmap */
#define JL_ALLOC_NCLASS     13            /* 2^4 .. 2^16 inclusive */
#define JL_ALLOC_ARENA      (1u << 20)    /* 1 MiB superblocks */

typedef struct jl_hdr {
    size_t usable; /* usable bytes (== class size for small allocations) */
    size_t mlen;   /* >0: large block; value is the mmap length to munmap.
                    *  0: arena block. While the block is free, this field
                    *  doubles as the intrusive free-list `next` pointer. */
} jl_hdr;

static jl_mutex_t jl_alloc_lock = JL_MUTEX_INIT;
static void      *jl_alloc_free[JL_ALLOC_NCLASS];
static char      *jl_alloc_bump;
static char      *jl_alloc_end;

static inline unsigned jl_size_to_pow2(size_t n) {
    if (n <= JL_ALLOC_MIN) return JL_ALLOC_MIN;
    return 1u << (unsigned)(64 - __builtin_clzll((unsigned long long)(n - 1)));
}

static inline int jl_class_index(size_t usable) {
    return (int)(__builtin_ctzll((unsigned long long)usable) - 4); /* 16 -> 0 */
}

/* caller holds jl_alloc_lock */
static inline void *jl_arena_bump(size_t total) {
    if (jl_alloc_bump == 0 || jl_alloc_bump + total > jl_alloc_end) {
        size_t chunk = JL_ALLOC_ARENA;
        if (total > chunk) chunk = (total + 4095) & ~(size_t)4095;
        char *base = (char *)jl_mmap_anon(chunk);
        if (!base) return (void *)0;
        jl_alloc_bump = base;
        jl_alloc_end  = base + chunk;
    }
    void *p = jl_alloc_bump;
    jl_alloc_bump += total;
    return p;
}

static inline void *jl_malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > JL_ALLOC_MAX_CLASS) {
        size_t mlen = (sizeof(jl_hdr) + n + 4095) & ~(size_t)4095;
        jl_hdr *h = (jl_hdr *)jl_mmap_anon(mlen);
        if (!h) return (void *)0;
        h->usable = mlen - sizeof(jl_hdr);
        h->mlen   = mlen;
        return (char *)h + sizeof(jl_hdr);
    }
    unsigned usable = jl_size_to_pow2(n);
    int idx = jl_class_index(usable);
    jl_mutex_lock(&jl_alloc_lock);
    jl_hdr *h = (jl_hdr *)jl_alloc_free[idx];
    if (h) {
        jl_alloc_free[idx] = (void *)h->mlen; /* pop: next was in mlen */
    } else {
        h = (jl_hdr *)jl_arena_bump(sizeof(jl_hdr) + usable);
    }
    jl_mutex_unlock(&jl_alloc_lock);
    if (!h) return (void *)0;
    h->usable = usable;
    h->mlen   = 0;
    return (char *)h + sizeof(jl_hdr);
}

static inline void jl_free(void *p) {
    if (!p) return;
    jl_hdr *h = (jl_hdr *)((char *)p - sizeof(jl_hdr));
    if (h->mlen) { jl_munmap(h, h->mlen); return; }
    int idx = jl_class_index(h->usable);
    jl_mutex_lock(&jl_alloc_lock);
    h->mlen = (size_t)jl_alloc_free[idx]; /* push: stash next in mlen */
    jl_alloc_free[idx] = (void *)h;
    jl_mutex_unlock(&jl_alloc_lock);
}

static inline void *jl_realloc(void *p, size_t n) {
    if (!p) return jl_malloc(n);
    if (n == 0) { jl_free(p); return (void *)0; }
    jl_hdr *h = (jl_hdr *)((char *)p - sizeof(jl_hdr));
    if (n <= h->usable) return p;
    void *np = jl_malloc(n);
    if (!np) return (void *)0;
    jl_memcpy(np, p, h->usable);
    jl_free(p);
    return np;
}

#endif /* JL_ALLOC_H */
