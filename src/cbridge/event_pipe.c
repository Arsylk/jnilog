/*
 * event_pipe.c — see event_pipe.h for the protocol description.
 *
 * Design notes:
 *
 * - The writer fd is non-blocking with a 1 MB SNDBUF.  If the Go reader is
 *   ever behind, send() returns EAGAIN and the event is dropped silently.
 *   We never block the calling thread — anti-tamper VMs would notice the
 *   stall just as readily as they notice cgo cadence.
 *
 * - Each emit builds the record on the stack.  Max datagram size is
 *   EVENT_MAX_BYTES; strings are truncated past their per-slot limit.  The
 *   limits are generous (Go-side renderers truncate further if needed).
 *
 * - send() to AF_UNIX SOCK_DGRAM preserves message boundaries — every
 *   read() on the consumer end returns exactly one event record, no
 *   framing needed on the Go side beyond parsing the fixed header.
 */

#include "event_pipe.h"
#include "visualize.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <jni.h>

/* Minimal direct logcat for the one-shot init readback (F24).  event_pipe.c is
 * the first file in the concatenated TU, so we keep a local guarded macro
 * rather than depending on bridge.h ordering. */
#if __has_include(<android/log.h>)
#include <android/log.h>
#define EVP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "JniLog", __VA_ARGS__)
#else
#define EVP_LOG(...) ((void)0)
#endif

/* Per-string max bytes when packing into the datagram.  Strings longer
 * than this are truncated.  Generous enough that the Go-side rendering
 * never sees a string the user-visible logger would otherwise have. */
#define EV_STR_MAX_BYTES   512
/* Hard upper bound on a single datagram.  Sum of fixed header + maximum
 * string payloads + their length prefixes, rounded up.  Generous. */
#define EVENT_MAX_BYTES    8192
/* Writer-side SNDBUF target (best-effort; SO_SNDBUF rounds to kernel
 * allocator quantum).  Sized to hold ~hundreds of mid-sized events
 * during burst load before EAGAIN drops kick in. */
#define EVENT_SNDBUF       (1 << 20)   /* 1 MiB */

#define HDR_FIXED_BYTES    32

/* Build-time guarantee that a maximal event still fits one datagram, so emits
 * never silently overflow (F7).  7 = most strings in any event (EV_CALL /
 * EV_FIELD_ACCESS); 2 = the u16 length prefix per string; the sidecar is
 * 1 byte nrefs + EVENT_PIPE_MAX_REFS × 8.  This 7×512 bound also dominates the
 * EV_REGISTER_NATIVES shape (512 + 2048 + 512). */
_Static_assert(HDR_FIXED_BYTES
               + 7 * (2 + EV_STR_MAX_BYTES)
               + (1 + EVENT_PIPE_MAX_REFS * 8)
               <= EVENT_MAX_BYTES,
               "maximal event must fit EVENT_MAX_BYTES");

/* Deferred-render slot is encoded on the wire as the two bytes "\x1A" then
 * (uint8_t)(slot+1) — the +1 keeps the slot byte non-zero so it survives the
 * NUL-terminated C string pipeline (a raw 0 would terminate the encoder
 * string).  That leaves room for slots 0..253 (F8), well past any plausible
 * EVENT_PIPE_MAX_REFS. */
_Static_assert(EVENT_PIPE_MAX_REFS <= 254,
               "deferred-render slot must fit a single non-zero byte (slot+1)");

static int g_writer_fd  = -1;
static int g_reader_fd  = -1;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set to 1 by the Go reader after a successful AttachCurrentThreadAsDaemon,
 * back to 0 if the reader loop exits.  Gates deferred-gref creation (F1) so a
 * gref is never minted for a consumer that will never render or free it.
 * Plain int + __atomic_* to match the project's existing atomic discipline. */
static int g_consumer_ready = 0;

/* Monotonic count of datagrams dropped on a full writer socket (F24). */
static uint64_t g_drop_count = 0;

int event_pipe_consumer_ready(void) {
    return __atomic_load_n(&g_consumer_ready, __ATOMIC_ACQUIRE);
}
void event_pipe_set_consumer_ready(int ready) {
    __atomic_store_n(&g_consumer_ready, ready ? 1 : 0, __ATOMIC_RELEASE);
}
uint64_t event_pipe_drops(void) {
    return __atomic_load_n(&g_drop_count, __ATOMIC_RELAXED);
}

