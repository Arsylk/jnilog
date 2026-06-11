/*
 * jl_syscall.h — freestanding aarch64 Linux syscall layer (no libc).
 *
 * Part of the "freestanding C-bridge" effort: jnilog must issue zero
 * reroute­able libc calls through its own GOT so that a GOT/PLT-patching
 * libc logger co-injected into the same process (e.g. libclog) — or a
 * plthook-based packer — has nothing in jnilog's module to intercept.
 *
 * These wrappers invoke the kernel directly via `svc #0` and return the raw
 * kernel ABI value: a non-negative result on success, or the negated errno
 * (e.g. -EAGAIN) on failure. Callers MUST test the negative return directly;
 * we never touch libc `errno`/`__errno` (itself a GOT-routed import).
 *
 * aarch64 syscall ABI: x8 = syscall number, x0..x5 = args, result in x0.
 */
#ifndef JL_SYSCALL_H
#define JL_SYSCALL_H

#include <stdint.h>
#include <sys/syscall.h> /* __NR_* / SYS_* numbers (header only, no code) */

static inline long jl_syscall6(long n, long a0, long a1, long a2,
                               long a3, long a4, long a5) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ __volatile__("svc #0"
                         : "+r"(x0)
                         : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                         : "memory", "cc");
    return x0;
}

#define jl_syscall0(n)                 jl_syscall6((n), 0, 0, 0, 0, 0, 0)
#define jl_syscall1(n, a)              jl_syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define jl_syscall2(n, a, b)           jl_syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define jl_syscall3(n, a, b, c)        jl_syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define jl_syscall4(n, a, b, c, d)     jl_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define jl_syscall5(n, a, b, c, d, e)  jl_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

#endif /* JL_SYSCALL_H */
