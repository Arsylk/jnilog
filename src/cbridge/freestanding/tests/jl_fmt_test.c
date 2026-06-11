/*
 * jl_fmt_test.c — host regression test for the freestanding jl_vsnprintf.
 *
 * jl_fmt.h is the highest-risk piece of the freestanding C-bridge (a hand-rolled
 * vsnprintf, incl. long-double-based %g). This compiles it on the host and
 * diffs every conversion the bridge uses against the platform libc snprintf.
 *
 *   build & run:  xmake fmttest      (or: cc -O2 jl_fmt_test.c && ./a.out)
 *
 * jl_fmt.h is self-contained (stdarg/stddef/stdint only), so no Android NDK is
 * needed. The one deliberate divergence from glibc is %p of NULL: the bridge
 * targets bionic, which prints "0x0" (glibc prints "(nil)") — asserted below.
 */
#include <stdio.h>
#include <string.h>
#include "../jl_fmt.h"

static int fails = 0, total = 0;

/* compare jl_snprintf vs the host libc snprintf for the same args */
#define CK(fmt, ...) do {                                              \
    char a[128], b[128];                                               \
    int ra = jl_snprintf(a, sizeof a, fmt, __VA_ARGS__);              \
    int rb = snprintf(b, sizeof b, fmt, __VA_ARGS__);                 \
    total++;                                                           \
    if (strcmp(a, b) != 0 || ra != rb) {                             \
        fails++;                                                       \
        printf("MISMATCH fmt=\"%s\"\n  jl =[%s] (ret %d)\n  libc=[%s] (ret %d)\n", \
               fmt, a, ra, b, rb);                                     \
    }                                                                 \
} while (0)

/* assert jl_snprintf produces exactly `want` (for intentional libc divergences) */
#define WANT(want, fmt, ...) do {                                     \
    char a[128];                                                      \
    jl_snprintf(a, sizeof a, fmt, __VA_ARGS__);                      \
    total++;                                                          \
    if (strcmp(a, want) != 0) {                                      \
        fails++;                                                      \
        printf("WANT mismatch fmt=\"%s\": jl=[%s] want=[%s]\n", fmt, a, want); \
    }                                                                \
} while (0)

int main(void) {
    /* integers: conversions, length modifiers, flags, width, precision */
    CK("%d", 0); CK("%d", 42); CK("%d", -42); CK("%d", (int)-2147483648);
    CK("%u", 4000000000u); CK("%x", 0xdeadbeef); CK("%X", 0xabc);
    CK("%lx", (unsigned long)0x7fab12345678ull); CK("%lld", -1234567890123LL);
    CK("%zu", (size_t)123456789); CK("%o", 0755); CK("% o", 8u);
    CK("%5d", 42); CK("%-5d|", 42); CK("%05d", 42); CK("%+d", 42);
    CK("%08x", 0x1234u); CK("%.5d", 42); CK("%#x", 255u); CK("%#o", 64u);
    CK("api=%d pid=%d", 36, 27413); CK("%lx-%lx", 0x7d0fUL, 0x7d10UL);

    /* strings / char / pointer */
    CK("%s", "hello"); CK("%.3s", "hello"); CK("%.400s", "short");
    CK("[%10s]", "hi"); CK("[%-10s]", "hi"); CK("%c", 'Q');
    CK("%s=%s", "key", "value"); CK("%.200s", "abcdef");
    CK("%p", (void *)0x7d0f250f18ull);
    WANT("0x0", "%p", (void *)0); /* bionic form (glibc: "(nil)") */

    /* floats: the round-trip precisions the bridge uses (%.9g / %.17g) */
    double ds[] = {0.0, -0.0, 1.0, -1.0, 0.5, 3.14159265358979, 1e10, 1e-10,
                   123456.789, 0.0001, 0.00001, 9.999999, 2.5, 1.0/3.0,
                   1234567890123456.0, 6.022e23, 1.6e-19, 100.0, 0.1, 255.255,
                   1e100, 1e-100, 9.999999999999999e22};
    for (unsigned i = 0; i < sizeof(ds)/sizeof(ds[0]); i++) {
        CK("%.9g", (double)(float)ds[i]);
        CK("%.17g", ds[i]);
    }
    float fs[] = {1.5f, 0.1f, 3.14159f, 1e20f, 2.0f, 0.333333f};
    for (unsigned i = 0; i < sizeof(fs)/sizeof(fs[0]); i++)
        CK("%.9g", (double)fs[i]);

    printf("\n%d/%d passed, %d mismatch(es)\n", total - fails, total, fails);
    return fails ? 1 : 0;
}
