/*
 * jl_str.h — freestanding str* implementations (no libc).
 *
 * Plain loops, built with -fno-builtin so the compiler won't re-synthesize
 * libc string/memory calls from them. See jl_libc.h for the redirect wiring.
 *
 * jl_strdup allocates with the in-tree allocator (jl_malloc), so a duplicated
 * string MUST be released with jl_free / c_free_cstr, never libc free.
 */
#ifndef JL_STR_H
#define JL_STR_H

#include <stddef.h>
#include "jl_mem.h"
#include "jl_alloc.h" /* jl_malloc for jl_strdup */

static inline size_t jl_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static inline int jl_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int jl_strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') break;
    }
    return 0;
}

/* Standard strncpy semantics: copy up to n bytes, NUL-pad the remainder, and
 * (per the standard) do NOT NUL-terminate if src is >= n. Callers in cbridge
 * follow the usual "size-1 then force dst[size-1]=0" idiom. */
static inline char *jl_strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

static inline char *jl_strchr(const char *s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char *)s;
        if (*s == '\0') return NULL;
    }
}

static inline char *jl_strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch) last = s;
        if (*s == '\0') return (char *)last;
    }
}

static inline char *jl_strstr(const char *hay, const char *needle) {
    if (*needle == '\0') return (char *)hay;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == '\0') return (char *)hay;
    }
    return NULL;
}

static inline char *jl_strdup(const char *s) {
    size_t n = jl_strlen(s) + 1;
    char *p = (char *)jl_malloc(n);
    if (p) jl_memcpy(p, s, n);
    return p;
}

#endif /* JL_STR_H */
