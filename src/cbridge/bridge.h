#ifndef JNILOG_BRIDGE_H
#define JNILOG_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <jni.h>
#include "visualize.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <android/log.h>

#define LOG_TAG "JNILogPayload"
#define LOG_DIRECT(priority, fmt, ...) __android_log_print(priority, LOG_TAG, fmt, ##__VA_ARGS__)

typedef struct JNINativeInterface JNINativeInterface;
typedef const JNINativeInterface* PJNINativeInterface;

/* ============================================================================
 * wire_kind_t — shared type tag between C hooks and Go formatter.
 *
 * Numeric values are stable: they match JNIKind constants in value.go.
 * Add new kinds only at the end to preserve binary compatibility.
 * ============================================================================ */
typedef enum {
    WIRE_KIND_NULL    = 0,
    WIRE_KIND_VOID    = 1,
    WIRE_KIND_BOOLEAN = 2,
    WIRE_KIND_BYTE    = 3,
    WIRE_KIND_CHAR    = 4,
    WIRE_KIND_SHORT   = 5,
    WIRE_KIND_INT     = 6,
    WIRE_KIND_LONG    = 7,
    WIRE_KIND_FLOAT   = 8,
    WIRE_KIND_DOUBLE  = 9,
    WIRE_KIND_STRING  = 10, /* java.lang.String — str_val = UTF-8 content */
    WIRE_KIND_CLASS   = 11, /* jclass — str_val = slash-separated class name */
    WIRE_KIND_OBJECT  = 12, /* jobject — str_val = class name, extra = toString */
    WIRE_KIND_ARRAY   = 13, /* jarray — str_val = formatted repr */
    WIRE_KIND_POINTER = 14, /* method/field ID or raw addr — raw_val = address */
} wire_kind_t;

/* Map return_kind_t (hook_internal.h) to wire_kind_t.
 * Return kinds match the RET_* enum order defined in hook_internal.h:
 *   RET_OBJECT=0, RET_BOOLEAN=1, ..., RET_VOID=9  */
static inline wire_kind_t return_kind_to_wire(int ret_kind) {
    /* Direct correspondence — object needs runtime refinement in C
     * (is it a String? jclass?) before passing to Go. */
    static const wire_kind_t map[] = {
        WIRE_KIND_OBJECT,   /* RET_OBJECT  — may be promoted to STRING/CLASS */
        WIRE_KIND_BOOLEAN,  /* RET_BOOLEAN */
        WIRE_KIND_BYTE,     /* RET_BYTE    */
        WIRE_KIND_CHAR,     /* RET_CHAR    */
        WIRE_KIND_SHORT,    /* RET_SHORT   */
        WIRE_KIND_INT,      /* RET_INT     */
        WIRE_KIND_LONG,     /* RET_LONG    */
        WIRE_KIND_FLOAT,    /* RET_FLOAT   */
        WIRE_KIND_DOUBLE,   /* RET_DOUBLE  */
        WIRE_KIND_VOID,     /* RET_VOID    */
    };
    if (ret_kind < 0 || ret_kind > 9) return WIRE_KIND_NULL;
    return map[ret_kind];
}

enum {
    kLogPriorityUnknown = 0,
    kLogPriorityDefault = 1,
    kLogPriorityVerbose = 2,
    kLogPriorityDebug   = 3,
    kLogPriorityInfo    = 4,
    kLogPriorityWarn    = 5,
    kLogPriorityError   = 6,
    kLogPriorityFatal   = 7,
    kLogPrioritySilent  = 8,
};

/* JNI hook table (defined in hooks.c) */
extern PJNINativeInterface g_original_jni_table;

/* Bridge lifecycle */
void bridge_init(void);
void bridge_activate_go(void);
void bridge_cleanup_with_env(void *env);

/* JNI hook management (hooks.c) */
int install_jni_hooks(void *env);
int restore_jni_hooks(void *env);

/* Native logging (variadic C wrappers — va_list can't cross cgo) */
void log_native(int priority, const char* format, ...);
void log_native_info(const char* format, ...);
void log_native_warn(const char* format, ...);
void log_native_error(const char* format, ...);
void log_message(const char* format, ...);

/* ============================================================================
 * Typed JNI event forwarding to Go.
 *
 * All "kind" parameters are wire_kind_t cast to int so they cross cgo cleanly.
 *
 * log_jni_call — called at method entry, before invocation.
 *   receiver_kind / _str / _extra  — typed receiver (KindNull for static)
 *   class_name                     — slash-separated declaring class
 *   method_name                    — short method name
 *   encoded_args                   — vis_encode_typed_args() output
 *
 * log_jni_return — called after method returns, with typed result.
 *   ret_kind     — wire_kind_t (promoted from return_kind_t)
 *   ret_raw      — numeric value for primitive kinds; ptr bits for objects
 *   ret_str      — class name (OBJECT), string content (STRING), array repr
 *   ret_extra    — toString (OBJECT), empty otherwise
 *
 * log_jni_field_access — Get/SetField events.
 *   value_kind / _raw / _str / _extra — typed field value
 * ============================================================================ */

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
    const char* caller);

void log_jni_return(
    int offset,
    const char* name,
    int ret_kind,
    uintptr_t ret_raw,
    const char* ret_str,
    const char* ret_extra);

