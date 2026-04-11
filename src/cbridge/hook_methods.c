#include "hook_internal.h"
#include <string.h>
#include <stdarg.h>

#define DEFINE_INSTANCE_NONVOID_HOOKS(Name, CType, RetKind, UnionF)              \
  CType hooked_Call##Name##Method(JNIEnv *env, jobject obj,                     \
                                  jmethodID method_id, ...) {                   \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      va_list ap; va_start(ap, method_id);                                      \
      CType res = (g_original_jni_table && g_original_jni_table->Call##Name##MethodV) ? \
                  g_original_jni_table->Call##Name##MethodV(env, obj, method_id, ap) : (CType)0; \
      va_end(ap); return res;                                                   \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    va_list ap_log; va_start(ap_log, method_id);                                \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, ap_log); \
    va_end(ap_log);                                                             \
    emit_method_call_begin(CALL_TARGET_INSTANCE, "Call" #Name "Method",        \
                           JNI_SLOT(Call##Name##Method), &ctx, method_id);      \
    if (g_original_jni_table && g_original_jni_table->Call##Name##MethodV) {    \
      va_list ap_call; va_start(ap_call, method_id);                            \
      result = g_original_jni_table->Call##Name##MethodV(env, obj, method_id, ap_call); \
      va_end(ap_call);                                                          \
    } else log_missing_original("Call" #Name "Method", ctx.should_log);         \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "Call" #Name "Method",                   \
                            JNI_SLOT(Call##Name##Method), RetKind, ret_value);  \
    method_log_ctx_destroy(&ctx); return result;                                \
  }                                                                             \
  CType hooked_Call##Name##MethodV(JNIEnv *env, jobject obj,                    \
                                   jmethodID method_id, va_list args) {         \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->Call##Name##MethodV) ? \
             g_original_jni_table->Call##Name##MethodV(env, obj, method_id, args) : (CType)0; \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "Call" #Name "MethodV",       \
                           JNI_SLOT(Call##Name##MethodV), &ctx, method_id);      \
    if (g_original_jni_table && g_original_jni_table->Call##Name##MethodV)      \
      result = g_original_jni_table->Call##Name##MethodV(env, obj, method_id, args); \
    else log_missing_original("Call" #Name "MethodV", ctx.should_log);          \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "Call" #Name "MethodV",                  \
                            JNI_SLOT(Call##Name##MethodV), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }                                                                             \
  CType hooked_Call##Name##MethodA(JNIEnv *env, jobject obj,                    \
                                   jmethodID method_id, const jvalue *args) {   \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->Call##Name##MethodA) ? \
             g_original_jni_table->Call##Name##MethodA(env, obj, method_id, args) : (CType)0; \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    prepare_method_log_ctx_from_jvalue(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "Call" #Name "MethodA",       \
                           JNI_SLOT(Call##Name##MethodA), &ctx, method_id);      \
    if (g_original_jni_table && g_original_jni_table->Call##Name##MethodA)      \
      result = g_original_jni_table->Call##Name##MethodA(env, obj, method_id, args); \
    else log_missing_original("Call" #Name "MethodA", ctx.should_log);          \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "Call" #Name "MethodA",                  \
                            JNI_SLOT(Call##Name##MethodA), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }

#define DEFINE_STATIC_NONVOID_HOOKS(Name, CType, RetKind, UnionF)               \
  CType hooked_CallStatic##Name##Method(JNIEnv *env, jclass clazz,              \
                                        jmethodID method_id, ...) {             \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      va_list ap; va_start(ap, method_id);                                      \
      CType res = (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV) ? \
                  g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, ap) : (CType)0; \
      va_end(ap); return res;                                                   \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    va_list ap_log; va_start(ap_log, method_id);                                \
    prepare_method_log_ctx_from_valist(&ctx, env, clazz, method_id, caller, ap_log); \
    va_end(ap_log);                                                             \
    emit_method_call_begin(CALL_TARGET_STATIC, "CallStatic" #Name "Method",     \
                           JNI_SLOT(CallStatic##Name##Method), &ctx, method_id);\
    if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV) { \
      va_list ap_call; va_start(ap_call, method_id);                            \
      result = g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, ap_call); \
      va_end(ap_call);                                                          \
    } else log_missing_original("CallStatic" #Name "Method", ctx.should_log);   \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "CallStatic" #Name "Method",             \
                            JNI_SLOT(CallStatic##Name##Method), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }                                                                             \
  CType hooked_CallStatic##Name##MethodV(JNIEnv *env, jclass clazz,             \
                                         jmethodID method_id, va_list args) {  \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV) ? \
             g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, args) : (CType)0; \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    prepare_method_log_ctx_from_valist(&ctx, env, clazz, method_id, caller, args); \
    emit_method_call_begin(CALL_TARGET_STATIC, "CallStatic" #Name "MethodV",    \
                           JNI_SLOT(CallStatic##Name##MethodV), &ctx, method_id);\
    if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV)\
      result = g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, args); \
    else log_missing_original("CallStatic" #Name "MethodV", ctx.should_log);    \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "CallStatic" #Name "MethodV",            \
                            JNI_SLOT(CallStatic##Name##MethodV), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }                                                                             \
  CType hooked_CallStatic##Name##MethodA(JNIEnv *env, jclass clazz,             \
                                         jmethodID method_id, const jvalue *args) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodA) ? \
             g_original_jni_table->CallStatic##Name##MethodA(env, clazz, method_id, args) : (CType)0; \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    prepare_method_log_ctx_from_jvalue(&ctx, env, clazz, method_id, caller, args); \
    emit_method_call_begin(CALL_TARGET_STATIC, "CallStatic" #Name "MethodA",    \
                           JNI_SLOT(CallStatic##Name##MethodA), &ctx, method_id);\
    if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodA)\
      result = g_original_jni_table->CallStatic##Name##MethodA(env, clazz, method_id, args); \
    else log_missing_original("CallStatic" #Name "MethodA", ctx.should_log);    \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "CallStatic" #Name "MethodA",            \
                            JNI_SLOT(CallStatic##Name##MethodA), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }

#define DEFINE_INSTANCE_VOID_HOOKS(Name, CType, RetKind)                        \
  void hooked_Call##Name##Method(JNIEnv *env, jobject obj,                      \
                                 jmethodID method_id, ...) {                    \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      va_list ap; va_start(ap, method_id);                                      \
      if (g_original_jni_table && g_original_jni_table->Call##Name##MethodV)    \
        g_original_jni_table->Call##Name##MethodV(env, obj, method_id, ap);      \
      va_end(ap); return;                                                       \
    }                                                                           \
    method_log_ctx_t ctx; va_list ap_log; va_start(ap_log, method_id);          \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, ap_log); \
    va_end(ap_log);                                                             \
    emit_method_call_begin(CALL_TARGET_INSTANCE, "Call" #Name "Method",        \
                           JNI_SLOT(Call##Name##Method), &ctx, method_id);      \
    if (g_original_jni_table && g_original_jni_table->Call##Name##MethodV) {    \
      va_list ap_call; va_start(ap_call, method_id);                            \
      g_original_jni_table->Call##Name##MethodV(env, obj, method_id, ap_call);  \
      va_end(ap_call);                                                          \
    } else log_missing_original("Call" #Name "Method", ctx.should_log);         \
    log_method_return_void(env, &ctx, "Call" #Name "Method", JNI_SLOT(Call##Name##Method)); \
    method_log_ctx_destroy(&ctx);                                               \
  }                                                                             \
  void hooked_Call##Name##MethodV(JNIEnv *env, jobject obj,                     \
                                  jmethodID method_id, va_list args) {          \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->Call##Name##MethodV)    \
        g_original_jni_table->Call##Name##MethodV(env, obj, method_id, args);   \
      return;                                                                   \
    }                                                                           \
    method_log_ctx_t ctx;                                                       \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "Call" #Name "MethodV",       \
                           JNI_SLOT(Call##Name##MethodV), &ctx, method_id);      \
    if (g_original_jni_table && g_original_jni_table->Call##Name##MethodV)      \
      g_original_jni_table->Call##Name##MethodV(env, obj, method_id, args);     \
    else log_missing_original("Call" #Name "MethodV", ctx.should_log);          \
    log_method_return_void(env, &ctx, "Call" #Name "MethodV", JNI_SLOT(Call##Name##MethodV)); \
    method_log_ctx_destroy(&ctx);                                               \
  }                                                                             \
  void hooked_Call##Name##MethodA(JNIEnv *env, jobject obj,                     \
                                  jmethodID method_id, const jvalue *args) {    \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->Call##Name##MethodA)    \
        g_original_jni_table->Call##Name##MethodA(env, obj, method_id, args);   \
      return;                                                                   \
    }                                                                           \
    method_log_ctx_t ctx;                                                       \
    prepare_method_log_ctx_from_jvalue(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "Call" #Name "MethodA",       \
                           JNI_SLOT(Call##Name##MethodA), &ctx, method_id);      \
    if (g_original_jni_table && g_original_jni_table->Call##Name##MethodA)      \
      g_original_jni_table->Call##Name##MethodA(env, obj, method_id, args);     \
    else log_missing_original("Call" #Name "MethodA", ctx.should_log);          \
    log_method_return_void(env, &ctx, "Call" #Name "MethodA", JNI_SLOT(Call##Name##MethodA)); \
    method_log_ctx_destroy(&ctx);                                               \
  }

#define DEFINE_STATIC_VOID_HOOKS(Name, CType, RetKind)                          \
  void hooked_CallStatic##Name##Method(JNIEnv *env, jclass clazz,               \
                                       jmethodID method_id, ...) {              \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      va_list ap; va_start(ap, method_id);                                      \
      if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV) \
        g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, ap); \
      va_end(ap); return;                                                       \
    }                                                                           \
    method_log_ctx_t ctx; va_list ap_log; va_start(ap_log, method_id);          \
    prepare_method_log_ctx_from_valist(&ctx, env, clazz, method_id, caller, ap_log); \
    va_end(ap_log);                                                             \
    emit_method_call_begin(CALL_TARGET_STATIC, "CallStatic" #Name "Method",     \
                           JNI_SLOT(CallStatic##Name##Method), &ctx, method_id);\
    if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV) { \
      va_list ap_call; va_start(ap_call, method_id);                            \
      g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, ap_call); \
      va_end(ap_call);                                                          \
    } else log_missing_original("CallStatic" #Name "Method", ctx.should_log);   \
    log_method_return_void(env, &ctx, "CallStatic" #Name "Method",              \
                           JNI_SLOT(CallStatic##Name##Method));                 \
    method_log_ctx_destroy(&ctx);                                               \
  }                                                                             \
  void hooked_CallStatic##Name##MethodV(JNIEnv *env, jclass clazz,              \
                                        jmethodID method_id, va_list args) {   \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV)\
        g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, args); \
      return;                                                                   \
    }                                                                           \
    method_log_ctx_t ctx;                                                       \
    prepare_method_log_ctx_from_valist(&ctx, env, clazz, method_id, caller, args); \
    emit_method_call_begin(CALL_TARGET_STATIC, "CallStatic" #Name "MethodV",    \
                           JNI_SLOT(CallStatic##Name##MethodV), &ctx, method_id);\
    if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodV)\
      g_original_jni_table->CallStatic##Name##MethodV(env, clazz, method_id, args); \
    else log_missing_original("CallStatic" #Name "MethodV", ctx.should_log);    \
    log_method_return_void(env, &ctx, "CallStatic" #Name "MethodV",             \
                           JNI_SLOT(CallStatic##Name##MethodV));                \
    method_log_ctx_destroy(&ctx);                                               \
  }                                                                             \
  void hooked_CallStatic##Name##MethodA(JNIEnv *env, jclass clazz,              \
                                        jmethodID method_id, const jvalue *args) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodA)\
        g_original_jni_table->CallStatic##Name##MethodA(env, clazz, method_id, args); \
      return;                                                                   \
    }                                                                           \
    method_log_ctx_t ctx;                                                       \
    prepare_method_log_ctx_from_jvalue(&ctx, env, clazz, method_id, caller, args); \
    emit_method_call_begin(CALL_TARGET_STATIC, "CallStatic" #Name "MethodA",    \
                           JNI_SLOT(CallStatic##Name##MethodA), &ctx, method_id);\
    if (g_original_jni_table && g_original_jni_table->CallStatic##Name##MethodA)\
      g_original_jni_table->CallStatic##Name##MethodA(env, clazz, method_id, args); \
    else log_missing_original("CallStatic" #Name "MethodA", ctx.should_log);    \
    log_method_return_void(env, &ctx, "CallStatic" #Name "MethodA",             \
                           JNI_SLOT(CallStatic##Name##MethodA));                \
    method_log_ctx_destroy(&ctx);                                               \
  }

#define DEFINE_NONVIRTUAL_NONVOID_HOOKS(Name, CType, RetKind, UnionF)           \
  CType hooked_CallNonvirtual##Name##Method(JNIEnv *env, jobject obj,           \
                                            jclass clazz, jmethodID method_id, ...) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      va_list ap; va_start(ap, method_id);                                      \
      CType res = (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) ? \
                  g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, ap) : (CType)0; \
      va_end(ap); return res;                                                   \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    va_list ap_log; va_start(ap_log, method_id);                                \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, ap_log); \
    va_end(ap_log);                                                             \
    emit_method_call_begin(CALL_TARGET_INSTANCE, "CallNonvirtual" #Name "Method", \
                           JNI_SLOT(CallNonvirtual##Name##Method), &ctx, method_id); \
    if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) { \
      va_list ap_call; va_start(ap_call, method_id);                            \
      result = g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, ap_call); \
      va_end(ap_call);                                                          \
    } else log_missing_original("CallNonvirtual" #Name "Method", ctx.should_log); \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "CallNonvirtual" #Name "Method",          \
                            JNI_SLOT(CallNonvirtual##Name##Method), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }                                                                             \
  CType hooked_CallNonvirtual##Name##MethodV(JNIEnv *env, jobject obj,           \
                                             jclass clazz, jmethodID method_id, va_list args) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) ? \
             g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, args) : (CType)0; \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "CallNonvirtual" #Name "MethodV", \
                           JNI_SLOT(CallNonvirtual##Name##MethodV), &ctx, method_id); \
    if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) \
      result = g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, args); \
    else log_missing_original("CallNonvirtual" #Name "MethodV", ctx.should_log); \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "CallNonvirtual" #Name "MethodV",         \
                            JNI_SLOT(CallNonvirtual##Name##MethodV), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }                                                                             \
  CType hooked_CallNonvirtual##Name##MethodA(JNIEnv *env, jobject obj,           \
                                             jclass clazz, jmethodID method_id, const jvalue *args) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodA) ? \
             g_original_jni_table->CallNonvirtual##Name##MethodA(env, obj, clazz, method_id, args) : (CType)0; \
    }                                                                           \
    method_log_ctx_t ctx; CType result = (CType)0;                              \
    prepare_method_log_ctx_from_jvalue(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "CallNonvirtual" #Name "MethodA", \
                           JNI_SLOT(CallNonvirtual##Name##MethodA), &ctx, method_id); \
    if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodA) \
      result = g_original_jni_table->CallNonvirtual##Name##MethodA(env, obj, clazz, method_id, args); \
    else log_missing_original("CallNonvirtual" #Name "MethodA", ctx.should_log); \
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));      \
    ret_value.UnionF = result;                                                  \
    log_method_return_value(env, &ctx, "CallNonvirtual" #Name "MethodA",         \
                            JNI_SLOT(CallNonvirtual##Name##MethodA), RetKind, ret_value); \
    method_log_ctx_destroy(&ctx); return result;                                \
  }

#define DEFINE_NONVIRTUAL_VOID_HOOKS(Name, CType, RetKind)                      \
  void hooked_CallNonvirtual##Name##Method(JNIEnv *env, jobject obj,            \
                                           jclass clazz, jmethodID method_id, ...) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      va_list ap; va_start(ap, method_id);                                      \
      if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) \
        g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, ap); \
      va_end(ap); return;                                                       \
    }                                                                           \
    method_log_ctx_t ctx; va_list ap_log; va_start(ap_log, method_id);          \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, ap_log); \
    va_end(ap_log);                                                             \
    emit_method_call_begin(CALL_TARGET_INSTANCE, "CallNonvirtual" #Name "Method", \
                           JNI_SLOT(CallNonvirtual##Name##Method), &ctx, method_id); \
    if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) { \
      va_list ap_call; va_start(ap_call, method_id);                            \
      g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, ap_call); \
      va_end(ap_call);                                                          \
    } else log_missing_original("CallNonvirtual" #Name "Method", ctx.should_log); \
    log_method_return_void(env, &ctx, "CallNonvirtual" #Name "Method",          \
                           JNI_SLOT(CallNonvirtual##Name##Method));             \
    method_log_ctx_destroy(&ctx);                                               \
  }                                                                             \
  void hooked_CallNonvirtual##Name##MethodV(JNIEnv *env, jobject obj,           \
                                            jclass clazz, jmethodID method_id, va_list args) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) \
        g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, args); \
      return;                                                                   \
    }                                                                           \
    method_log_ctx_t ctx;                                                       \
    prepare_method_log_ctx_from_valist(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "CallNonvirtual" #Name "MethodV", \
                           JNI_SLOT(CallNonvirtual##Name##MethodV), &ctx, method_id); \
    if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodV) \
      g_original_jni_table->CallNonvirtual##Name##MethodV(env, obj, clazz, method_id, args); \
    else log_missing_original("CallNonvirtual" #Name "MethodV", ctx.should_log); \
    log_method_return_void(env, &ctx, "CallNonvirtual" #Name "MethodV",         \
                           JNI_SLOT(CallNonvirtual##Name##MethodV));            \
    method_log_ctx_destroy(&ctx);                                               \
  }                                                                             \
  void hooked_CallNonvirtual##Name##MethodA(JNIEnv *env, jobject obj,           \
                                            jclass clazz, jmethodID method_id, const jvalue *args) { \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodA) \
        g_original_jni_table->CallNonvirtual##Name##MethodA(env, obj, clazz, method_id, args); \
      return;                                                                   \
    }                                                                           \
    method_log_ctx_t ctx;                                                       \
    prepare_method_log_ctx_from_jvalue(&ctx, env, obj, method_id, caller, args);\
    emit_method_call_begin(CALL_TARGET_INSTANCE, "CallNonvirtual" #Name "MethodA", \
                           JNI_SLOT(CallNonvirtual##Name##MethodA), &ctx, method_id); \
    if (g_original_jni_table && g_original_jni_table->CallNonvirtual##Name##MethodA) \
      g_original_jni_table->CallNonvirtual##Name##MethodA(env, obj, clazz, method_id, args); \
    else log_missing_original("CallNonvirtual" #Name "MethodA", ctx.should_log); \
    log_method_return_void(env, &ctx, "CallNonvirtual" #Name "MethodA",         \
                           JNI_SLOT(CallNonvirtual##Name##MethodA));            \
    method_log_ctx_destroy(&ctx);                                               \
  }

jobject hooked_NewObject(JNIEnv *env, jclass clazz, jmethodID method_id, ...) {
    void *caller = __builtin_return_address(0);
    if (is_reentrant_call()) {
        va_list ap; va_start(ap, method_id);
        jobject res = (g_original_jni_table && g_original_jni_table->NewObjectV) ? 
                      g_original_jni_table->NewObjectV(env, clazz, method_id, ap) : NULL;
        va_end(ap); return res;
    }
    method_log_ctx_t ctx; jobject result = NULL;
    va_list ap_log; va_start(ap_log, method_id);
    prepare_method_log_ctx_from_valist(&ctx, env, NULL, method_id, caller, ap_log);
    va_end(ap_log);
    emit_method_call_begin(CALL_TARGET_STATIC, "NewObject", JNI_SLOT(NewObject), &ctx, method_id);
    if (g_original_jni_table && g_original_jni_table->NewObjectV) {
        va_list ap_call; va_start(ap_call, method_id);
        result = g_original_jni_table->NewObjectV(env, clazz, method_id, ap_call);
        va_end(ap_call);
    } else log_missing_original("NewObject", ctx.should_log);
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));
    ret_value.obj = result;
    log_method_return_value(env, &ctx, "NewObject", JNI_SLOT(NewObject), RET_OBJECT, ret_value);
    method_log_ctx_destroy(&ctx); return result;
}

jobject hooked_NewObjectV(JNIEnv *env, jclass clazz, jmethodID method_id, va_list args) {
    void *caller = __builtin_return_address(0);
    if (is_reentrant_call()) {
        return (g_original_jni_table && g_original_jni_table->NewObjectV) ? 
               g_original_jni_table->NewObjectV(env, clazz, method_id, args) : NULL;
    }
    method_log_ctx_t ctx; jobject result = NULL;
    prepare_method_log_ctx_from_valist(&ctx, env, NULL, method_id, caller, args);
    emit_method_call_begin(CALL_TARGET_STATIC, "NewObjectV", JNI_SLOT(NewObjectV), &ctx, method_id);
    if (g_original_jni_table && g_original_jni_table->NewObjectV)
        result = g_original_jni_table->NewObjectV(env, clazz, method_id, args);
    else log_missing_original("NewObjectV", ctx.should_log);
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));
    ret_value.obj = result;
    log_method_return_value(env, &ctx, "NewObjectV", JNI_SLOT(NewObjectV), RET_OBJECT, ret_value);
    method_log_ctx_destroy(&ctx); return result;
}

jobject hooked_NewObjectA(JNIEnv *env, jclass clazz, jmethodID method_id, const jvalue *args) {
    void *caller = __builtin_return_address(0);
    if (is_reentrant_call()) {
        return (g_original_jni_table && g_original_jni_table->NewObjectA) ? 
               g_original_jni_table->NewObjectA(env, clazz, method_id, args) : NULL;
    }
    method_log_ctx_t ctx; jobject result = NULL;
    prepare_method_log_ctx_from_jvalue(&ctx, env, NULL, method_id, caller, args);
    emit_method_call_begin(CALL_TARGET_STATIC, "NewObjectA", JNI_SLOT(NewObjectA), &ctx, method_id);
    if (g_original_jni_table && g_original_jni_table->NewObjectA)
        result = g_original_jni_table->NewObjectA(env, clazz, method_id, args);
    else log_missing_original("NewObjectA", ctx.should_log);
    jni_return_value_t ret_value; memset(&ret_value, 0, sizeof(ret_value));
    ret_value.obj = result;
    log_method_return_value(env, &ctx, "NewObjectA", JNI_SLOT(NewObjectA), RET_OBJECT, ret_value);
    method_log_ctx_destroy(&ctx); return result;
}

JNI_INSTANCE_NONVOID_TYPES(DEFINE_INSTANCE_NONVOID_HOOKS)
JNI_STATIC_NONVOID_TYPES(DEFINE_STATIC_NONVOID_HOOKS)
JNI_INSTANCE_NONVOID_TYPES(DEFINE_NONVIRTUAL_NONVOID_HOOKS)
JNI_VOID_TYPES(DEFINE_INSTANCE_VOID_HOOKS)
JNI_VOID_TYPES(DEFINE_STATIC_VOID_HOOKS)
JNI_VOID_TYPES(DEFINE_NONVIRTUAL_VOID_HOOKS)
