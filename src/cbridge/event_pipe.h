/*
 * event_pipe.h — C → Go binary event channel.
 *
 * Per-JNI-event cgo crossings into the Go runtime trip PairIP's integrity
 * VM on anti-tamper-protected apps (the cgo dispatch fires Go scheduler
 * activity detectable across thread boundaries).  This module replaces the
 * goJNI*Callback cgo path with a libc-only socketpair: log_jni_call /
 * log_jni_return / log_jni_lookup serialize their arguments into a packed
 * binary record and send() it to one end of an AF_UNIX SOCK_DGRAM pair.
 * A Go reader goroutine drains the other end and feeds the existing
 * emitCallFull / emitJNILookup renderers — final ANSI output is byte-
 * identical to the cgo-callback design.
 *
 * Wire format (little-endian, packed, single datagram per event):
 *
 *   offset  size  field
 *   ─────────────────────
 *      0     4    magic = JNIEVT_MAGIC ('JNIE')
 *      4     1    event type (see EV_*)
 *      5     1    receiver_kind  (CALL only)
 *      6     1    ret_kind       (RETURN only)
 *      7     1    nstrings  (count of strings that follow)
 *      8     4    offset (int32)
 *     12     4    reserved
 *     16     8    call_id (CALL/RETURN pairing)
 *     24     8    mid_or_raw  (mid for CALL, ret_raw for RETURN, clazz for LOOKUP)
 *     32          ── end of fixed header ──
 *     32     N    strings[i] = u16-le length + raw bytes (no NUL)
 *
 * String slots per event type:
 *   EV_CALL    : jni_name, receiver_str, receiver_extra, class_name,
 *                method_name, encoded_args, caller        (7 strings)
 *   EV_RETURN  : name, ret_str, ret_extra                  (3 strings)
 *   EV_LOOKUP  : lookup_type, name, sig, class_name, caller (5 strings)
 */

#ifndef JNILOG_EVENT_PIPE_H
#define JNILOG_EVENT_PIPE_H

#include <stdint.h>

#define JNIEVT_MAGIC  0x4A4E4945u   /* 'JNIE' little-endian */

enum jni_event_type {
    EV_CALL   = 1,
    EV_RETURN = 2,
    EV_LOOKUP = 3,
    /* EV_OBJ_RETURN: jobject return value to be rendered off-thread. */
    EV_OBJ_RETURN = 4,
    /* EV_FIELD_ACCESS: Get/Set Field event (instance or static).  Carries
     * a typed receiver + typed value, both potentially with deferred-render
     * placeholders pulled out of the TLS sidecar. */
    EV_FIELD_ACCESS = 5,
};

/* Initialize the socketpair.  Called once from bridge_init.  Returns 0 on
 * success, -errno on failure (in which case event_pipe is disabled and
 * emits are no-ops). */
int  event_pipe_init(void);

/* Returns the reader-side fd Go should drain.  Must only be called after
 * event_pipe_init() returns 0.  Returns -1 if event_pipe is disabled. */
int  event_pipe_consumer_fd(void);

/* Maximum datagram size — must match EVENT_MAX_BYTES inside event_pipe.c.
 * Exposed for the Go-side reader to size its read buffer. */
#define EVENT_PIPE_MAX_BYTES 8192

/* Render an object globalref using the consumer thread's JNIEnv*.  Returns
 * heap-allocated strings (caller frees via free()) for the class/string
 * value and toString().  *out_kind receives one of WIRE_KIND_STRING,
 * WIRE_KIND_CLASS, WIRE_KIND_OBJECT, or WIRE_KIND_NULL.  Pass NULL for
 * out_extra if toString isn't wanted.  The function calls DeleteGlobalRef
 * on gref before returning so the caller must NOT use gref afterwards. */
int  event_pipe_render_obj(
        void *consumer_env,
        uintptr_t gref,
        int *out_kind,
        char **out_str,
        char **out_extra);

/* TLS-side sidecar for per-event render refs.  Each event's encoded_args /
 * receiver_str / receiver_extra strings may contain "\x1A<n>" markers (2-byte
 * placeholders) — the Go consumer substitutes each marker with a fully
 * formatted chunk produced by vis_*ing the corresponding ref on the consumer's
 * JNIEnv*.
 *
 * Sidecar lifecycle:
 *   1. hot path: defer_render_push(env, obj) for each object to render
 *      → NewGlobalRef + record in TLS, returns slot index 0..7 (or -1 on full)
 *   2. encoder writes "\x1A<slot>" placeholder where the rendered chunk
 *      would have appeared
 *   3. event_pipe_emit_* picks up the TLS refs, appends them to the
 *      datagram, then clears the TLS state
 */
#define EVENT_PIPE_MAX_REFS 8

int  event_pipe_defer_render_push(void *env, void *obj);
void event_pipe_render_refs_reset(void);

/* Hot-path emits.  Each builds a single packed datagram and sends it on
 * the writer fd.  Returns 0 on success, -1 if event_pipe is disabled or
 * send() failed (e.g. peer not yet draining and buffer full — events are
 * dropped, never block).  All string args may be NULL → treated as "". */
int  event_pipe_emit_call(
        uint64_t call_id, int32_t offset,
        int receiver_kind, uintptr_t mid,
        const char *jni_name,
        const char *receiver_str, const char *receiver_extra,
        const char *class_name, const char *method_name,
        const char *encoded_args, const char *caller);

int  event_pipe_emit_return(
        uint64_t call_id, int32_t offset, int ret_kind,
        uintptr_t ret_raw,
        const char *name, const char *ret_str, const char *ret_extra);

int  event_pipe_emit_lookup(
        uintptr_t clazz,
        const char *lookup_type, const char *name, const char *sig,
        const char *class_name, const char *caller);

/* EV_OBJ_RETURN: defer the vis_* rendering of an object return value to the
 * Go-side consumer.  The hot path must call NewGlobalRef on the jobject and
 * pass the resulting global ref as gref; the consumer is responsible for
 * the matching DeleteGlobalRef after rendering. */
int  event_pipe_emit_obj_return(
        uint64_t call_id, int32_t offset,
        uintptr_t gref, const char *name);

/* EV_FIELD_ACCESS: route the formerly-cgo goJNIFieldCallback through the
 * socket so PairIP doesn't observe per-event cgo from Set/Get*Field hooks
 * (these are SetStaticObjectField's volume). */
int  event_pipe_emit_field_access(
        int32_t offset, const char *name,
        int receiver_kind,
        const char *receiver_str, const char *receiver_extra,
        const char *field_name,
        int value_kind, uintptr_t value_raw,
        const char *value_str, const char *value_extra,
        const char *caller);

#endif /* JNILOG_EVENT_PIPE_H */
