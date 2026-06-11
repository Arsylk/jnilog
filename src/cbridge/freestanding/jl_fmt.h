/*
 * jl_fmt.h — freestanding vsnprintf/snprintf (no libc).
 *
 * Supports exactly what the bridge uses (audited): conversions d i u o x X c s
 * p % and the float form %g (used as %.9g / %.17g for jfloat/jdouble); flags
 * - 0 + space #; field width; precision (min-digits for ints, max-chars for
 * strings, significant-digits for %g); length modifiers l ll z j t (and h/hh
 * accepted). Output is bounded like C99 snprintf: at most cap-1 bytes are
 * written, the result is always NUL-terminated when cap>0, and the return is
 * the number of bytes that WOULD have been written.
 *
 * Floating point: digits are extracted in `long double` (binary128 on aarch64,
 * 80-bit on the x86-64 host test), whose mantissa dwarfs the 17 significant
 * decimal digits a double needs, so %.17g round-trips. long double arithmetic
 * lowers to compiler-rt soft-float (__multf3 etc.) — NOT libc, and statically
 * linked — so it adds no reroute­able libc import. See jl_libc.h for wiring.
 */
#ifndef JL_FMT_H
#define JL_FMT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { char *buf; size_t cap; size_t len; } jl_fmt_sink;

static inline void jl_fmt_putc(jl_fmt_sink *s, char c) {
    if (s->cap && s->len < s->cap - 1) s->buf[s->len] = c;
    s->len++;
}
static inline void jl_fmt_pad(jl_fmt_sink *s, char c, int n) {
    while (n-- > 0) jl_fmt_putc(s, c);
}

/* unsigned integer (base 8/10/16) with C printf flags/width/precision */
static void jl_fmt_uint(jl_fmt_sink *s, unsigned long long v, unsigned base,
                        int upper, int width, int prec, int has_prec,
                        int left, int zero, const char *pfx) {
    char d[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!(has_prec && prec == 0 && v == 0)) {
        do { d[n++] = digits[v % base]; v /= base; } while (v);
    }
    int plen = 0;
    while (pfx && pfx[plen]) plen++;
    int zpad = (has_prec && prec > n) ? prec - n : 0;
    int body = n + zpad + plen;
    int wpad = width > body ? width - body : 0;
    if (!left && !zero) jl_fmt_pad(s, ' ', wpad);
    for (int i = 0; i < plen; i++) jl_fmt_putc(s, pfx[i]);
    if (!left && zero && !has_prec) { jl_fmt_pad(s, '0', wpad); wpad = 0; }
    jl_fmt_pad(s, '0', zpad);
    while (n > 0) jl_fmt_putc(s, d[--n]);
    if (left) jl_fmt_pad(s, ' ', wpad);
}

static void jl_fmt_int(jl_fmt_sink *s, long long val, int width, int prec,
                       int has_prec, int left, int zero, int plus, int space) {
    unsigned long long mag = val < 0 ? (unsigned long long)(-(val + 1)) + 1ull
                                     : (unsigned long long)val;
    const char *pfx = val < 0 ? "-" : plus ? "+" : space ? " " : "";
    jl_fmt_uint(s, mag, 10, 0, width, prec, has_prec, left, zero, pfx);
}

/* %g of `value` with `prec` significant digits (prec<=0 => 1). Writes a decimal
 * string into `out` (caller buffer, >= 32 bytes). Returns length. */
static int jl_fmt_g(char *out, double value, int prec) {
    int o = 0;
    union { double d; uint64_t u; } bits; bits.d = value;
    int neg = (int)(bits.u >> 63);
    uint64_t exp = (bits.u >> 52) & 0x7ff;
    uint64_t man = bits.u & 0xfffffffffffffull;
    if (exp == 0x7ff) { /* inf / nan */
        if (neg && man == 0) out[o++] = '-';
        const char *t = man ? "nan" : "inf";
        for (int i = 0; t[i]; i++) out[o++] = t[i];
        out[o] = '\0';
        return o;
    }
    if (neg) out[o++] = '-';
    if (prec < 1) prec = 1;
    if (prec > 17) prec = 17;

    if (exp == 0 && man == 0) { out[o++] = '0'; out[o] = '\0'; return o; } /* ±0 */

    long double m = (long double)(value < 0 ? -value : value);
    int e10 = 0;
    while (m >= 10.0L) { m /= 10.0L; e10++; }
    while (m < 1.0L)   { m *= 10.0L; e10--; }

    /* extract prec+1 digits (last is the rounding guard) */
    char dig[20];
    for (int i = 0; i < prec; i++) {
        int dd = (int)m;
        if (dd < 0) dd = 0;
        if (dd > 9) dd = 9;
        dig[i] = (char)('0' + dd);
        m = (m - dd) * 10.0L;
    }
    int round_up = ((int)m >= 5);
    if (round_up) {
        int i = prec - 1;
        for (; i >= 0; i--) {
            if (dig[i] == '9') { dig[i] = '0'; }
            else { dig[i]++; break; }
        }
        if (i < 0) { /* carry past the top: 9.99..->1.00.. */
            for (int k = prec - 1; k > 0; k--) dig[k] = dig[k - 1];
            dig[0] = '1';
            e10++;
        }
    }
    /* strip trailing zeros (%g) */
    int ndig = prec;
    while (ndig > 1 && dig[ndig - 1] == '0') ndig--;

    if (e10 < -4 || e10 >= prec) {
        /* %e style: d.ddde±NN */
        out[o++] = dig[0];
        if (ndig > 1) { out[o++] = '.'; for (int i = 1; i < ndig; i++) out[o++] = dig[i]; }
        out[o++] = 'e';
        int ev = e10;
        out[o++] = ev < 0 ? '-' : '+';
        if (ev < 0) ev = -ev;
        char eb[6]; int en = 0;
        do { eb[en++] = (char)('0' + ev % 10); ev /= 10; } while (ev);
        while (en < 2) eb[en++] = '0';            /* at least 2 exponent digits */
        while (en > 0) out[o++] = eb[--en];
    } else if (e10 >= 0) {
        /* integer part has e10+1 digits */
        int ip = e10 + 1;
        for (int i = 0; i < ip; i++) out[o++] = i < ndig ? dig[i] : '0';
        if (ndig > ip) { out[o++] = '.'; for (int i = ip; i < ndig; i++) out[o++] = dig[i]; }
    } else {
        /* 0.00ddd */
        out[o++] = '0'; out[o++] = '.';
        for (int i = 0; i < -e10 - 1; i++) out[o++] = '0';
        for (int i = 0; i < ndig; i++) out[o++] = dig[i];
    }
    out[o] = '\0';
    return o;
}

