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

/* ── Dual-output logging ─────────────────────────────────────────────────── */

void log_message(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    goLogNative(kLogPriorityInfo, buffer);
    if (goGetLoggingReady()) goLogCallback(buffer);
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
 * log_jni_call — invoked at method entry.
 * All string params are guaranteed non-NULL by hooks.c / hook_logging.c
 * (callers pass "" rather than NULL).
 */
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
    if (!goGetLoggingReady()) return;
    if (!config_is_allowed(jni_name)) return;
    goJNICallCallback(
        offset,
        (char*)jni_name,
        receiver_kind,
        (char*)(receiver_str   ? receiver_str   : ""),
        (char*)(receiver_extra ? receiver_extra : ""),
        (char*)(class_name     ? class_name     : ""),
        (char*)(method_name    ? method_name    : ""),
        (char*)(encoded_args   ? encoded_args   : ""),
        mid,
        (char*)(caller         ? caller         : ""));
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
    if (!goGetLoggingReady()) return;
    if (!config_is_allowed(name)) return;
    goJNIReturnCallback(
        offset,
        (char*)(name     ? name     : ""),
        ret_kind,
        ret_raw,
        (char*)(ret_str  ? ret_str  : ""),
        (char*)(ret_extra ? ret_extra : ""));
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
    if (!goGetLoggingReady()) return;
    if (!config_is_allowed(lookup_type)) return;
    goJNILookupCallback(
        (char*)lookup_type,
        (char*)name,
        (char*)(sig        ? sig        : ""),
        (uintptr_t)clazz,
        (char*)(class_name ? class_name : ""),
        (char*)(caller     ? caller     : ""));
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
    if (!goGetLoggingReady()) return;
    if (!config_is_allowed("RegisterNatives")) return;

    char buf[2048];
    char* p = buf;
    char* end = buf + sizeof(buf) - 1;
    *p = '\0';

    for (int i = 0; i < n_methods && p < end; i++) {
        if (i > 0) p += snprintf(p, end - p, ", ");
        p += snprintf(p, end - p, "%s %s @%p",
                     methods[i].name      ? methods[i].name      : "?",
                     methods[i].signature ? methods[i].signature : "?",
                     methods[i].fnPtr);
    }

    goJNIRegisterNativesCallback((uintptr_t)clazz, (char*)class_name, buf, (char*)caller);
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
    if (!goGetLoggingReady()) return;
    if (!config_is_allowed(name)) return;
    goJNIFieldCallback(
        offset,
        (char*)(name           ? name           : ""),
        receiver_kind,
        (char*)(receiver_str   ? receiver_str   : ""),
        (char*)(receiver_extra ? receiver_extra : ""),
        (char*)(field_name     ? field_name     : ""),
        value_kind,
        value_raw,
        (char*)(value_str      ? value_str      : ""),
        (char*)(value_extra    ? value_extra    : ""),
        (char*)(caller         ? caller         : ""));
}