/* Per-thread sidecar for deferred-render refs.  `env` is the hook thread's
 * JNIEnv* captured at push time, so the emit path can DeleteGlobalRef the refs
 * itself when the carrying datagram is dropped (F1) — the hook thread always
 * has a valid env, and every ref in one event is pushed from that same thread. */
struct tls_render_refs {
    uint8_t   nrefs;
    uintptr_t refs[EVENT_PIPE_MAX_REFS];
    JNIEnv   *env;
};
static __thread struct tls_render_refs g_tls_refs = {0};

int event_pipe_defer_render_push(void *env, void *obj) {
    if (!env || !obj) return -1;
    /* Never mint a gref unless a consumer is attached to render + free it. */
    if (!event_pipe_consumer_ready()) return -1;
    if (g_tls_refs.nrefs >= EVENT_PIPE_MAX_REFS) return -1;
    JNIEnv *je = (JNIEnv*)env;
    void *gref = (void*)(*je)->NewGlobalRef(je, obj);
    if (!gref) return -1;
    int slot = g_tls_refs.nrefs++;
    g_tls_refs.refs[slot] = (uintptr_t)gref;
    g_tls_refs.env = je;
    return slot;
}

/* Success path: the consumer now owns the refs and will DeleteGlobalRef them
 * after rendering.  Just forget them locally. */
static void event_pipe_render_refs_reset(void) {
    g_tls_refs.nrefs = 0;
    g_tls_refs.env   = NULL;
}

/* Failure path: the datagram carrying these refs was never delivered, so the
 * hook thread still owns them — free them here (we stashed a valid env at push
 * time) so a dropped/overflowed event never leaks a global ref (F1). */
static void event_pipe_render_refs_drop(void) {
    JNIEnv *je = g_tls_refs.env;
    if (je) {
        for (uint8_t i = 0; i < g_tls_refs.nrefs; i++) {
            (*je)->DeleteGlobalRef(je, (jobject)g_tls_refs.refs[i]);
        }
    }
    g_tls_refs.nrefs = 0;
    g_tls_refs.env   = NULL;
}

/* ── little-endian writers (no host-byte-order assumptions) ─────────── */
static inline void put_u8 (uint8_t  *p, size_t off, uint8_t  v)  { p[off] = v; }
static inline void put_u16(uint8_t  *p, size_t off, uint16_t v)  { p[off]=v; p[off+1]=v>>8; }
static inline void put_u32(uint8_t  *p, size_t off, uint32_t v)  { p[off]=v; p[off+1]=v>>8; p[off+2]=v>>16; p[off+3]=v>>24; }
static inline void put_u64(uint8_t  *p, size_t off, uint64_t v)  {
    for (int i = 0; i < 8; i++) p[off + i] = (uint8_t)(v >> (i * 8));
}
static inline void put_i32(uint8_t  *p, size_t off, int32_t  v)  { put_u32(p, off, (uint32_t)v); }

/* Given that we intend to keep the first `len` bytes of NUL-terminated `s`
 * (len <= strlen(s)), back the cut off to a safe boundary so a truncated field
 * never desyncs the wire (F7).  Three hazards, all of which the blind clip used
 * to hit:
 *   1. UTF-8: if the first dropped byte is a continuation byte (10xxxxxx) the
 *      multibyte char straddles the cut — retreat to its char boundary so Go
 *      never sees invalid UTF-8 / a replacement char.
 *   2. A trailing lone "\x1A" deferred-render marker whose slot byte was cut
 *      off — drop it so the Go substituter never mistakes the next byte for a
 *      slot and the sidecar count never disagrees with the placeholders.
 *   3. A trailing lone "\x05" F9 escape lead-in (vea_append_escaped) whose
 *      companion byte was cut off — drop it so Go's unescapeWireContent never
 *      appends the bare 0x05 literally (a stray ENQ at the field tail). */
static size_t safe_trunc_len(const char *s, size_t len) {
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) {
        len--;
    }
    /* 0x05 is always an escape lead-in and raw 0x1A content is itself escaped
     * (to 0x05 0x5A), so a lone trailing 0x05/0x1A is always a malformed half,
     * never legitimate data.  Loop so an adjacent marker+escape pair straddling
     * the cut can't leave a dangling byte behind. */
    while (len > 0 && ((unsigned char)s[len - 1] == 0x1A ||
                       (unsigned char)s[len - 1] == 0x05)) {
        len--;
    }
    return len;
}

