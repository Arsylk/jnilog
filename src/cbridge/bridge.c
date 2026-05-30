/*
 * JNILog - C Bridge
 *
 * Thin C wrappers that cannot exist in Go:
 *   - Variadic log_native_* wrappers (va_list can't cross cgo)
 *   - Typed log_jni_* forwarders to Go exports
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>
#if __has_include(<android/log.h>)
#include <android/log.h>
#endif
#include "bridge.h"
#include "hook_internal.h"
#include "event_pipe.h"
#include "_cgo_export.h"

static const char* k_log_tag = "JNILogPayload";
static pthread_once_t g_go_bridge_once = PTHREAD_ONCE_INIT;

/* ── Native logging (pre-Go-init safe) ───────────────────────────────────── */

static void vlog_native(int priority, const char* format, va_list args) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    goLogNative(priority, buffer);
}

void log_native(int priority, const char* format, ...) {
    va_list args; va_start(args, format);
    vlog_native(priority, format, args);
    va_end(args);
}
void log_native_info(const char* format, ...) {
    va_list args; va_start(args, format);
    vlog_native(kLogPriorityInfo, format, args);
    va_end(args);
}
void log_native_warn(const char* format, ...) {
    va_list args; va_start(args, format);
    vlog_native(kLogPriorityWarn, format, args);
    va_end(args);
}
void log_native_error(const char* format, ...) {
    va_list args; va_start(args, format);
    vlog_native(kLogPriorityError, format, args);
    va_end(args);
}

/* ── Bridge lifecycle ────────────────────────────────────────────────────── */

static const char* (*art_field_get_name)(void*) = NULL;

static void resolve_art_symbols(void) {
    /* Try multiple mangled names for art::ArtField::GetName across ART versions:
     *   _ZN3art8ArtField7GetNameEv        — Android 5-13, non-const
     *   _ZNK3art8ArtField7GetNameEv        — Android 14+, const-qualified
     *   _ZN3art9ArtField7GetNameEv         — older/alternative ABI
     */
    static const char* const kArtFieldGetNameSyms[] = {
        "_ZN3art8ArtField7GetNameEv",
        "_ZNK3art8ArtField7GetNameEv",
        "_ZN3art9ArtField7GetNameEv",
    };
    for (size_t i = 0; i < sizeof(kArtFieldGetNameSyms) / sizeof(kArtFieldGetNameSyms[0]); i++) {
        art_field_get_name = (const char* (*)(void*))dlsym(RTLD_DEFAULT, kArtFieldGetNameSyms[i]);
        if (art_field_get_name) {
            LOG_DIRECT(ANDROID_LOG_INFO, "Resolved ArtField::GetName via %s at %p",
                       kArtFieldGetNameSyms[i], (void*)art_field_get_name);
            return;
        }
    }
    LOG_DIRECT(ANDROID_LOG_WARN, "Failed to resolve art::ArtField::GetName (tried %zu variants)",
               sizeof(kArtFieldGetNameSyms) / sizeof(kArtFieldGetNameSyms[0]));
}

void bridge_init(void) {
    LOG_DIRECT(ANDROID_LOG_INFO, "=== JNILog Bridge Init (v3 typed) ===");
    // NOTE: Do NOT call resolve_loader_symbols_once() here.
    // bridge_init() is called from the background init thread while dlopen() is still in
    // progress (the linker lock is held). Calling dlsym(RTLD_NEXT, ...) from a thread that
    // is not the dlopen caller would deadlock on Android's global linker mutex.
    // Each interposition wrapper (dlopen, android_dlopen_ext, mprotect) already calls
    // pthread_once(&loader_symbol_once, resolve_loader_symbols_once) lazily on first use.
    resolve_art_symbols();
    int rc = event_pipe_init();
    if (rc == 0) {
        LOG_DIRECT(ANDROID_LOG_INFO, "event_pipe ready: consumer_fd=%d", event_pipe_consumer_fd());
    } else {
        LOG_DIRECT(ANDROID_LOG_ERROR, "event_pipe init failed: %d", rc);
    }
}

static void bridge_activate_go_once(void) {
    goBridgeInit();
}

void bridge_activate_go(void) {
    pthread_once(&g_go_bridge_once, bridge_activate_go_once);
}

