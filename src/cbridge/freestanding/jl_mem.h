/*
 * jl_mem.h — freestanding mem* implementations (no libc).
 *
 * Plain byte loops. Built with -fno-builtin (see xmake.lua), so the compiler
 * will NOT recognize these loops and lower them back into calls to libc
 * memcpy/memset (which would re-import the symbol and recurse). Correctness
 * over speed; the per-event cost is dwarfed by JNI + logcat.
 *
 * See jl_libc.h for how these are wired in as redirect macros.
 */
#ifndef JL_MEM_H
#define JL_MEM_H

#include <stddef.h>

static inline void *jl_memcpy(void *__restrict dst, const void *__restrict src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void *jl_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

static inline void *jl_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

static inline int jl_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (; n; n--, pa++, pb++)
        if (*pa != *pb) return (int)*pa - (int)*pb;
    return 0;
}

#endif /* JL_MEM_H */
