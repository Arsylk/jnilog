/*
 * JNILog - JNI Value Visualization
 *
 * Based on frida-clockwork's visualize.ts approach.
 * Provides type-aware value formatting for JNI arguments and return values.
 *
 * NOTE: All object/array parameters use void* instead of JNI typedefs
 * (jobject, jbyteArray, etc.) for cgo compatibility. The C implementations
 * cast internally.
 */

#ifndef JNILOG_VISUALIZE_H
#define JNILOG_VISUALIZE_H

#include <jni.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Type Detection Helpers
 * ============================================================================ */

int vis_is_string(JNIEnv* env, void* obj);
int vis_is_class(JNIEnv* env, void* obj);

/* ============================================================================
 * Object Introspection
 * ============================================================================ */

char* vis_class_name(JNIEnv* env, void* clazz_ptr);
char* vis_object_class_name(JNIEnv* env, void* obj_ptr);
char* vis_object_tostring(JNIEnv* env, void* obj_ptr);
char* vis_string_value(JNIEnv* env, void* str_ptr);
/* Like vis_string_value but returns bare UTF-8 content WITHOUT wrapping quotes. */
char* vis_string_value_raw(JNIEnv* env, void* str_ptr);
void vis_set_vm(void* vm);
JNIEnv* vis_get_env(void);

/* ============================================================================
 * Per-item array encoding for the new wire protocol.
 *
 * vis_encode_array_items encodes up to MAX_ARRAY_ITEMS from a JNI array into
 * the per-item wire payload consumed by Go's buildJNIValue(KindArray, ...):
 *   itemSigChar \x04 item1 \x04 item2 \x04 ... [\x04 +N]
 *
 * itemSigChar — single JNI primitive char: Z B C S I J F D, or L for objects.
 * Items are separated by \x04. If the array is longer than MAX_ARRAY_ITEMS,
 * a trailing \x04+N element encodes the remainder count.
 * Returns a heap-allocated string; caller must free().
 *
 * vis_encode_ptr_array_items is the same but operates on a raw C pointer
 * (used by array region hooks where no JNI array object is available).
 * ============================================================================ */
char* vis_encode_array_items(JNIEnv* env, void* arr, char itemSigChar);
char* vis_encode_ptr_array_items(const void* buf, jsize count, char itemSigChar);

int extract_va_args(const char* sig, va_list ap, uintptr_t* out, int max_args);
int extract_jvalue_args(const char* sig, const jvalue* args, uintptr_t* out, int max_args);

/* ============================================================================
 * Typed argument encoding for the new wire protocol.
 *
 * vis_encode_typed_args encodes up to `count` extracted args into the wire
 * format consumed by Go's decodeArgs():
 *   sigChar \x01 primaryValue [ \x03 extraValue ] \x02  (per arg)
 *
 * Returns a heap-allocated string; caller must free().
 * ============================================================================ */
char* vis_encode_typed_args(JNIEnv* env, const char* sig,
                             uintptr_t* extracted, int count);

/* vis_object_tostring — calls obj.toString() via JNI; returns heap string. */
char* vis_object_tostring(JNIEnv* env, void* obj_ptr);

#ifdef __cplusplus
}
#endif

#endif /* JNILOG_VISUALIZE_H */
