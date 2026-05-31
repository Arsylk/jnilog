//go:build !android

package main

// C-side unit harness (F5 test nit): the wire-encoding helpers (F9 content
// escaping, F7 boundary-safe truncation) and the F6 per-thread call-id stack
// were previously validated only by a Go reimplementation (encodeArgs) or
// on-device — so a C-specific bug (signedness, NUL-in-string, off-by-one) could
// hide. This compiles a tiny standalone C program with the host `cc` and runs
// its assertions, exercising the C compiler directly. Skipped where cc is
// absent.
//
// DRIFT NOTE: the algorithms below MIRROR the production statics and must be
// kept in sync with them:
//   - safe_trunc_len  / vea_append_escaped  → src/cbridge/{event_pipe,visualize}.c
//   - tls_push_call_id / tls_pop_call_id     → src/cbridge/bridge.c
// The Go round-trip PBT (TestWireProtocolRoundTripControlBytes) independently
// cross-checks the escape contract, so a divergence is caught from two sides.

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestCWireAlgorithms(t *testing.T) {
	cc, err := exec.LookPath("cc")
	if err != nil {
		t.Skip("cc not available; skipping C-side wire harness")
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "wire_algo_test.c")
	if err := os.WriteFile(src, []byte(cWireTestSource), 0o644); err != nil {
		t.Fatalf("write C source: %v", err)
	}
	bin := filepath.Join(dir, "wire_algo_test")
	if out, err := exec.Command(cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", bin, src).CombinedOutput(); err != nil {
		t.Fatalf("compiling C harness failed: %v\n%s", err, out)
	}
	if out, err := exec.Command(bin).CombinedOutput(); err != nil {
		t.Fatalf("C wire harness assertions failed: %v\n%s", err, out)
	}
}

const cWireTestSource = `
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ---- mirror of safe_trunc_len (event_pipe.c, F7) ---- */
static size_t safe_trunc_len(const char *s, size_t len) {
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    if (len > 0 && (unsigned char)s[len - 1] == 0x1A) len--;
    return len;
}

/* ---- mirror of the escape (vea_append_escaped, F9) + a reference decoder ---- */
static int needs_escape(unsigned char b) {
    return b==0x01||b==0x02||b==0x03||b==0x04||b==0x05||b==0x1A;
}
static size_t wire_escape(const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)in[i];
        if (needs_escape(b)) { out[o++] = 0x05; out[o++] = (char)(b ^ 0x40); }
        else out[o++] = (char)b;
    }
    return o;
}
static size_t wire_unescape(const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)in[i] == 0x05 && i + 1 < n) { out[o++] = (char)((unsigned char)in[i+1] ^ 0x40); i++; }
        else out[o++] = in[i];
    }
    return o;
}

/* ---- mirror of the per-thread call-id stack (bridge.c, F6) ---- */
#define DEPTH 32
static uint64_t ids[DEPTH];
static int depth = 0;
static void push_id(uint64_t id) { int d = depth++; if (d >= 0 && d < DEPTH) ids[d] = id; }
static uint64_t pop_id(void) { if (depth <= 0) { depth = 0; return 0; } int d = --depth; return d < DEPTH ? ids[d] : 0; }

int main(void) {
    /* truncation: ascii boundary unchanged */
    assert(safe_trunc_len("abcdefghij", 5) == 5);
    /* cut mid 2-byte UTF-8 (x C3 A9): drop the partial char */
    assert(safe_trunc_len("x\xC3\xA9yz", 2) == 1);
    /* full char kept */
    assert(safe_trunc_len("x\xC3\xA9yz", 3) == 3);
    /* 3-byte UTF-8 cut mid-sequence -> drop whole char */
    assert(safe_trunc_len("\xE0\xA4\xB9Z", 2) == 0);
    /* dangling \x1A dropped */
    { const char s[] = {'a','b',0x1A}; assert(safe_trunc_len(s, 3) == 2); }

    /* escape round-trip over every byte value, incl. the special ones */
    for (int v = 0; v < 256; v++) {
        char in = (char)v, esc[4], dec[4];
        size_t en = wire_escape(&in, 1, esc);
        size_t dn = wire_unescape(esc, en, dec);
        assert(dn == 1 && (unsigned char)dec[0] == (unsigned char)v);
        if (needs_escape((unsigned char)v)) assert(en == 2 && esc[0] == 0x05);
        else assert(en == 1);
    }
    /* escaped output never contains a raw framing/marker byte */
    { const char in[] = {0x01,0x02,'A',0x1A,0x05,0x03}; char esc[16];
      size_t en = wire_escape(in, sizeof in, esc);
      for (size_t i = 0; i < en; i++) {
          unsigned char b = (unsigned char)esc[i];
          assert(b==0x05 || (b!=0x01 && b!=0x02 && b!=0x03 && b!=0x04 && b!=0x1A));
      }
      char dec[16]; size_t dn = wire_unescape(esc, en, dec);
      assert(dn == sizeof in && memcmp(dec, in, sizeof in) == 0);
    }

    /* call-id stack: LIFO + nesting */
    depth = 0;
    push_id(10); push_id(20);            /* outer 10, inner 20 */
    assert(pop_id() == 20);              /* inner returns first */
    assert(pop_id() == 10);              /* then outer */
    assert(pop_id() == 0);               /* empty -> 0 */
    assert(depth == 0);
    /* overflow past the cap stays balanced and returns 0 for deep frames */
    for (int i = 0; i < DEPTH + 5; i++) push_id((uint64_t)(i + 1));
    for (int i = 0; i < 5; i++) assert(pop_id() == 0);     /* beyond cap: unstored */
    for (int i = DEPTH; i >= 1; i--) assert(pop_id() == (uint64_t)i);
    assert(depth == 0);
    assert(pop_id() == 0);

    printf("OK\n");
    return 0;
}
`
