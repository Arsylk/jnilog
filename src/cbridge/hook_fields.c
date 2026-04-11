#include "hook_internal.h"
#include <string.h>
#include <stdlib.h>

/*
 * JNI_FIELD_RETURN_KIND maps JNI type name to return_kind_t for field hooks.
 * Object uses RET_OBJECT so log_field_access_result can resolve class/toString.
 */

/* ============================================================================
 * Primitive → wire_kind helpers (used by Set* hooks before the actual call)
 * ============================================================================ */

/* Encode a single primitive value to (wire_kind, raw). */
#define FIELD_PRIM_BOOL(v)   (int)WIRE_KIND_BOOLEAN, (uintptr_t)((v) != 0), "", ""
#define FIELD_PRIM_BYTE(v)   (int)WIRE_KIND_BYTE,    (uintptr_t)(uint8_t)(v), "", ""
#define FIELD_PRIM_CHAR(v)   (int)WIRE_KIND_CHAR,    (uintptr_t)(v), "", ""
#define FIELD_PRIM_SHORT(v)  (int)WIRE_KIND_SHORT,   (uintptr_t)(uint16_t)(int16_t)(v), "", ""
#define FIELD_PRIM_INT(v)    (int)WIRE_KIND_INT,     (uintptr_t)(uint32_t)(int32_t)(v), "", ""
#define FIELD_PRIM_LONG(v)   (int)WIRE_KIND_LONG,    (uintptr_t)(uint64_t)(int64_t)(v), "", ""

/* Float/double: bit-cast to integer for the raw field */
#define FIELD_PRIM_FLOAT(v)  _field_float_kind(), _field_float_raw(v), "", ""
#define FIELD_PRIM_DOUBLE(v) _field_double_kind(), _field_double_raw(v), "", ""

static inline int _field_float_kind(void)                { return (int)WIRE_KIND_FLOAT; }
static inline uintptr_t _field_float_raw(jfloat v)       { uint32_t u; memcpy(&u, &v, 4); return (uintptr_t)u; }
static inline int _field_double_kind(void)               { return (int)WIRE_KIND_DOUBLE; }
static inline uintptr_t _field_double_raw(jdouble v)     { uintptr_t u; memcpy(&u, &v, sizeof(u)); return u; }

/* Object Set*: resolve class name and toString before the call. */
static inline void field_obj_parts(JNIEnv *env, jobject obj,
                                    int *out_kind, uintptr_t *out_raw,
                                    char **out_str, char **out_extra) {
    *out_str   = NULL;
    *out_extra = NULL;
    if (!obj) { *out_kind = WIRE_KIND_NULL; *out_raw = 0; return; }
    *out_raw = (uintptr_t)obj;
    if (vis_is_string(env, obj)) {
        *out_kind = WIRE_KIND_STRING;
        *out_str  = vis_string_value(env, obj);
    } else if (vis_is_class(env, obj)) {
        *out_kind = WIRE_KIND_CLASS;
        *out_str  = vis_class_name(env, obj);
    } else {
        *out_kind  = WIRE_KIND_OBJECT;
        *out_str   = vis_object_class_name(env, obj);
        *out_extra = vis_object_tostring(env, obj);
    }
}

/* ============================================================================
 * Get*Field — log the result after retrieval
 * ============================================================================ */

