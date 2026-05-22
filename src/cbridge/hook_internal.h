#ifndef JNILOG_HOOK_INTERNAL_H
#define JNILOG_HOOK_INTERNAL_H

#include <jni.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "bridge.h"
#include <sys/mman.h>
#include <unistd.h>

#ifndef METHOD_SIG_MAX
#define METHOD_SIG_MAX 256
#endif

#ifndef METHOD_NAME_MAX
#define METHOD_NAME_MAX 128
#endif

#define METHOD_CACHE_SIZE 2048u /* must remain a power of two */

typedef struct JNINativeInterface jni_table_t;

typedef struct {
  jmethodID method_id;
  char name[METHOD_NAME_MAX];
  char signature[METHOD_SIG_MAX];
  char clazz[METHOD_NAME_MAX];
  uint8_t in_use;
} method_sig_entry_t;

typedef struct {
  jfieldID field_id;
  char name[METHOD_NAME_MAX];
  char signature[METHOD_SIG_MAX];
  char clazz[METHOD_NAME_MAX];
  uint8_t in_use;
} field_sig_entry_t;

/* method_log_ctx_t and field_log_ctx_t are defined in bridge.h */

typedef union {
  jobject obj;
  jboolean z;
  jbyte b;
  jchar c;
  jshort s;
  jint i;
  jlong j;
  jfloat f;
  jdouble d;
} jni_return_value_t;

typedef enum {
  CALL_TARGET_INSTANCE = 0,
  CALL_TARGET_STATIC   = 1,
} call_target_kind_t;

typedef enum {
  RET_OBJECT = 0,
  RET_BOOLEAN,
  RET_BYTE,
  RET_CHAR,
  RET_SHORT,
  RET_INT,
  RET_LONG,
  RET_FLOAT,
  RET_DOUBLE,
  RET_VOID,
} return_kind_t;

typedef struct {
  void  *page_start;
  size_t region_size;
} prot_region_t;

typedef struct {
  const char *name;
  const char *sig;
  const char *clazz;
} method_info_t;

typedef struct {
  const char *name;
  const char *sig;
  const char *clazz;
} field_info_t;

#define JNI_SLOT(member) ((int)(offsetof(jni_table_t, member) / sizeof(void *)))

extern PJNINativeInterface g_original_jni_table;
extern pthread_mutex_t     g_hook_lock;

#ifndef MAX_EXTRACTED_ARGS
#define MAX_EXTRACTED_ARGS 32
#endif

/* ============================================================================
 * Shared helpers
 * ============================================================================ */
int  is_reentrant_call(void);
void set_reentrant_call(int val);
int  is_jni_critical(void);
void set_jni_critical(int val);
void address_of_r(void *addr, char *buf, size_t bufsz);
int  should_log_from_caller(JNIEnv *env, void *caller);
int  should_log_jni(JNIEnv *env, void *caller, const char *jni_name);
int  config_is_allowed(const char *jni_name);
void log_missing_original(const char *name, int should_log);
int  has_no_exception(JNIEnv *env);
int  protect_region(prot_region_t *region, int prot);
prot_region_t jni_table_region(void *table, size_t table_size);

/* ============================================================================
 * Cache management
 * ============================================================================ */
void cache_method_signature(jmethodID method_id, const char *name,
                             const char *sig, const char *clazz);
method_info_t lookup_method_info(jmethodID method_id);
void cache_field_signature(jfieldID field_id, const char *name,
                            const char *sig, const char *clazz);
field_info_t lookup_field_info(jfieldID field_id);

/* ============================================================================
 * Logging context — method calls
 * ============================================================================ */
void method_log_ctx_init(method_log_ctx_t *ctx);
void method_log_ctx_destroy(method_log_ctx_t *ctx);

void prepare_method_log_ctx_from_valist(method_log_ctx_t *ctx, JNIEnv *env,
                                        void *receiver, jmethodID method_id,
                                        void *caller, va_list args);
void prepare_method_log_ctx_from_jvalue(method_log_ctx_t *ctx, JNIEnv *env,
                                        void *receiver, jmethodID method_id,
                                        void *caller, const jvalue *args);

void emit_method_call_begin(call_target_kind_t target_kind, const char *name,
                            int slot, const method_log_ctx_t *ctx,
                            jmethodID method_id);

void log_method_return_value(JNIEnv *env, const method_log_ctx_t *ctx,
                             const char *name, int slot, return_kind_t kind,
                             jni_return_value_t value);
void log_method_return_void(JNIEnv *env, const method_log_ctx_t *ctx,
                            const char *name, int slot);

/* ============================================================================
 * Logging context — field access
 * ============================================================================ */
void field_log_ctx_init(field_log_ctx_t *ctx);
void field_log_ctx_destroy(field_log_ctx_t *ctx);

