/*
 * hook_logging.c — JNI hook logging context preparation.
 *
 * Redesign: this file no longer formats values to strings before passing
 * them to Go. Instead it extracts typed components (kind + raw numeric +
 * optional string content) and hands them to log_jni_* bridge functions,
 * which forward them to Go's typed formatJNIValue() dispatcher.
 *
 * The old vis_format_* calls are gone from the hot path for primitives.
 * For objects/arrays we still call into visualize.c to resolve class names
 * and toString(), but we pass those as separate str/extra strings rather
 * than a single pre-formatted blob.
 */

#include "hook_internal.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Typed argument encoding
 *
 * vis_encode_typed_args() (visualize.c) builds the wire format that Go's
 * decodeArgs() consumes:
 *
 *   For each arg:  sigChar \x01 primaryValue [ \x03 extraValue ] \x02
 *
 * Primitives: primaryValue = decimal integer or float string.
 * java.lang.String: sigChar='L', primary = UTF-8 content (no \x03).
 * Other objects: primary = class/Name, extra = toString().
 * Arrays: primary = formatted array repr (e.g. "[1, 2, 3]").
 * ============================================================================ */

/* ============================================================================
 * Receiver encoding helpers
 * ============================================================================ */

static wire_kind_t classify_object(JNIEnv *env, void *obj) {
    if (obj == NULL) return WIRE_KIND_NULL;
    if (vis_is_string(env, obj)) return WIRE_KIND_STRING;
    if (vis_is_class(env, obj))  return WIRE_KIND_CLASS;
    return WIRE_KIND_OBJECT;
}

/*
 * fill_object_strings — resolves class name and toString for a jobject.
 * Returns heap strings; caller must free both.
 */
static void fill_object_strings(JNIEnv *env, void *obj,
                                char **out_str, char **out_extra) {
    *out_str   = NULL;
    *out_extra = NULL;
    if (!obj || !env) return;

    wire_kind_t k = classify_object(env, obj);
    switch (k) {
    case WIRE_KIND_STRING:
        *out_str = vis_string_value_raw(env, obj);
        break;
    case WIRE_KIND_CLASS:
        *out_str = vis_class_name(env, obj);   /* slash-separated */
        break;
    case WIRE_KIND_OBJECT:
        *out_str   = vis_object_class_name(env, obj);
        *out_extra = vis_object_tostring(env, obj);
        break;
    default:
        break;
    }
}

/* ============================================================================
 * Method logging context
 * ============================================================================ */

void method_log_ctx_init(method_log_ctx_t *ctx) {
    if (ctx) memset(ctx, 0, sizeof(*ctx));
}

void method_log_ctx_destroy(method_log_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx->receiver_str);
    free(ctx->receiver_extra);
    free(ctx->encoded_args);
    ctx->receiver_str   = NULL;
    ctx->receiver_extra = NULL;
    ctx->encoded_args   = NULL;
}

static void prepare_method_log_ctx_common(method_log_ctx_t *ctx, JNIEnv *env,
                                          void *receiver, jmethodID method_id,
                                          void *caller) {
    method_log_ctx_init(ctx);
    if (!ctx || is_reentrant_call()) return;
    ctx->should_log = should_log_from_caller(env, caller);
    if (!ctx->should_log) return;
    ctx->logging_ready = goGetLoggingReady();
    if (!ctx->logging_ready) return;

    set_reentrant_call(1);
    address_of_r(caller, ctx->caller_str, sizeof(ctx->caller_str));

    /* Receiver */
    ctx->receiver_kind = (int)classify_object(env, receiver);
    fill_object_strings(env, receiver, &ctx->receiver_str, &ctx->receiver_extra);

    /* Method info from cache */
    method_info_t info = lookup_method_info(method_id);
    ctx->sig         = info.sig;
    ctx->method_name = info.name;
    ctx->clazz_name  = info.clazz;
    set_reentrant_call(0);
}

void prepare_method_log_ctx_from_valist(method_log_ctx_t *ctx, JNIEnv *env,
                                        void *receiver, jmethodID method_id,
                                        void *caller, va_list args) {
    prepare_method_log_ctx_common(ctx, env, receiver, method_id, caller);
    if (!ctx->should_log || !ctx->logging_ready) return;
    if (!ctx->sig) return;

    set_reentrant_call(1);
    uintptr_t extracted[MAX_EXTRACTED_ARGS];
    va_list ap_copy;
    va_copy(ap_copy, args);
    int count = extract_va_args(ctx->sig, ap_copy, extracted, MAX_EXTRACTED_ARGS);
    va_end(ap_copy);
    ctx->encoded_args = vis_encode_typed_args(env, ctx->sig, extracted, count);
    set_reentrant_call(0);
}