void bridge_cleanup_with_env(void *env) {
    restore_jni_hooks(env);
    goBridgeCleanup();
}

const char* art_get_field_name(void* field_id) {
    if (art_field_get_name && field_id) return art_field_get_name(field_id);
    return NULL;
}

/* ============================================================================
 * Typed JNI event forwarders
 *
 * Each function guards on goGetLoggingReady() then calls the matching
 * Go export with typed components.  No string pre-formatting happens here.
 * ============================================================================ */

/*
 * Call/return pairing via monotonic atomic counter.
 *
 * Each log_jni_call increments g_call_id_counter and stashes the value in a
 * __thread slot.  log_jni_return reads the same TLS slot, so the same thread's
 * matching return picks up the exact ID without a global map or mutex.  The
 * ID is passed to both Go callbacks; the Go side uses it as a sync.Map key
 * to pair the call frame with its result for emit.
 *
 * This replaces the old design of a globally-locked tid→[]callFrame map whose
 * mutex acquire/release pattern across thousands of JNI events per second
 * was a major source of Go scheduler activity.
 *
 * Recursion within a single thread (rare — should_log_from_caller gates
 * vis_* helper re-entry) would corrupt the TLS slot.  We don't pair
 * nested calls because the outer call's ID is overwritten; the outer
 * return then sees ID=0 and emits as unmatched.  Acceptable: this is
 * vastly less common than the typical call/return on the same thread.
 */
static uint64_t            g_call_id_counter = 0;
static __thread uint64_t   tls_last_call_id  = 0;

/* Cache goGetLoggingReady() at C-side after the first true return.  Once
 * jnilog has handed off Go-side initialization to the user, the value never
 * flips back to false, so a single cgo crossing is enough to know "ready
 * forever".  Without this cache, every JNI hook event fires a cgo callback
 * just to ask "is logging on?" — and on PairIP-protected apps that's enough
 * cgo activity per second to trip the integrity VM. */