/* Append a length-prefixed string to buf at *pos, truncating at max_bytes on a
 * safe boundary (see safe_trunc_len).  s may be NULL → treated as empty string.
 * Returns 0 on success or -1 if the buffer would overflow. */
static int append_str_max(uint8_t *buf, size_t *pos, size_t cap,
                          const char *s, size_t max_bytes) {
    if (!s) s = "";
    size_t len = strlen(s);
    if (len > max_bytes) len = safe_trunc_len(s, max_bytes);
    if (*pos + 2 + len > cap) return -1;
    put_u16(buf, *pos, (uint16_t)len);
    if (len) memcpy(buf + *pos + 2, s, len);
    *pos += 2 + len;
    return 0;
}

/* Default-cap append (EV_STR_MAX_BYTES). */
static int append_str(uint8_t *buf, size_t *pos, size_t cap, const char *s) {
    return append_str_max(buf, pos, cap, s, EV_STR_MAX_BYTES);
}

/* Append the TLS sidecar refs (if any) to the datagram.  Sidecar wire layout:
 * 1 byte nrefs, then nrefs × 8 bytes gref.  Returns the new pos, or 0 on
 * overflow.  Does NOT clear the TLS sidecar — ownership of the grefs is
 * resolved by finish_record() based on whether send() actually delivered the
 * datagram (consumer keeps them) or not (we free them) (F1). */
static size_t append_render_refs(uint8_t *buf, size_t pos, size_t cap) {
    uint8_t n = g_tls_refs.nrefs;
    if (pos + 1 + (size_t)n * 8 > cap) {
        return 0;
    }
    buf[pos++] = n;
    for (int i = 0; i < (int)n; i++) {
        put_u64(buf, pos, (uint64_t)g_tls_refs.refs[i]);
        pos += 8;
    }
    return pos;
}

/* ── header build ────────────────────────────────────────────────────── */
static size_t put_header(uint8_t *buf,
                         uint8_t event_type,
                         uint8_t receiver_kind, uint8_t ret_kind,
                         uint8_t nstrings,
                         int32_t offset,
                         uint64_t call_id, uint64_t mid_or_raw)
{
    put_u32(buf, 0,  JNIEVT_MAGIC);
    put_u8 (buf, 4,  event_type);
    put_u8 (buf, 5,  receiver_kind);
    put_u8 (buf, 6,  ret_kind);
    put_u8 (buf, 7,  nstrings);
    put_i32(buf, 8,  offset);
    put_u32(buf, 12, 0);                /* reserved */
    put_u64(buf, 16, call_id);
    put_u64(buf, 24, mid_or_raw);
    return HDR_FIXED_BYTES;
}

/* ── lifecycle ──────────────────────────────────────────────────────── */
int event_pipe_init(void) {
    if (g_writer_fd >= 0) return 0;     /* idempotent */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) != 0) {
        return -errno;
    }
    /* Writer non-blocking — never stall the JNI hook on backpressure. */
    int flags = fcntl(sv[0], F_GETFL, 0);
    if (flags >= 0) fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    /* Larger SNDBUF tolerates burst loads from PairIP's JNI dispatch loop. */
    int sndbuf = EVENT_SNDBUF;
    (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int rcvbuf = EVENT_SNDBUF;
    (void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    g_writer_fd = sv[0];
    g_reader_fd = sv[1];
    /* Read back the kernel-granted buffer sizes (the kernel clamps to
     * wmem_max/rmem_max and typically doubles the request for bookkeeping) so
     * the real capacity is observable rather than guessed (F24). */
    int got_snd = 0, got_rcv = 0;
    socklen_t sl = sizeof(got_snd);
    (void)getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &got_snd, &sl);
    sl = sizeof(got_rcv);
    (void)getsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &got_rcv, &sl);
    EVP_LOG("event_pipe: SNDBUF requested=%d granted=%d; RCVBUF requested=%d granted=%d",
            sndbuf, got_snd, rcvbuf, got_rcv);
    return 0;
}

int event_pipe_consumer_fd(void) { return g_reader_fd; }

