/*
 * jl_lock.h — freestanding locks on raw futex (no libc / no pthread).
 *
 * Replaces the bridge's pthread_mutex_t / pthread_rwlock_t / pthread_once_t.
 * The whole C bridge is one translation unit (cbridge_all.c), so the type
 * redirect (pthread_mutex_t -> jl_mutex_t, …) is internally consistent; no cgo
 * TU references these lock objects, so there is no cross-TU type mismatch.
 *
 * Concurrency model:
 *  - jl_mutex: canonical 3-state futex mutex (Drepper, "Futexes Are Tricky").
 *    Non-recursive, matching the default pthread mutex the bridge used.
 *  - jl_rwlock: backed by jl_mutex (read and write both take the mutex).
 *    Correctness-first — exclusion is stricter than a true rwlock, so it can
 *    never be wrong; it only forgoes reader parallelism on the method/field
 *    caches. Revisit if Phase 8 benchmarks show contention.
 *  - jl_once: atomic CAS + futex, exactly-once init with waiters parked.
 *
 * Fork note: the bridge is dlopen'd into the already-forked app process, so
 * these locks are born fresh there; no lock is inherited across a fork.
 *
 * Futexes are PRIVATE (process-local) — the locks are never in shared memory.
 */
#ifndef JL_LOCK_H
#define JL_LOCK_H

#include <sys/syscall.h> /* __NR_futex */
#include "jl_syscall.h"

#define JL_FUTEX_WAIT_PRIVATE 128 /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG */
#define JL_FUTEX_WAKE_PRIVATE 129 /* FUTEX_WAKE | FUTEX_PRIVATE_FLAG */
#define JL_WAKE_ALL           0x7fffffff

static inline long jl__futex_wait(int *uaddr, int val) {
    return jl_syscall6(__NR_futex, (long)uaddr, JL_FUTEX_WAIT_PRIVATE, val, 0, 0, 0);
}
static inline long jl__futex_wake(int *uaddr, int n) {
    return jl_syscall6(__NR_futex, (long)uaddr, JL_FUTEX_WAKE_PRIVATE, n, 0, 0, 0);
}

/* ── mutex (3-state futex) ──────────────────────────────────────────────── */
typedef struct { int v; } jl_mutex_t; /* 0=free 1=locked 2=locked+waiters */
#define JL_MUTEX_INIT { 0 }

static inline int jl_mutex_lock(jl_mutex_t *m) {
    int c = 0;
    if (__atomic_compare_exchange_n(&m->v, &c, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0; /* uncontended: 0 -> 1 */
    /* contended: ensure state is 2 (locked+waiters) and park until free */
    if (c != 2)
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        jl__futex_wait(&m->v, 2);
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

static inline int jl_mutex_unlock(jl_mutex_t *m) {
    if (__atomic_fetch_sub(&m->v, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->v, 0, __ATOMIC_RELEASE);
        jl__futex_wake(&m->v, 1);
    }
    return 0;
}

/* ── rwlock (mutex-backed; see header note) ─────────────────────────────── */
typedef struct { jl_mutex_t m; } jl_rwlock_t;
#define JL_RWLOCK_INIT { JL_MUTEX_INIT }

static inline int jl_rwlock_rdlock(jl_rwlock_t *l) { return jl_mutex_lock(&l->m); }
static inline int jl_rwlock_wrlock(jl_rwlock_t *l) { return jl_mutex_lock(&l->m); }
static inline int jl_rwlock_unlock(jl_rwlock_t *l) { return jl_mutex_unlock(&l->m); }

/* ── once ───────────────────────────────────────────────────────────────── */
typedef struct { int state; } jl_once_t; /* 0=fresh 1=running 2=done */
#define JL_ONCE_INIT { 0 }

static inline int jl_once(jl_once_t *o, void (*init_routine)(void)) {
    if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 2) return 0;
    int expected = 0;
    if (__atomic_compare_exchange_n(&o->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        init_routine();
        __atomic_store_n(&o->state, 2, __ATOMIC_RELEASE);
        jl__futex_wake(&o->state, JL_WAKE_ALL);
    } else {
        while (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) != 2)
            jl__futex_wait(&o->state, 1);
    }
    return 0;
}

#endif /* JL_LOCK_H */