void log_jni_lookup(
    const char* lookup_type,
    const char* name,
    const char* sig,
    void* clazz,
    const char* class_name,
    const char* caller);

void log_jni_register_natives(
    jclass clazz,
    const char* class_name,
    const JNINativeMethod* methods,
    int n_methods,
    const char* caller);

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
    const char* caller);

/* ============================================================================
 * method_log_ctx_t — captures all per-call data in typed form.
 *
 * receiver_kind / receiver_str / receiver_extra carry the typed receiver
 * (WIRE_KIND_NULL for static calls).
 * encoded_args is the vis_encode_typed_args() output for Go's decodeArgs().
 * ============================================================================ */
typedef struct {
    int should_log;
    int logging_ready;
    int receiver_kind;        /* wire_kind_t cast to int */
    const char *sig;          /* JNI descriptor string, from cache */
    const char *method_name;  /* short name, from cache */
    const char *clazz_name;   /* declaring class, from cache */
    char caller_str[192];
    char *receiver_str;       /* heap: class name or string content */
    char *receiver_extra;     /* heap: toString() for KindObject */
    char *encoded_args;       /* heap: vis_encode_typed_args() output */
} method_log_ctx_t;

/* ============================================================================
 * field_log_ctx_t — captures per-field-access data in typed form.
 *
 * receiver_kind / receiver_str / receiver_extra carry the typed receiver
 * (WIRE_KIND_CLASS for static fields, WIRE_KIND_OBJECT for instance fields).
 * clazz_name is the declaring class from the field ID cache.
 * ============================================================================ */
typedef struct {
    int should_log;
    int logging_ready;
    int receiver_kind;        /* wire_kind_t cast to int */
    const char *sig;          /* field type descriptor, from cache */
    const char *field_name;   /* field name, from cache */
    const char *clazz_name;   /* declaring class, from cache */
    char caller_str[192];
    char *receiver_str;       /* heap: class name */
    char *receiver_extra;     /* heap: toString() */
} field_log_ctx_t;

/* ============================================================================
 * Cgo export function signatures — Go callbacks invoked from C hooks.
 *
 * These are implemented in Go (src/go/) and exported via //export directives.
 * The C hook layer calls these to forward typed JNI event data to Go.
 *
 * When building with cgo, _cgo_export.h provides the canonical declarations.
 * These extern declarations serve as forward references for standalone C
 * compilation (xmake) or for IDE tooling that doesn't process cgo headers.
 * ============================================================================ */
#ifndef _CGO_EXPORT_H_
extern void goJNICallCallback(
    int offset,
    char* jni_name,
    int receiver_kind,
    char* receiver_str,
    char* receiver_extra,
    char* class_name,
    char* method_name,
    char* encoded_args,
    uintptr_t mid,
    char* caller);

extern void goJNIReturnCallback(
    int offset,
    char* name,
    int ret_kind,
    uintptr_t ret_raw,
    char* ret_str,
    char* ret_extra);

extern void goJNILookupCallback(
    char* lookup_type,
    char* name,
    char* sig,
    uintptr_t clazz,
    char* class_name,
    char* caller);

extern void goJNIRegisterNativesCallback(
    uintptr_t clazz,
    char* class_name,
    char* methods,
    char* caller);

extern void goJNIFieldCallback(
    int offset,
    char* name,
    int receiver_kind,
    char* receiver_str,
    char* receiver_extra,
    char* field_name,
    int value_kind,
    uintptr_t value_raw,
    char* value_str,
    char* value_extra,
    char* caller);

/* ============================================================================
 * Config query function signatures — Go cgo exports for C-side filter cache.
 *
 * These are called from C hook entry points to implement Gate 1 (blacklist)
 * and Gate 2 (whitelist) filtering without building full wire data.
 * ============================================================================ */
extern int config_function_blacklisted(char* name);
extern int config_function_enabled(char* name);
extern int config_array_max_items(void);
extern int config_stack_depth(void);

/* ============================================================================
 * Logging readiness — Go-side flag queried by C hooks.
 * ============================================================================ */
extern void goSetLoggingReady(int ready);
extern int  goGetLoggingReady(void);

/* ============================================================================
 * Go bridge lifecycle callbacks.
 * ============================================================================ */
extern void goBridgeInit(void);
extern void goBridgeCleanup(void);

/* ============================================================================
 * Go native logging helpers.
 * ============================================================================ */
extern void goLogNative(int priority, char* message);
extern void goLogNativeInfo(char* message);
extern void goLogNativeWarn(char* message);
extern void goLogNativeError(char* message);
extern void goLogCallback(char* message);
#endif /* _CGO_EXPORT_H_ */

/* Pure C range tracking (rangeset.c) */
void c_init_range_tracking(void);
const char* art_get_field_name(void* field_id);
void c_set_package_name(const char* name);
const char* c_get_package_name(void);
int c_path_contains_package(const char* path);
int c_is_system_lib_path(const char* path);
int c_add_exec_range(uintptr_t base, uintptr_t size);
int c_is_in_exec_range(uintptr_t addr);
int c_has_exec_ranges(void);
int c_should_try_seed(void);
void c_reset_seed_attempted(void);
int c_seed_exec_ranges_from_maps(void);

#ifdef __cplusplus
}
#endif

#endif /* JNILOG_BRIDGE_H */