/* ── send helper ────────────────────────────────────────────────────── */
static int send_record(const uint8_t *buf, size_t len) {
    if (g_writer_fd < 0) return -1;
    /* AF_UNIX SOCK_DGRAM send() is atomic per message — no partial writes.
     * Mutex protects against concurrent send() that could interleave at
     * the kernel level on some implementations; it does NOT serialize the
     * hot path significantly because send() itself is fast. */
    pthread_mutex_lock(&g_send_lock);
    ssize_t n = send(g_writer_fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&g_send_lock);
    if (n == (ssize_t)len) return 0;
    /* EAGAIN/EWOULDBLOCK — Go reader behind; drop the event. */
    __atomic_add_fetch(&g_drop_count, 1, __ATOMIC_RELAXED);
    return -1;
}

/* Finalize an emit: append the TLS sidecar, send the datagram, and resolve
 * gref ownership.  On success the consumer owns the sidecar grefs (it renders
 * then DeleteGlobalRefs them); on any failure (sidecar overflow or a dropped
 * datagram) we DeleteGlobalRef them here so nothing leaks (F1).  All emit bufs
 * are sized EVENT_MAX_BYTES.  Returns 0 on success, -1 if the event was
 * dropped. */
static int finish_record(uint8_t *buf, size_t pos) {
    pos = append_render_refs(buf, pos, EVENT_MAX_BYTES);
    if (pos == 0) {
        event_pipe_render_refs_drop();
        return -1;
    }
    if (send_record(buf, pos) != 0) {
        event_pipe_render_refs_drop();
        return -1;
    }
    event_pipe_render_refs_reset();
    return 0;
}

/* ── emit functions ─────────────────────────────────────────────────── */
int event_pipe_emit_call(
        uint64_t call_id, int32_t offset,
        int receiver_kind, uintptr_t mid,
        const char *jni_name,
        const char *receiver_str, const char *receiver_extra,
        const char *class_name, const char *method_name,
        const char *encoded_args, const char *caller)
{
    uint8_t buf[EVENT_MAX_BYTES];
    size_t  pos = put_header(buf, EV_CALL,
                             (uint8_t)receiver_kind, 0,
                             /* nstrings = */ 7,
                             offset, call_id, (uint64_t)mid);
    if (append_str(buf, &pos, sizeof(buf), jni_name)        ||
        append_str(buf, &pos, sizeof(buf), receiver_str)    ||
        append_str(buf, &pos, sizeof(buf), receiver_extra)  ||
        append_str(buf, &pos, sizeof(buf), class_name)      ||
        append_str(buf, &pos, sizeof(buf), method_name)     ||
        append_str(buf, &pos, sizeof(buf), encoded_args)    ||
        append_str(buf, &pos, sizeof(buf), caller)) {
        event_pipe_render_refs_drop();
        return -1;
    }
    return finish_record(buf, pos);
}

int event_pipe_emit_return(
        uint64_t call_id, int32_t offset, int ret_kind,
        uintptr_t ret_raw,
        const char *name, const char *ret_str, const char *ret_extra)
{
    uint8_t buf[EVENT_MAX_BYTES];
    size_t  pos = put_header(buf, EV_RETURN,
                             0, (uint8_t)ret_kind,
                             /* nstrings = */ 3,
                             offset, call_id, (uint64_t)ret_raw);
    if (append_str(buf, &pos, sizeof(buf), name)     ||
        append_str(buf, &pos, sizeof(buf), ret_str)  ||
        append_str(buf, &pos, sizeof(buf), ret_extra)) {
        event_pipe_render_refs_drop();
        return -1;
    }
    return finish_record(buf, pos);
}

int event_pipe_emit_lookup(
        uintptr_t clazz,
        const char *lookup_type, const char *name, const char *sig,
        const char *class_name, const char *caller)
{
    uint8_t buf[EVENT_MAX_BYTES];
    size_t  pos = put_header(buf, EV_LOOKUP,
                             0, 0,
                             /* nstrings = */ 5,
                             0, 0, (uint64_t)clazz);
    if (append_str(buf, &pos, sizeof(buf), lookup_type) ||
        append_str(buf, &pos, sizeof(buf), name)        ||
        append_str(buf, &pos, sizeof(buf), sig)         ||
        append_str(buf, &pos, sizeof(buf), class_name)  ||
        append_str(buf, &pos, sizeof(buf), caller)) {
        event_pipe_render_refs_drop();
        return -1;
    }
    return finish_record(buf, pos);
}