static int jl_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    jl_fmt_sink s = { buf, cap, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { jl_fmt_putc(&s, *p); continue; }
        p++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else break;
        }
        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); p++; if (width < 0) { left = 1; width = -width; } }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = 0, has_prec = 0;
        if (*p == '.') {
            has_prec = 1; p++;
            if (*p == '*') { prec = va_arg(ap, int); p++; if (prec < 0) has_prec = 0; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0; /* 0=int,1=long,2=longlong,3=size_t */
        for (;;) {
            if (*p == 'l') { lng = lng >= 1 ? 2 : 1; p++; }
            else if (*p == 'z' || *p == 'j' || *p == 't') { lng = 3; p++; }
            else if (*p == 'h') { p++; } /* promoted; ignore */
            else break;
        }
        char c = *p;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng == 0 ? (long long)va_arg(ap, int)
                       : lng == 1 ? (long long)va_arg(ap, long)
                                  : va_arg(ap, long long);
            jl_fmt_int(&s, v, width, prec, has_prec, left, zero, plus, space);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = lng == 0 ? (unsigned long long)va_arg(ap, unsigned)
                                : lng == 1 ? (unsigned long long)va_arg(ap, unsigned long)
                                : lng == 3 ? (unsigned long long)va_arg(ap, size_t)
                                           : va_arg(ap, unsigned long long);
            unsigned base = c == 'o' ? 8 : (c == 'u' ? 10 : 16);
            const char *pfx = "";
            if (alt && base == 16 && v) pfx = (c == 'X') ? "0X" : "0x";
            else if (alt && base == 8 && v) pfx = "0"; /* #o: force leading 0 */
            jl_fmt_uint(&s, v, base, c == 'X', width, prec, has_prec, left, zero, pfx);
            break;
        }
        case 'p': {
            /* bionic prints "0x0" for NULL (glibc prints "(nil)"); match the
             * device libc so output stays byte-identical to a bionic build. */
            void *ptr = va_arg(ap, void *);
            jl_fmt_uint(&s, (unsigned long long)(uintptr_t)ptr, 16, 0, width, 0, 0, left, zero, "0x");
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int wpad = width > 1 ? width - 1 : 0;
            if (!left) jl_fmt_pad(&s, ' ', wpad);
            jl_fmt_putc(&s, ch);
            if (left) jl_fmt_pad(&s, ' ', wpad);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int n = 0;
            while (str[n] && (!has_prec || n < prec)) n++;
            int wpad = width > n ? width - n : 0;
            if (!left) jl_fmt_pad(&s, ' ', wpad);
            for (int i = 0; i < n; i++) jl_fmt_putc(&s, str[i]);
            if (left) jl_fmt_pad(&s, ' ', wpad);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            /* The bridge only ever uses %g (as %.9g / %.17g). e/E/f/F/a/A are
             * accepted but rendered in %g style — if a caller ever needs true
             * %f/%e/%a formatting, jl_fmt_g must be extended to honor the
             * conversion char. */
            double v = va_arg(ap, double);
            char tmp[40];
            int n = jl_fmt_g(tmp, v, has_prec ? prec : 6);
            int wpad = width > n ? width - n : 0;
            if (!left) jl_fmt_pad(&s, ' ', wpad);
            for (int i = 0; i < n; i++) jl_fmt_putc(&s, tmp[i]);
            if (left) jl_fmt_pad(&s, ' ', wpad);
            break;
        }
        case '%': jl_fmt_putc(&s, '%'); break;
        case '\0': p--; break; /* trailing '%' */
        default:   jl_fmt_putc(&s, '%'); jl_fmt_putc(&s, c); break;
        }
    }
    if (cap) s.buf[s.len < cap ? s.len : cap - 1] = '\0';
    return (int)s.len;
}

static int jl_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = jl_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

#endif /* JL_FMT_H */