void prepare_method_log_ctx_from_jvalue(method_log_ctx_t *ctx, JNIEnv *env,
                                        void *receiver, jmethodID method_id,
                                        void *caller, const jvalue *args) {
    prepare_method_log_ctx_common(ctx, env, receiver, method_id, caller);
    if (!ctx->should_log || !ctx->logging_ready) return;
    if (!ctx->sig) return;

    set_reentrant_call(1);
    uintptr_t extracted[MAX_EXTRACTED_ARGS];
    int count = extract_jvalue_args(ctx->sig, args, extracted, MAX_EXTRACTED_ARGS);
    ctx->encoded_args = vis_encode_typed_args(env, ctx->sig, extracted, count);
    set_reentrant_call(0);
}

void emit_method_call_begin(call_target_kind_t target_kind, const char *name,
                            int slot, const method_log_ctx_t *ctx,
                            jmethodID method_id) {
    if (!ctx || !ctx->should_log || !ctx->logging_ready || is_reentrant_call()) return;

    int recv_kind = (target_kind == CALL_TARGET_STATIC)
                        ? WIRE_KIND_NULL
                        : ctx->receiver_kind;

    log_jni_call(
        slot,
        name,
        recv_kind,
        ctx->receiver_str   ? ctx->receiver_str   : "",
        ctx->receiver_extra ? ctx->receiver_extra : "",
        ctx->clazz_name     ? ctx->clazz_name     : "",
        ctx->method_name    ? ctx->method_name    : "",
        ctx->encoded_args   ? ctx->encoded_args   : "",
        (uintptr_t)method_id,
        ctx->caller_str);
}

/* ============================================================================
 * Return value — typed dispatch, no pre-formatting for primitives
 * ============================================================================ */

void log_method_return_value(JNIEnv *env, const method_log_ctx_t *ctx,
                             const char *name, int slot, return_kind_t kind,
                             jni_return_value_t value) {
    if (!ctx || !ctx->should_log || !ctx->logging_ready
            || !has_no_exception(env) || is_reentrant_call()) return;

    set_reentrant_call(1);

    wire_kind_t wkind = return_kind_to_wire((int)kind);
    uintptr_t   raw   = 0;
    char       *str   = NULL;
    char       *extra = NULL;

    switch (kind) {
    /* ── Primitives: raw numeric value only ─────────────────────────── */
    case RET_BOOLEAN: raw = (uintptr_t)value.z;  break;
    case RET_BYTE:    raw = (uintptr_t)(uint8_t)value.b; break;
    case RET_CHAR:    raw = (uintptr_t)value.c;  break;
    case RET_SHORT:   raw = (uintptr_t)(uint16_t)(int16_t)value.s; break;
    case RET_INT:     raw = (uintptr_t)(uint32_t)(int32_t)value.i; break;
    case RET_LONG:    raw = (uintptr_t)(uint64_t)(int64_t)value.j; break;
    case RET_FLOAT: {
        /* Send float bits in low 32 of raw so Go can bit-cast back. */
        uint32_t bits;
        memcpy(&bits, &value.f, sizeof(bits));
        raw = (uintptr_t)bits;
        break;
    }
    case RET_DOUBLE:
        memcpy(&raw, &value.d, sizeof(raw)); /* 64-bit platforms only */
        break;
    case RET_VOID:
        /* No value to encode. */
        break;

    /* ── Object: resolve class name and toString ─────────────────────── */
    case RET_OBJECT:
        raw = (uintptr_t)value.obj;
        if (value.obj != NULL && env != NULL) {
            wkind = (wire_kind_t)classify_object(env, value.obj);
            fill_object_strings(env, value.obj, &str, &extra);
        } else {
            wkind = WIRE_KIND_NULL;
        }
        break;
    }

    const char *display_name = ctx->method_name ? ctx->method_name : name;
    log_jni_return(slot, display_name, (int)wkind, raw,
                   str   ? str   : "",
                   extra ? extra : "");
    free(str);
    free(extra);
    set_reentrant_call(0);
}

void log_method_return_void(JNIEnv *env, const method_log_ctx_t *ctx,
                            const char *name, int slot) {
    jni_return_value_t unused;
    memset(&unused, 0, sizeof(unused));
    log_method_return_value(env, ctx, name, slot, RET_VOID, unused);
}

/* ============================================================================
 * Field logging context
 * ============================================================================ */

void field_log_ctx_init(field_log_ctx_t *ctx) {
    if (ctx) memset(ctx, 0, sizeof(*ctx));
}

