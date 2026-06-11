/*
 * jl_libc.h — the freestanding C-bridge shim.
 *
 * Single include point pulled in by src/go/cbridge_all.c BEFORE the cbridge
 * *.c bodies. It first includes the full set of system headers the bridge
 * uses (so every libc declaration is in place and its include guard is set),
 * then includes the freestanding primitives, then redirects each
 * reroute­able libc call site in the bridge to an in-tree implementation via
 * an object-like macro.
 *
 * Ordering contract (unity build): system headers are included here FIRST.
 * When a cbridge body later re-includes <string.h> etc., the guard makes it a
 * no-op, so the redirect macros below only ever rewrite call sites in our own
 * code — never a system-header declaration.
 *
 * Why: see jl_syscall.h. The goal is zero reroute­able libc imports from
 * jnilog's GOT (verified by the readelf import gate in xmake.lua), so a
 * co-injected GOT-patching libc logger (libclog) cannot observe the bridge.
 *
 * The redirect macros are introduced one category per migration phase; until
 * a category's private implementation lands, its calls remain on real libc.
 */
#ifndef JL_LIBC_H
#define JL_LIBC_H

/* ── 1. system headers (complete set used across src/cbridge) ───────────── */
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <jni.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* ── 2. freestanding primitives ─────────────────────────────────────────── */
#include "jl_syscall.h"

/* ── 3. per-phase redirect macros ───────────────────────────────────────────
 * Each macro rewrites a bare libc call site in the bridge to an in-tree
 * implementation. Object-like macros only match whole tokens, so string
 * literals ("strlen"), comments, and longer identifiers (mystrlen) are
 * untouched; a prior collision scan confirmed no struct member / non-call use
 * of these names in src/cbridge.
 */

/* Phase 2: str/mem (jl_str.h / jl_mem.h). memcpy/memset/strlen may still
 * appear in `readelf` imports afterwards — those residuals come from the Go
 * runtime + cgo glue (separate TUs that don't include this shim), not from the
 * bridge. The bridge's own call sites are now private. */
#include "jl_mem.h"
#include "jl_str.h"
#define memcpy  jl_memcpy
#define memmove jl_memmove
#define memset  jl_memset
#define memcmp  jl_memcmp
#define strlen  jl_strlen
#define strcmp  jl_strcmp
#define strncmp jl_strncmp
#define strncpy jl_strncpy
#define strchr  jl_strchr
#define strrchr jl_strrchr
#define strstr  jl_strstr
#define strdup  jl_strdup

/* Phase 3: I/O (jl_io.h, raw syscalls). socketpair/send/fcntl/setsockopt/
 * getsockopt/getpid redirect via macro; the stdio + sscanf maps/cmdline
 * readers in rangeset.c/main.c and the sysconf(_SC_PAGESIZE) sites were
 * refactored directly to jl_linereader / jl_page_size / jl_parse_*. */
#include "jl_io.h"
#define send       jl_send
#define socketpair jl_socketpair
#define fcntl      jl_fcntl
#define setsockopt jl_setsockopt
#define getsockopt jl_getsockopt
#define getpid     jl_getpid

/* Phase 4: locks (jl_lock.h, futex). Redirects the mutex/rwlock/once TYPES,
 * INITIALIZERS and call sites. pthread_create/pthread_detach/pthread_t are NOT
 * redirected — the bridge spawns exactly one init-worker thread at startup, so
 * those stay on libc (accepted one-time residual). Contained entirely within
 * the cbridge unity TU; no cgo TU references these lock objects. */
#include "jl_lock.h"
#define pthread_mutex_t        jl_mutex_t
#define pthread_rwlock_t       jl_rwlock_t
#define pthread_once_t         jl_once_t
#undef  PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER  JL_MUTEX_INIT
#undef  PTHREAD_RWLOCK_INITIALIZER
#define PTHREAD_RWLOCK_INITIALIZER JL_RWLOCK_INIT
#undef  PTHREAD_ONCE_INIT
#define PTHREAD_ONCE_INIT          JL_ONCE_INIT
#define pthread_mutex_lock     jl_mutex_lock
#define pthread_mutex_unlock   jl_mutex_unlock
#define pthread_rwlock_rdlock  jl_rwlock_rdlock
#define pthread_rwlock_wrlock  jl_rwlock_wrlock
#define pthread_rwlock_unlock  jl_rwlock_unlock
#define pthread_once           jl_once

/* Phase 5: heap (jl_alloc.h, mmap segregated free lists). malloc/free/realloc
 * persist in `readelf` imports — the Go runtime + cgo glue (C.CString/C.free)
 * use them in separate TUs. The bridge's own heap is now fully private.
 * CROSS-ALLOCATOR RULE: strings the bridge hands to Go are released via
 * C.c_free_cstr (-> jl_free); only C.CString pointers use Go's C.free. */
#include "jl_alloc.h"
#define malloc  jl_malloc
#define free    jl_free
#define realloc jl_realloc

/* Phase 6: format (jl_fmt.h). The bridge only uses snprintf/vsnprintf (the
 * visualize.c fprintf is in the non-Android #else branch, dead here). Validated
 * byte-for-byte against the platform libc by tools host test test_jl_fmt.c,
 * including the %.9g/%.17g float round-trip cases. */
#include "jl_fmt.h"
#define snprintf  jl_snprintf
#define vsnprintf jl_vsnprintf

/* Phase 7: symbolize (jl_symbolize.h). Replaces the hot per-event dladdr with a
 * cached /proc/self/maps module-table lookup. dl_iterate_phdr and the two
 * one-time dlsym resolutions (android_dlopen_ext via RTLD_NEXT, the optional
 * ART symbol via RTLD_DEFAULT) are cold (init/dlopen only) and remain on libdl;
 * the Go runtime imports them regardless, so they are accepted residual. The
 * mprotect dlsym was eliminated (the interposer uses the raw syscall). */
#include "jl_symbolize.h"
#define dladdr jl_dladdr

#endif /* JL_LIBC_H */