static int g_logging_ready_cached = 0;
static inline int logging_ready_fast(void) {
    if (__atomic_load_n(&g_logging_ready_cached, __ATOMIC_ACQUIRE)) return 1;
    if (goGetLoggingReady()) {
        __atomic_store_n(&g_logging_ready_cached, 1, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}

void log_jni_call(
        int offset,
        const char* jni_name,
        int receiver_kind,
        const char* receiver_str,
        const char* receiver_extra,
        const char* class_name,
        const char* method_name,
        const char* encoded_args,
        uintptr_t mid,
        const char* caller) {
    if (!logging_ready_fast()) return;
    if (!config_is_allowed(jni_name)) return;
    uint64_t cid = __atomic_add_fetch(&g_call_id_counter, 1, __ATOMIC_RELAXED);
    tls_last_call_id = cid;
    /* Push to the C→Go event socket — no cgo on this path. */
    event_pipe_emit_call(cid, offset, receiver_kind, mid,
                         jni_name, receiver_str, receiver_extra,
                         class_name, method_name, encoded_args, caller);
}

/*
 * log_jni_return — invoked after a method returns.
 * ret_kind is wire_kind_t cast to int; ret_raw carries primitive bits or
 * object pointer bits; ret_str / ret_extra carry object metadata.
 */
void log_jni_return(
        int offset,
        const char* name,
        int ret_kind,
        uintptr_t ret_raw,
        const char* ret_str,
        const char* ret_extra) {
    if (!logging_ready_fast()) return;
    if (!config_is_allowed(name)) return;
    uint64_t cid = tls_last_call_id;
    tls_last_call_id = 0;
    event_pipe_emit_return(cid, offset, ret_kind, ret_raw,
                           name, ret_str, ret_extra);
}

/*
 * log_jni_lookup — FindClass / GetMethodID / GetFieldID events.
 * These go through the lookup path (emitJNILookup), not the call/return stack.
 */
void log_jni_lookup(
        const char* lookup_type,
        const char* name,
        const char* sig,
        void* clazz,
        const char* class_name,
        const char* caller) {
    if (!logging_ready_fast()) return;
    if (!config_is_allowed(lookup_type)) return;
    event_pipe_emit_lookup((uintptr_t)clazz, lookup_type, name, sig,
                           class_name, caller);
}

/* Deferred variant: NewGlobalRef the clazz so the Go consumer can render the
 * class name off-thread using its own attached JNIEnv*.  Hooks should prefer
 * this over log_jni_lookup wherever they currently call vis_class_name solely
 * to feed log_jni_lookup. */
void log_jni_lookup_deferred(
        void* env,
        const char* lookup_type,
        const char* name,
        const char* sig,
        void* clazz,
        const char* caller) {
    if (!logging_ready_fast()) return;
    if (!config_is_allowed(lookup_type)) return;
    JNIEnv *je = (JNIEnv*)env;
    /* Defer (and thus NewGlobalRef) only when a consumer is attached to render +
     * free the gref, and only commit ownership if the datagram is delivered;
     * otherwise free it ourselves so a dropped lookup never leaks a gref (F1). */
    if (je && clazz && event_pipe_consumer_ready()) {
        void *gref = (void*)(*je)->NewGlobalRef(je, clazz);
        if (gref) {
            /* Empty class_name + non-zero clazz tells the consumer to render. */
            if (event_pipe_emit_lookup((uintptr_t)gref, lookup_type, name, sig, "", caller) != 0) {
                (*je)->DeleteGlobalRef(je, gref);   /* dropped — we still own it */
            }
            return;
        }
    }
    /* No consumer / no gref: emit with the raw clazz pointer as an opaque
     * display id and no resolvable class name. */
    event_pipe_emit_lookup((uintptr_t)clazz, lookup_type, name, sig, "", caller);
}

/*
 * log_jni_register_natives — RegisterNatives event.
 * Formats the method list as a comma-separated "name sig @ptr" string,
 * identical to the original (the Go side still parses this text for
 * RegisterNatives display since there's no strong type benefit there).
 */
void log_jni_register_natives(
        jclass clazz,
        const char* class_name,
        const JNINativeMethod* methods,
        int n_methods,
        const char* caller) {
    if (!logging_ready_fast()) return;
    if (!config_is_allowed("RegisterNatives")) return;

    /* Build a comma-separated "name sig @ptr" list, bounded against buf[2048].
     * Note: snprintf returns the would-have-written length, not the actual.
     * The previous `p += snprintf(p, end - p, ...)` form advanced p past end
     * on truncation, then `end - p` (ptrdiff_t) sign-extended to ~SIZE_MAX on
     * the next iteration → the next snprintf wrote the entire formatted
     * output past the stack buffer. App-controlled methods[i].name /
     * signature (no JNI-imposed length limit) triggered the overflow. */
    char buf[2048];
    size_t pos = 0;
    buf[0] = '\0';
#define REGNAT_APPEND(fmt, ...) do { \
    if (pos + 1 >= sizeof(buf)) break; \
    size_t _room = sizeof(buf) - pos; /* includes NUL */ \
    int _w = snprintf(buf + pos, _room, fmt, ##__VA_ARGS__); \
    if (_w < 0) break; \
    if ((size_t)_w >= _room) { pos = sizeof(buf) - 1; break; } \
    pos += (size_t)_w; \
} while (0)

    for (int i = 0; i < n_methods; i++) {
        if (i > 0) REGNAT_APPEND("%s", ", ");
        if (pos + 1 >= sizeof(buf)) break;
        REGNAT_APPEND("%.200s %.400s @%p",
                      methods[i].name      ? methods[i].name      : "?",
                      methods[i].signature ? methods[i].signature : "?",
                      methods[i].fnPtr);
        if (pos + 1 >= sizeof(buf)) break;
    }
#undef REGNAT_APPEND
    buf[pos] = '\0';

    event_pipe_emit_register_natives((uintptr_t)clazz, class_name, buf, caller);
}

/*
 * log_jni_field_access — Get/SetField events.
 * Both receiver and value are fully typed.
 */
void log_jni_field_access(
        int offset,
        const char* name,
        int receiver_kind,
        const char* receiver_str,
        const char* receiver_extra,
        const char* field_name,
        int value_kind,
        uintptr_t value_raw,
        const char* value_str,
        const char* value_extra,
        const char* caller) {
    if (!logging_ready_fast()) return;
    if (!config_is_allowed(name)) return;
    event_pipe_emit_field_access(offset, name,
                                 receiver_kind, receiver_str, receiver_extra,
                                 field_name,
                                 value_kind, value_raw, value_str, value_extra,
                                 caller);
}