void prepare_field_log_ctx(field_log_ctx_t *ctx, JNIEnv *env, void *receiver,
                           jfieldID field_id, void *caller);

/*
 * emit_field_access_begin — for Set* operations (value known before the call).
 * value_kind / value_raw / value_str / value_extra carry the typed value.
 */
void emit_field_access_begin(call_target_kind_t target_kind, const char *name,
                             int slot, const field_log_ctx_t *ctx,
                             int value_kind, uintptr_t value_raw,
                             const char *value_str, const char *value_extra);

/*
 * log_field_access_result — for Get* operations (value is the return value).
 */
void log_field_access_result(JNIEnv *env, const field_log_ctx_t *ctx,
                             const char *name, int slot,
                             return_kind_t kind, jni_return_value_t value);

/* ============================================================================
 * Hook declarations — Reference Operations
 * ============================================================================ */
void    hooked_DeleteGlobalRef(JNIEnv *env, jobject globalRef);
void    hooked_DeleteLocalRef(JNIEnv *env, jobject localRef);
jobject hooked_NewLocalRef(JNIEnv *env, jobject ref);
jweak   hooked_NewWeakGlobalRef(JNIEnv *env, jobject obj);
void    hooked_DeleteWeakGlobalRef(JNIEnv *env, jweak ref);
jboolean hooked_IsSameObject(JNIEnv *env, jobject ref1, jobject ref2);
jint    hooked_PushLocalFrame(JNIEnv *env, jint capacity);
jobject hooked_PopLocalFrame(JNIEnv *env, jobject result);
jint    hooked_EnsureLocalCapacity(JNIEnv *env, jint capacity);

/* ============================================================================
 * Hook declarations — String Operations
 * ============================================================================ */
jstring      hooked_NewString(JNIEnv *env, const jchar *unicode, jsize len);
jsize        hooked_GetStringLength(JNIEnv *env, jstring str);
const jchar* hooked_GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy);
void         hooked_ReleaseStringChars(JNIEnv *env, jstring str, const jchar *chars);
jstring      hooked_NewStringUTF(JNIEnv *env, const char *utf);
jsize        hooked_GetStringUTFLength(JNIEnv *env, jstring str);
const char*  hooked_GetStringUTFChars(JNIEnv *env, jstring str, jboolean *isCopy);
void         hooked_ReleaseStringUTFChars(JNIEnv *env, jstring str, const char *chars);
void         hooked_GetStringRegion(JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf);
void         hooked_GetStringUTFRegion(JNIEnv *env, jstring str, jsize start, jsize len, char *buf);

/* ============================================================================
 * Primitive array type lists (X-macros)
 * ============================================================================ */
#define JNI_PRIMITIVE_ARRAY_TYPES(X)          \
  X(Boolean, jboolean, RET_BOOLEAN, z)        \
  X(Byte,    jbyte,    RET_BYTE,    b)        \
  X(Char,    jchar,    RET_CHAR,    c)        \
  X(Short,   jshort,   RET_SHORT,   s)        \
  X(Int,     jint,     RET_INT,     i)        \
  X(Long,    jlong,    RET_LONG,    j)        \
  X(Float,   jfloat,   RET_FLOAT,   f)        \
  X(Double,  jdouble,  RET_DOUBLE,  d)

/* Array Region Operations */
#define DECLARE_ARRAY_REGION_HOOKS(Name, CType, ...) \
  void hooked_Get##Name##ArrayRegion(JNIEnv *env, CType##Array array, jsize start, jsize len, CType *buf); \
  void hooked_Set##Name##ArrayRegion(JNIEnv *env, CType##Array array, jsize start, jsize len, const CType *buf);

JNI_PRIMITIVE_ARRAY_TYPES(DECLARE_ARRAY_REGION_HOOKS)

/* ============================================================================
 * Hook declarations — Exception Operations
 * ============================================================================ */
jint      hooked_Throw(JNIEnv *env, jthrowable obj);
jint      hooked_ThrowNew(JNIEnv *env, jclass clazz, const char *msg);
jthrowable hooked_ExceptionOccurred(JNIEnv *env);
void      hooked_ExceptionDescribe(JNIEnv *env);
void      hooked_ExceptionClear(JNIEnv *env);
void      hooked_FatalError(JNIEnv *env, const char *msg);
jboolean  hooked_ExceptionCheck(JNIEnv *env);

/* ============================================================================
 * Hook declarations — Object & Class Operations
 * ============================================================================ */
jobject      hooked_AllocObject(JNIEnv *env, jclass clazz);
jboolean     hooked_IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz);
jclass       hooked_DefineClass(JNIEnv *env, const char *name, jobject loader, const jbyte *buf, jsize bufLen);
jclass       hooked_GetSuperclass(JNIEnv *env, jclass clazz);
jboolean     hooked_IsAssignableFrom(JNIEnv *env, jclass clazz1, jclass clazz2);
jclass       hooked_GetObjectClass(JNIEnv *env, jobject obj);
jobjectRefType hooked_GetObjectRefType(JNIEnv *env, jobject obj);

