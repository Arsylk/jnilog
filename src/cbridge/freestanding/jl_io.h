/*
 * jl_io.h — freestanding raw-syscall I/O + /proc text parsing (no libc).
 *
 * All wrappers invoke the kernel via jl_syscall (svc #0) and return the raw
 * kernel ABI value: >= 0 on success, or the negated errno on failure. Callers
 * test the negative return directly; we never read or set libc errno on these
 * paths (the one exception is the mprotect interposer in main.c, which is a
 * libc-ABI-compatible exported symbol and must keep errno semantics).
 *
 * Also provides a tiny buffered line reader (jl_linereader) and hex/maps
 * parsers that replace the stdio + sscanf used to read /proc/self/{maps,
 * cmdline,auxv}, so no stdio (FILE, getline, fgets, fread) or sscanf remain in
 * the bridge.
 *
 * See jl_libc.h for the redirect wiring.
 */
#ifndef JL_IO_H
#define JL_IO_H

#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>      /* O_RDONLY, O_CLOEXEC, AT_FDCWD, F_* */
#include <sys/socket.h> /* socklen_t */
#include <sys/types.h>  /* pid_t, ssize_t */
#include <sys/mman.h>   /* PROT_*, MAP_* */
#include <elf.h>        /* Elf64_auxv_t, AT_PAGESZ, AT_NULL */
#include "jl_syscall.h"

/* ── raw-syscall wrappers ───────────────────────────────────────────────── */

static inline int jl_openat(const char *path, int flags) {
    return (int)jl_syscall4(__NR_openat, AT_FDCWD, (long)path, flags, 0);
}

static inline long jl_read(int fd, void *buf, size_t n) {
    return jl_syscall3(__NR_read, fd, (long)buf, (long)n);
}

static inline int jl_close(int fd) {
    return (int)jl_syscall1(__NR_close, fd);
}

/* send(fd,buf,len,flags) == sendto(fd,buf,len,flags,NULL,0) on aarch64. */
static inline long jl_send(int fd, const void *buf, size_t len, int flags) {
    return jl_syscall6(__NR_sendto, fd, (long)buf, (long)len, flags, 0, 0);
}

static inline int jl_socketpair(int domain, int type, int proto, int sv[2]) {
    return (int)jl_syscall4(__NR_socketpair, domain, type, proto, (long)sv);
}

/* Fixed 3-arg form — the bridge only ever calls F_GETFL (arg ignored) and
 * F_SETFL (int arg), so we don't need libc fcntl's varargs. */
static inline int jl_fcntl(int fd, int cmd, int arg) {
    return (int)jl_syscall3(__NR_fcntl, fd, cmd, arg);
}

static inline int jl_setsockopt(int fd, int level, int optname,
                                const void *optval, socklen_t optlen) {
    return (int)jl_syscall5(__NR_setsockopt, fd, level, optname,
                            (long)optval, optlen);
}

static inline int jl_getsockopt(int fd, int level, int optname,
                                void *optval, socklen_t *optlen) {
    return (int)jl_syscall5(__NR_getsockopt, fd, level, optname,
                            (long)optval, (long)optlen);
}

static inline pid_t jl_getpid(void) {
    return (pid_t)jl_syscall0(__NR_getpid);
}

/* Anonymous private mapping (for the in-tree allocator). Returns NULL on
 * failure — kernel returns -errno in [-4095,-1] on error, a valid address
 * otherwise (user VAs are well below 2^48). */
static inline void *jl_mmap_anon(size_t len) {
    long r = jl_syscall6(__NR_mmap, 0, (long)len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((unsigned long)r > (unsigned long)(-4096L)) return (void *)0;
    return (void *)r;
}

static inline int jl_munmap(void *addr, size_t len) {
    return (int)jl_syscall2(__NR_munmap, (long)addr, (long)len);
}

/* Page size from the auxiliary vector (correct on 4K and 16K-page devices),
 * resolved once and cached. Fully freestanding: reads /proc/self/auxv raw
 * rather than calling sysconf()/getauxval(). */
static inline long jl_page_size(void) {
    static long cached = 0;
    if (cached) return cached;
    long ps = 4096;
    int fd = jl_openat("/proc/self/auxv", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        Elf64_auxv_t av;
        while (jl_read(fd, &av, sizeof(av)) == (long)sizeof(av)) {
            if (av.a_type == AT_PAGESZ) {
                if (av.a_un.a_val) ps = (long)av.a_un.a_val;
                break;
            }
            if (av.a_type == AT_NULL) break;
        }
        jl_close(fd);
    }
    cached = ps;
    return ps;
}

/* ── buffered line reader (replaces fopen/fgets/getline) ────────────────── */

typedef struct {
    int    fd;
    int    eof;
    size_t pos;
    size_t len;
    char   buf[4096];
} jl_linereader;

/* Returns fd (>=0) on success, negative on failure. */
static inline int jl_lr_open(jl_linereader *lr, const char *path, int flags) {
    lr->fd = jl_openat(path, flags);
    lr->eof = 0;
    lr->pos = 0;
    lr->len = 0;
    return lr->fd;
}

/* Read the next '\n'-delimited line into `out` (NUL-terminated, newline
 * stripped). Returns out, or NULL at EOF. A line longer than outsz-1 is
 * truncated and its remainder discarded up to the newline, so the next call
 * starts on a fresh line (never a fragment). Size `out` generously for long
 * split-APK maps paths. */
static inline char *jl_lr_next(jl_linereader *lr, char *out, size_t outsz) {
    if (lr->fd < 0 || outsz == 0) return NULL;
    size_t oi = 0;
    int got_any = 0;
    for (;;) {
        if (lr->pos >= lr->len) {
            if (lr->eof) break;
            long r = jl_read(lr->fd, lr->buf, sizeof(lr->buf));
            if (r <= 0) { lr->eof = 1; break; }
            lr->len = (size_t)r;
            lr->pos = 0;
        }
        got_any = 1;
        char c = lr->buf[lr->pos++];
        if (c == '\n') break;
        if (oi + 1 < outsz) out[oi++] = c; /* else discard overflow byte */
    }
    if (!got_any && oi == 0) return NULL;
    out[oi] = '\0';
    return out;
}

static inline void jl_lr_close(jl_linereader *lr) {
    if (lr->fd >= 0) { jl_close(lr->fd); lr->fd = -1; }
}

/* ── hex / maps-line parsing (replaces sscanf) ──────────────────────────── */

/* Parse a run of hex digits at *p; advance *p past them; return the value. */
static inline unsigned long jl_parse_hex(const char **p) {
    unsigned long v = 0;
    const char *s = *p;
    for (;;) {
        char c = *s;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        s++;
    }
    *p = s;
    return v;
}

/* Parse the "lo-hi perms" head of a /proc/self/maps line (replaces
 * sscanf(line,"%lx-%lx %4s",...)). Writes up to 4 perm chars + NUL into
 * perms[5]. Returns 1 on success, 0 on malformed input. */
static inline int jl_parse_maps_head(const char *line, uintptr_t *lo,
                                     uintptr_t *hi, char perms[5]) {
    const char *p = line;
    *lo = (uintptr_t)jl_parse_hex(&p);
    if (*p != '-') return 0;
    p++;
    *hi = (uintptr_t)jl_parse_hex(&p);
    while (*p == ' ' || *p == '\t') p++;
    int i = 0;
    for (; i < 4 && p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n'; i++)
        perms[i] = p[i];
    perms[i] = '\0';
    return i > 0;
}

#endif /* JL_IO_H */