int event_pipe_emit_field_access(
        int32_t offset, const char *name,
        int receiver_kind,
        const char *receiver_str, const char *receiver_extra,
        const char *field_name,
        int value_kind, uintptr_t value_raw,
        const char *value_str, const char *value_extra,
        const char *caller)
{
    uint8_t buf[EVENT_MAX_BYTES];
    /* Pack receiver_kind in the "receiver_kind" header byte and value_kind in
     * the "ret_kind" byte. value_raw goes in the mid_or_raw slot. */
    size_t pos = put_header(buf, EV_FIELD_ACCESS,
                            (uint8_t)receiver_kind, (uint8_t)value_kind,
                            /* nstrings = */ 6,
                            offset, 0, (uint64_t)value_raw);
    if (append_str(buf, &pos, sizeof(buf), name)            ||
        append_str(buf, &pos, sizeof(buf), receiver_str)    ||
        append_str(buf, &pos, sizeof(buf), receiver_extra)  ||
        append_str(buf, &pos, sizeof(buf), field_name)      ||
        append_str(buf, &pos, sizeof(buf), value_str)       ||
        append_str(buf, &pos, sizeof(buf), value_extra)     ||
        append_str(buf, &pos, sizeof(buf), caller)) {
        event_pipe_render_refs_drop();
        return -1;
    }
    return finish_record(buf, pos);
}

int event_pipe_emit_register_natives(
        uintptr_t clazz,
        const char *class_name, const char *methods, const char *caller)
{
    uint8_t buf[EVENT_MAX_BYTES];
    size_t  pos = put_header(buf, EV_REGISTER_NATIVES,
                             0, 0,
                             /* nstrings = */ 3,
                             0, 0, (uint64_t)clazz);
    /* methods is the pre-built "name sig @ptr, ..." list (bounded at 2048 by
     * the hook-thread builder); allow the full list so RegisterNatives output
     * matches the old cgo path byte-for-byte rather than clipping at 512. */
    if (append_str(buf, &pos, sizeof(buf), class_name)             ||
        append_str_max(buf, &pos, sizeof(buf), methods, 2048)      ||
        append_str(buf, &pos, sizeof(buf), caller)) {
        event_pipe_render_refs_drop();
        return -1;
    }
    return finish_record(buf, pos);
}

int event_pipe_emit_obj_return(
        uint64_t call_id, int32_t offset,
        uintptr_t gref, const char *name)
{
    uint8_t buf[EVENT_MAX_BYTES];
    size_t  pos = put_header(buf, EV_OBJ_RETURN,
                             0, 0,
                             /* nstrings = */ 1,
                             offset, call_id, (uint64_t)gref);
    if (append_str(buf, &pos, sizeof(buf), name)) {
        event_pipe_render_refs_drop();
        return -1;
    }
    return finish_record(buf, pos);
}

/* WIRE_KIND_* values are defined in bridge.h via hook_internal.h. */
#include "hook_internal.h"

int event_pipe_render_obj(
        void *consumer_env,
        uintptr_t gref,
        int *out_kind,
        char **out_str,
        char **out_extra)
{
    JNIEnv *env = (JNIEnv*)consumer_env;
    void *obj = (void*)gref;
    if (out_str)   *out_str   = NULL;
    if (out_extra) *out_extra = NULL;
    if (out_kind)  *out_kind  = WIRE_KIND_NULL;
    if (!env || !obj) return -1;

    int kind; char *str = NULL, *extra = NULL;
    if (vis_is_string(env, obj)) {
        kind = WIRE_KIND_STRING;
        str  = vis_string_value_raw(env, obj);
    } else if (vis_is_class(env, obj)) {
        kind = WIRE_KIND_CLASS;
        str  = vis_class_name(env, obj);
    } else {
        kind = WIRE_KIND_OBJECT;
        str   = vis_object_class_name(env, obj);
        extra = vis_object_tostring(env, obj);
    }
    if (out_kind)  *out_kind  = kind;
    if (out_str)   *out_str   = str;   else free(str);
    if (out_extra) *out_extra = extra; else free(extra);

    /* Release the global ref — caller no longer owns it. */
    (*env)->DeleteGlobalRef(env, obj);
    return 0;
}