/* ============================================================================
 * Hook declarations — Direct Buffer Operations
 * ============================================================================ */
jobject hooked_NewDirectByteBuffer(JNIEnv *env, void* address, jlong capacity);
void*   hooked_GetDirectBufferAddress(JNIEnv *env, jobject buf);
jlong   hooked_GetDirectBufferCapacity(JNIEnv *env, jobject buf);

/* ============================================================================
 * Hook declarations — Monitor Operations
 * ============================================================================ */
jint hooked_MonitorEnter(JNIEnv *env, jobject obj);
jint hooked_MonitorExit(JNIEnv *env, jobject obj);

/* ============================================================================
 * Hook declarations — Miscellaneous
 * ============================================================================ */
jint      hooked_GetVersion(JNIEnv *env);
jint      hooked_GetJavaVM(JNIEnv *env, JavaVM **vm);
jint      hooked_UnregisterNatives(JNIEnv *env, jclass clazz);
jobject   hooked_ToReflectedMethod(JNIEnv *env, jclass cls, jmethodID methodID, jboolean isStatic);
jmethodID hooked_FromReflectedMethod(JNIEnv *env, jobject method);
jfieldID  hooked_FromReflectedField(JNIEnv *env, jobject field);
jobject   hooked_ToReflectedField(JNIEnv *env, jclass cls, jfieldID fieldID, jboolean isStatic);

/* ============================================================================
 * Method call hook declarations (generated via X-macros)
 * ============================================================================ */
#define DECLARE_INSTANCE_NONVOID_HOOKS(Name, CType, RetKind, UnionF) \
  CType hooked_Call##Name##Method(JNIEnv *env, jobject obj, jmethodID method_id, ...); \
  CType hooked_Call##Name##MethodV(JNIEnv *env, jobject obj, jmethodID method_id, va_list args); \
  CType hooked_Call##Name##MethodA(JNIEnv *env, jobject obj, jmethodID method_id, const jvalue *args);

#define DECLARE_STATIC_NONVOID_HOOKS(Name, CType, RetKind, UnionF) \
  CType hooked_CallStatic##Name##Method(JNIEnv *env, jclass clazz, jmethodID method_id, ...); \
  CType hooked_CallStatic##Name##MethodV(JNIEnv *env, jclass clazz, jmethodID method_id, va_list args); \
  CType hooked_CallStatic##Name##MethodA(JNIEnv *env, jclass clazz, jmethodID method_id, const jvalue *args);

#define DECLARE_INSTANCE_VOID_HOOKS(Name, CType, RetKind) \
  void hooked_Call##Name##Method(JNIEnv *env, jobject obj, jmethodID method_id, ...); \
  void hooked_Call##Name##MethodV(JNIEnv *env, jobject obj, jmethodID method_id, va_list args); \
  void hooked_Call##Name##MethodA(JNIEnv *env, jobject obj, jmethodID method_id, const jvalue *args);

#define DECLARE_STATIC_VOID_HOOKS(Name, CType, RetKind) \
  void hooked_CallStatic##Name##Method(JNIEnv *env, jclass clazz, jmethodID method_id, ...); \
  void hooked_CallStatic##Name##MethodV(JNIEnv *env, jclass clazz, jmethodID method_id, va_list args); \
  void hooked_CallStatic##Name##MethodA(JNIEnv *env, jclass clazz, jmethodID method_id, const jvalue *args);

#define DECLARE_NONVIRTUAL_NONVOID_HOOKS(Name, CType, RetKind, UnionF) \
  CType hooked_CallNonvirtual##Name##Method(JNIEnv *env, jobject obj, jclass clazz, jmethodID method_id, ...); \
  CType hooked_CallNonvirtual##Name##MethodV(JNIEnv *env, jobject obj, jclass clazz, jmethodID method_id, va_list args); \
  CType hooked_CallNonvirtual##Name##MethodA(JNIEnv *env, jobject obj, jclass clazz, jmethodID method_id, const jvalue *args);

#define DECLARE_NONVIRTUAL_VOID_HOOKS(Name, CType, RetKind) \
  void hooked_CallNonvirtual##Name##Method(JNIEnv *env, jobject obj, jclass clazz, jmethodID method_id, ...); \
  void hooked_CallNonvirtual##Name##MethodV(JNIEnv *env, jobject obj, jclass clazz, jmethodID method_id, va_list args); \
  void hooked_CallNonvirtual##Name##MethodA(JNIEnv *env, jobject obj, jclass clazz, jmethodID method_id, const jvalue *args);