void field_log_ctx_destroy(field_log_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx->receiver_str);
    free(ctx->receiver_extra);
    ctx->receiver_str   = NULL;
    ctx->receiver_extra = NULL;
}

void prepare_field_log_ctx(field_log_ctx_t *ctx, JNIEnv *env, void *receiver,
                           jfieldID field_id, void *caller) {
    field_log_ctx_init(ctx);
    if (!ctx || is_reentrant_call()) return;
    ctx->should_log = should_log_from_caller(env, caller);
    if (!ctx->should_log) return;
    ctx->logging_ready = goGetLoggingReady();
    if (!ctx->logging_ready) return;

    set_reentrant_call(1);
    address_of_r(caller, ctx->caller_str, sizeof(ctx->caller_str));

    ctx->receiver_kind = (int)classify_object(env, receiver);
    fill_object_strings(env, receiver, &ctx->receiver_str, &ctx->receiver_extra);

    field_info_t info = lookup_field_info(field_id);
    ctx->sig        = info.sig;
    ctx->field_name = info.name;
    set_reentrant_call(0);
}

/*
 * emit_field_access_begin — used for Set* calls (value is known before the op).
 * value_kind, value_raw, value_str, value_extra carry the typed field value.
 */
void emit_field_access_begin(call_target_kind_t target_kind, const char *name,
                             int slot, const field_log_ctx_t *ctx,
                             int value_kind, uintptr_t value_raw,
                             const char *value_str, const char *value_extra) {
    if (!ctx || !ctx->should_log || !ctx->logging_ready || is_reentrant_call()) return;

    set_reentrant_call(1);
    int recv_kind = (target_kind == CALL_TARGET_STATIC)
                        ? ctx->receiver_kind  /* pass class as receiver for static */
                        : ctx->receiver_kind;
    log_jni_field_access(
        slot, name,
        recv_kind,
        ctx->receiver_str   ? ctx->receiver_str   : "",
        ctx->receiver_extra ? ctx->receiver_extra : "",
        ctx->field_name     ? ctx->field_name     : "?",
        value_kind, value_raw,
        value_str   ? value_str   : "",
        value_extra ? value_extra : "",
        ctx->caller_str);
    set_reentrant_call(0);
}

/*
 * log_field_access_result — used for Get* calls (value is the return of the op).
 * Resolves the typed value the same way log_method_return_value does.
 */
void log_field_access_result(JNIEnv *env, const field_log_ctx_t *ctx,
                             const char *name, int slot,
                             return_kind_t kind, jni_return_value_t value) {
    if (!ctx || !ctx->should_log || !ctx->logging_ready
            || !has_no_exception(env) || is_reentrant_call()) return;

    set_reentrant_call(1);

    wire_kind_t wkind = return_kind_to_wire((int)kind);
    uintptr_t   raw   = 0;
    char       *str   = NULL;
    char       *extra = NULL;

    switch (kind) {
    case RET_BOOLEAN: raw = (uintptr_t)value.z; break;
    case RET_BYTE:    raw = (uintptr_t)(uint8_t)value.b; break;
    case RET_CHAR:    raw = (uintptr_t)value.c; break;
    case RET_SHORT:   raw = (uintptr_t)(uint16_t)(int16_t)value.s; break;
    case RET_INT:     raw = (uintptr_t)(uint32_t)(int32_t)value.i; break;
    case RET_LONG:    raw = (uintptr_t)(uint64_t)(int64_t)value.j; break;
    case RET_FLOAT: {
        uint32_t bits; memcpy(&bits, &value.f, sizeof(bits)); raw = bits; break;
    }
    case RET_DOUBLE:
        memcpy(&raw, &value.d, sizeof(raw)); break;
    case RET_VOID:
        break;
    case RET_OBJECT:
        raw = (uintptr_t)value.obj;
        if (value.obj && env) {
            wkind = (wire_kind_t)classify_object(env, value.obj);
            fill_object_strings(env, value.obj, &str, &extra);
        } else {
            wkind = WIRE_KIND_NULL;
        }
        break;
    }

    int recv_kind = ctx->receiver_kind;
    log_jni_field_access(
        slot, name,
        recv_kind,
        ctx->receiver_str   ? ctx->receiver_str   : "",
        ctx->receiver_extra ? ctx->receiver_extra : "",
        ctx->field_name     ? ctx->field_name     : "?",
        (int)wkind, raw,
        str   ? str   : "",
        extra ? extra : "",
        ctx->caller_str);
    free(str);
    free(extra);
    set_reentrant_call(0);
}