#define DEFINE_FIELD_GET_HOOKS(Name, CType, RetKind, UnionF)                    \
  CType hooked_Get##Name##Field(JNIEnv *env, jobject obj, jfieldID field_id) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call() || !g_original_jni_table                            \
            || !g_original_jni_table->Get##Name##Field) {                       \
      return (g_original_jni_table && g_original_jni_table->Get##Name##Field)  \
             ? g_original_jni_table->Get##Name##Field(env, obj, field_id)      \
             : (CType)0;                                                        \
    }                                                                           \
    field_log_ctx_t ctx;                                                        \
    prepare_field_log_ctx(&ctx, env, obj, field_id, caller);                    \
    CType result = g_original_jni_table->Get##Name##Field(env, obj, field_id); \
    jni_return_value_t rv; rv.UnionF = result;                                  \
    log_field_access_result(env, &ctx, "Get" #Name "Field",                    \
                            JNI_SLOT(Get##Name##Field), RetKind, rv);           \
    field_log_ctx_destroy(&ctx);                                                \
    return result;                                                              \
  }

/* ============================================================================
 * Set*Field — encode the value before the call, then call
 * ============================================================================ */

/* Primitive Set*Field — encode with compile-time kind/raw helpers */
#define DEFINE_FIELD_SET_HOOKS_PRIM(Name, CType, WireKindExpr, RawExpr)         \
  void hooked_Set##Name##Field(JNIEnv *env, jobject obj,                        \
                               jfieldID field_id, CType value) {                \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call() || !g_original_jni_table                            \
            || !g_original_jni_table->Set##Name##Field) {                       \
      if (g_original_jni_table && g_original_jni_table->Set##Name##Field)       \
        g_original_jni_table->Set##Name##Field(env, obj, field_id, value);      \
      return;                                                                   \
    }                                                                           \
    field_log_ctx_t ctx;                                                        \
    prepare_field_log_ctx(&ctx, env, obj, field_id, caller);                    \
    emit_field_access_begin(CALL_TARGET_INSTANCE, "Set" #Name "Field",          \
                            JNI_SLOT(Set##Name##Field), &ctx,                   \
                            (WireKindExpr), (RawExpr), "", "");                 \
    g_original_jni_table->Set##Name##Field(env, obj, field_id, value);          \
    field_log_ctx_destroy(&ctx);                                                \
  }

/* Object Set*Field — resolve class/toString before the call */
#define DEFINE_FIELD_SET_OBJECT_HOOK()                                           \
  void hooked_SetObjectField(JNIEnv *env, jobject obj,                           \
                             jfieldID field_id, jobject value) {                 \
    void *caller = __builtin_return_address(0);                                  \
    if (is_reentrant_call() || !g_original_jni_table                             \
            || !g_original_jni_table->SetObjectField) {                          \
      if (g_original_jni_table && g_original_jni_table->SetObjectField)          \
        g_original_jni_table->SetObjectField(env, obj, field_id, value);         \
      return;                                                                    \
    }                                                                            \
    field_log_ctx_t ctx;                                                         \
    prepare_field_log_ctx(&ctx, env, obj, field_id, caller);                     \
    if (ctx.should_log && ctx.logging_ready) {                                   \
      set_reentrant_call(1);                                                     \
      int vkind; uintptr_t vraw; char *vstr, *vextra;                           \
      field_obj_parts(env, value, &vkind, &vraw, &vstr, &vextra);               \
      set_reentrant_call(0);                                                     \
      emit_field_access_begin(CALL_TARGET_INSTANCE, "SetObjectField",            \
                              JNI_SLOT(SetObjectField), &ctx,                    \
                              vkind, vraw,                                       \
                              vstr   ? vstr   : "",                              \
                              vextra ? vextra : "");                             \
      free(vstr); free(vextra);                                                  \
    }                                                                            \
    g_original_jni_table->SetObjectField(env, obj, field_id, value);             \
    field_log_ctx_destroy(&ctx);                                                 \
  }

/* ============================================================================
 * GetStatic*Field
 * ============================================================================ */

#define DEFINE_STATIC_FIELD_GET_HOOKS(Name, CType, RetKind, UnionF)             \
  CType hooked_GetStatic##Name##Field(JNIEnv *env, jclass clazz,               \
                                      jfieldID field_id) {                      \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call() || !g_original_jni_table                            \
            || !g_original_jni_table->GetStatic##Name##Field) {                 \
      return (g_original_jni_table && g_original_jni_table->GetStatic##Name##Field) \
             ? g_original_jni_table->GetStatic##Name##Field(env, clazz, field_id) \
             : (CType)0;                                                        \
    }                                                                           \
    field_log_ctx_t ctx;                                                        \
    prepare_field_log_ctx(&ctx, env, clazz, field_id, caller);                  \
    CType result = g_original_jni_table->GetStatic##Name##Field(env, clazz, field_id); \
    jni_return_value_t rv; rv.UnionF = result;                                  \
    log_field_access_result(env, &ctx, "GetStatic" #Name "Field",               \
                            JNI_SLOT(GetStatic##Name##Field), RetKind, rv);     \
    field_log_ctx_destroy(&ctx);                                                \
    return result;                                                              \
  }

/* ============================================================================
 * SetStatic*Field
 * ============================================================================ */

#define DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Name, CType, WireKindExpr, RawExpr)  \
  void hooked_SetStatic##Name##Field(JNIEnv *env, jclass clazz,                 \
                                     jfieldID field_id, CType value) {          \
    void *caller = __builtin_return_address(0);                                  \
    if (is_reentrant_call() || !g_original_jni_table                             \
            || !g_original_jni_table->SetStatic##Name##Field) {                  \
      if (g_original_jni_table && g_original_jni_table->SetStatic##Name##Field)  \
        g_original_jni_table->SetStatic##Name##Field(env, clazz, field_id, value); \
      return;                                                                    \
    }                                                                            \
    field_log_ctx_t ctx;                                                         \
    prepare_field_log_ctx(&ctx, env, clazz, field_id, caller);                   \
    emit_field_access_begin(CALL_TARGET_STATIC, "SetStatic" #Name "Field",       \
                            JNI_SLOT(SetStatic##Name##Field), &ctx,              \
                            (WireKindExpr), (RawExpr), "", "");                  \
    g_original_jni_table->SetStatic##Name##Field(env, clazz, field_id, value);   \
    field_log_ctx_destroy(&ctx);                                                 \
  }

#define DEFINE_STATIC_FIELD_SET_OBJECT_HOOK()                                    \
  void hooked_SetStaticObjectField(JNIEnv *env, jclass clazz,                    \
                                   jfieldID field_id, jobject value) {           \
    void *caller = __builtin_return_address(0);                                  \
    if (is_reentrant_call() || !g_original_jni_table                             \
            || !g_original_jni_table->SetStaticObjectField) {                    \
      if (g_original_jni_table && g_original_jni_table->SetStaticObjectField)    \
        g_original_jni_table->SetStaticObjectField(env, clazz, field_id, value); \
      return;                                                                    \
    }                                                                            \
    field_log_ctx_t ctx;                                                         \
    prepare_field_log_ctx(&ctx, env, clazz, field_id, caller);                   \
    if (ctx.should_log && ctx.logging_ready) {                                   \
      set_reentrant_call(1);                                                     \
      int vkind; uintptr_t vraw; char *vstr, *vextra;                           \
      field_obj_parts(env, value, &vkind, &vraw, &vstr, &vextra);               \
      set_reentrant_call(0);                                                     \
      emit_field_access_begin(CALL_TARGET_STATIC, "SetStaticObjectField",        \
                              JNI_SLOT(SetStaticObjectField), &ctx,              \
                              vkind, vraw,                                       \
                              vstr   ? vstr   : "",                              \
                              vextra ? vextra : "");                             \
      free(vstr); free(vextra);                                                  \
    }                                                                            \
    g_original_jni_table->SetStaticObjectField(env, clazz, field_id, value);     \
    field_log_ctx_destroy(&ctx);                                                 \
  }

/* ============================================================================
 * Instantiate all Get hooks via the original X-macro (works for all types
 * including Object because log_field_access_result handles the typed union).
 * ============================================================================ */
JNI_FIELD_TYPES(DEFINE_FIELD_GET_HOOKS)
JNI_FIELD_TYPES(DEFINE_STATIC_FIELD_GET_HOOKS)

/* ============================================================================
 * Instantiate Set hooks — Object uses special path, primitives use raw bits.
 * ============================================================================ */
DEFINE_FIELD_SET_OBJECT_HOOK()
DEFINE_STATIC_FIELD_SET_OBJECT_HOOK()

DEFINE_FIELD_SET_HOOKS_PRIM(Boolean, jboolean, (int)WIRE_KIND_BOOLEAN, (uintptr_t)((value)!=0))
DEFINE_FIELD_SET_HOOKS_PRIM(Byte,    jbyte,    (int)WIRE_KIND_BYTE,    (uintptr_t)(uint8_t)(value))
DEFINE_FIELD_SET_HOOKS_PRIM(Char,    jchar,    (int)WIRE_KIND_CHAR,    (uintptr_t)(value))
DEFINE_FIELD_SET_HOOKS_PRIM(Short,   jshort,   (int)WIRE_KIND_SHORT,   (uintptr_t)(uint16_t)(int16_t)(value))
DEFINE_FIELD_SET_HOOKS_PRIM(Int,     jint,     (int)WIRE_KIND_INT,     (uintptr_t)(uint32_t)(int32_t)(value))
DEFINE_FIELD_SET_HOOKS_PRIM(Long,    jlong,    (int)WIRE_KIND_LONG,    (uintptr_t)(uint64_t)(int64_t)(value))
DEFINE_FIELD_SET_HOOKS_PRIM(Float,   jfloat,   _field_float_kind(),    _field_float_raw(value))
DEFINE_FIELD_SET_HOOKS_PRIM(Double,  jdouble,  _field_double_kind(),   _field_double_raw(value))

DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Boolean, jboolean, (int)WIRE_KIND_BOOLEAN, (uintptr_t)((value)!=0))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Byte,    jbyte,    (int)WIRE_KIND_BYTE,    (uintptr_t)(uint8_t)(value))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Char,    jchar,    (int)WIRE_KIND_CHAR,    (uintptr_t)(value))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Short,   jshort,   (int)WIRE_KIND_SHORT,   (uintptr_t)(uint16_t)(int16_t)(value))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Int,     jint,     (int)WIRE_KIND_INT,     (uintptr_t)(uint32_t)(int32_t)(value))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Long,    jlong,    (int)WIRE_KIND_LONG,    (uintptr_t)(uint64_t)(int64_t)(value))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Float,   jfloat,   _field_float_kind(),    _field_float_raw(value))
DEFINE_STATIC_FIELD_SET_HOOKS_PRIM(Double,  jdouble,  _field_double_kind(),   _field_double_raw(value))