#define JNI_INSTANCE_NONVOID_TYPES(X) \
  X(Object,  jobject,  RET_OBJECT,  obj) \
  X(Boolean, jboolean, RET_BOOLEAN, z)   \
  X(Byte,    jbyte,    RET_BYTE,    b)   \
  X(Char,    jchar,    RET_CHAR,    c)   \
  X(Short,   jshort,   RET_SHORT,   s)   \
  X(Int,     jint,     RET_INT,     i)   \
  X(Long,    jlong,    RET_LONG,    j)   \
  X(Float,   jfloat,   RET_FLOAT,   f)   \
  X(Double,  jdouble,  RET_DOUBLE,  d)

#define JNI_STATIC_NONVOID_TYPES(X) JNI_INSTANCE_NONVOID_TYPES(X)
#define JNI_VOID_TYPES(X) X(Void, void, RET_VOID)

JNI_INSTANCE_NONVOID_TYPES(DECLARE_INSTANCE_NONVOID_HOOKS)
JNI_STATIC_NONVOID_TYPES(DECLARE_STATIC_NONVOID_HOOKS)
JNI_INSTANCE_NONVOID_TYPES(DECLARE_NONVIRTUAL_NONVOID_HOOKS)
JNI_VOID_TYPES(DECLARE_INSTANCE_VOID_HOOKS)
JNI_VOID_TYPES(DECLARE_STATIC_VOID_HOOKS)
JNI_VOID_TYPES(DECLARE_NONVIRTUAL_VOID_HOOKS)

jobject hooked_NewObject(JNIEnv *env, jclass clazz, jmethodID method_id, ...);
jobject hooked_NewObjectV(JNIEnv *env, jclass clazz, jmethodID method_id, va_list args);
jobject hooked_NewObjectA(JNIEnv *env, jclass clazz, jmethodID method_id, const jvalue *args);

/* ============================================================================
 * Field hook declarations (generated via X-macros)
 * ============================================================================ */
#define DECLARE_FIELD_GET_HOOKS(Name, CType, RetKind, UnionF) \
  CType hooked_Get##Name##Field(JNIEnv *env, jobject obj, jfieldID field_id);

#define DECLARE_FIELD_SET_HOOKS(Name, CType, RetKind, UnionF) \
  void hooked_Set##Name##Field(JNIEnv *env, jobject obj, jfieldID field_id, CType value);

#define DECLARE_STATIC_FIELD_GET_HOOKS(Name, CType, RetKind, UnionF) \
  CType hooked_GetStatic##Name##Field(JNIEnv *env, jclass clazz, jfieldID field_id);

#define DECLARE_STATIC_FIELD_SET_HOOKS(Name, CType, RetKind, UnionF) \
  void hooked_SetStatic##Name##Field(JNIEnv *env, jclass clazz, jfieldID field_id, CType value);

#define JNI_FIELD_TYPES(X) \
  X(Object,  jobject,  RET_OBJECT,  obj) \
  X(Boolean, jboolean, RET_BOOLEAN, z)   \
  X(Byte,    jbyte,    RET_BYTE,    b)   \
  X(Char,    jchar,    RET_CHAR,    c)   \
  X(Short,   jshort,   RET_SHORT,   s)   \
  X(Int,     jint,     RET_INT,     i)   \
  X(Long,    jlong,    RET_LONG,    j)   \
  X(Float,   jfloat,   RET_FLOAT,   f)   \
  X(Double,  jdouble,  RET_DOUBLE,  d)

JNI_FIELD_TYPES(DECLARE_FIELD_GET_HOOKS)
JNI_FIELD_TYPES(DECLARE_FIELD_SET_HOOKS)
JNI_FIELD_TYPES(DECLARE_STATIC_FIELD_GET_HOOKS)
JNI_FIELD_TYPES(DECLARE_STATIC_FIELD_SET_HOOKS)

/* ============================================================================
 * Release*ArrayElements — paired with Get*ArrayElements hooks (lifecycle completion)
 * ============================================================================ */
#define DECLARE_RELEASE_ARRAY_ELEMENTS_HOOKS(Name, CType, ...) \
  void hooked_Release##Name##ArrayElements(JNIEnv *env, CType##Array array, CType *elems, jint mode);
JNI_PRIMITIVE_ARRAY_TYPES(DECLARE_RELEASE_ARRAY_ELEMENTS_HOOKS)

/* Critical section hooks */
void*        hooked_GetPrimitiveArrayCritical(JNIEnv *env, jarray array, jboolean *isCopy);
void         hooked_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray array, void *carray, jint mode);
const jchar* hooked_GetStringCritical(JNIEnv *env, jstring string, jboolean *isCopy);
void         hooked_ReleaseStringCritical(JNIEnv *env, jstring string, const jchar *cstring);

#endif /* JNILOG_HOOK_INTERNAL_H */
