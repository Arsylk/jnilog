#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <float.h>
#include <stdarg.h>
#include <jni.h>
#include <math.h>
#ifdef __ANDROID__
#if __has_include(<android/log.h>)
#include <android/log.h>
#endif
#endif
#include "bridge.h"
#include "visualize.h"
#include "hook_internal.h"

#define VIS_LOG_TAG "JNILogVisualize"
#if __has_include(<android/log.h>)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, VIS_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, VIS_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VIS_LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, __VA_ARGS__)
#define LOGW(...) fprintf(stderr, __VA_ARGS__)
#define LOGE(...) fprintf(stderr, __VA_ARGS__)
#endif

static JavaVM* cached_vm = NULL;

#define MAX_ARRAY_ITEMS 16
#define MAX_ARRAY_CHARS 200

static int vis_safe_to_call(JNIEnv* env) {
    if (env == NULL || g_original_jni_table == NULL) return 0;
    if (is_jni_critical()) return 0; // JNI calls prohibited during critical sections
    if (g_original_jni_table->ExceptionCheck == NULL) return 0;
    if (g_original_jni_table->ExceptionCheck(env)) return 0;
    return 1;
}

void vis_set_vm(void* vm) {
    cached_vm = (JavaVM*)vm;
    LOGI("Visualization helpers VM set: %p", (void*)cached_vm);
}

JNIEnv* vis_get_env(void) {
    if (cached_vm == NULL) return NULL;
    JNIEnv* env = NULL;
    jint rc = (*cached_vm)->GetEnv(cached_vm, (void**)&env, JNI_VERSION_1_6);
    if (rc == JNI_OK) return env;
    if (rc == JNI_EDETACHED) {
        if ((*cached_vm)->AttachCurrentThread(cached_vm, &env, NULL) == JNI_OK) return env;
    }
    return NULL;
}

char* vis_format_bool(jboolean value) {
    return strdup(value ? "true" : "false");
}

char* vis_format_byte(jbyte value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    return strdup(buf);
}

char* vis_format_char(jchar value) {
    char buf[16];
    unsigned int c = (unsigned int)value;
    if (c == '\n') snprintf(buf, sizeof(buf), "'\\n'");
    else if (c == '\r') snprintf(buf, sizeof(buf), "'\\r'");
    else if (c == '\t') snprintf(buf, sizeof(buf), "'\\t'");
    else if (c >= 32 && c <= 126) {
        if (c == '\'' || c == '\\') snprintf(buf, sizeof(buf), "'\\%c'", c);
        else snprintf(buf, sizeof(buf), "'%c'", c);
    } else snprintf(buf, sizeof(buf), "'\\u%04x'", c);
    return strdup(buf);
}

char* vis_format_short(jshort value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    return strdup(buf);
}

char* vis_format_int(jint value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    return strdup(buf);
}

char* vis_format_long(jlong value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return strdup(buf);
}

char* vis_format_float(jfloat value) {
    char buf[64];
    if (value != value) snprintf(buf, sizeof(buf), "NaN");
    else if (isinf(value)) snprintf(buf, sizeof(buf), "%s", value > 0 ? "Infinity" : "-Infinity");
    else snprintf(buf, sizeof(buf), "%g", value);
    return strdup(buf);
}

char* vis_format_double(jdouble value) {
    char buf[64];
    if (value != value) snprintf(buf, sizeof(buf), "NaN");
    else if (isinf(value)) snprintf(buf, sizeof(buf), "%s", value > 0 ? "Infinity" : "-Infinity");
    else snprintf(buf, sizeof(buf), "%lg", value);
    return strdup(buf);
}

char* vis_format_void(void) { return strdup("void"); }

int vis_is_array(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    jclass objClass = g_original_jni_table->GetObjectClass(env, (jobject)obj);
    if (objClass == NULL) { set_reentrant_call(0); return 0; }
    jclass classClass = g_original_jni_table->GetObjectClass(env, objClass);
    jmethodID isArrayMethod = g_original_jni_table->GetMethodID(env, classClass, "isArray", "()Z");
    jboolean isArray = JNI_FALSE;
    if (isArrayMethod) isArray = g_original_jni_table->CallBooleanMethod(env, (jobject)objClass, isArrayMethod);
    else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, classClass);
    g_original_jni_table->DeleteLocalRef(env, objClass);
    set_reentrant_call(0);
    return isArray ? 1 : 0;
}

int vis_is_primitive_array(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    char* className = vis_object_class_name(env, obj);
    if (className == NULL) return 0;
    int res = (strlen(className) >= 2 && className[0] == '[' && strchr("ZBCSIFJD", className[1]) != NULL);
    free(className);
    return res;
}

char* vis_get_array_component_type(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jclass objClass = g_original_jni_table->GetObjectClass(env, (jobject)obj);
    if (objClass == NULL) { set_reentrant_call(0); return NULL; }
    jclass classClass = g_original_jni_table->GetObjectClass(env, objClass);
    jmethodID getCompMethod = g_original_jni_table->GetMethodID(env, classClass, "getComponentType", "()Ljava/lang/Class;");
    char* result = NULL;
    if (getCompMethod) {
        jclass compClass = (jclass)g_original_jni_table->CallObjectMethod(env, objClass, getCompMethod);
        if (compClass) {
            result = vis_class_name(env, compClass);
            g_original_jni_table->DeleteLocalRef(env, compClass);
        }
    } else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, classClass);
    g_original_jni_table->DeleteLocalRef(env, objClass);
    set_reentrant_call(0);
    return result;
}

char* vis_format_boolean_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jbooleanArray jarr = (jbooleanArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jboolean* el = g_original_jni_table->GetBooleanArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 12 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "%s", el[i] ? "true" : "false");
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseBooleanArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_byte_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jbyteArray jarr = (jbyteArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jbyte* el = g_original_jni_table->GetByteArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 12 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "0x%02x", (unsigned char)el[i]);
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d bytes)", (int)len);
    sprintf(p, "]");
    g_original_jni_table->ReleaseByteArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_char_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jcharArray jarr = (jcharArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jchar* el = g_original_jni_table->GetCharArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 12 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        unsigned int c = (unsigned int)el[i];
        if (c == '\n') p += sprintf(p, "'\\n'");
        else if (c == '\r') p += sprintf(p, "'\\r'");
        else if (c == '\t') p += sprintf(p, "'\\t'");
        else if (c >= 32 && c <= 126) {
            if (c == '\'' || c == '\\') p += sprintf(p, "'\\%c'", c);
            else p += sprintf(p, "'%c'", c);
        } else {
            p += sprintf(p, "'\\u%04x'", c);
        }
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseCharArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_short_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jshortArray jarr = (jshortArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jshort* el = g_original_jni_table->GetShortArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 12 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "%d", (int)el[i]);
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseShortArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_int_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jintArray jarr = (jintArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jint* el = g_original_jni_table->GetIntArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 24 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "%d", (int)el[i]);
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseIntArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_long_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jlongArray jarr = (jlongArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jlong* el = g_original_jni_table->GetLongArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 32 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "%lld", (long long)el[i]);
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseLongArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_float_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jfloatArray jarr = (jfloatArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jfloat* el = g_original_jni_table->GetFloatArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 32 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "%g", el[i]);
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseFloatArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_double_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    if (!vis_safe_to_call(env)) return strdup("0x0");
    set_reentrant_call(1);
    jdoubleArray jarr = (jdoubleArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    jdouble* el = g_original_jni_table->GetDoubleArrayElements(env, jarr, NULL);
    if (el == NULL) { set_reentrant_call(0); return strdup("[error]"); }
    char* buf = (char*)malloc(dlen * 32 + 64);
    char* p = buf; p += sprintf(p, "[");
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) p += sprintf(p, ", ");
        p += sprintf(p, "%lg", el[i]);
    }
    if (len > MAX_ARRAY_ITEMS) p += sprintf(p, ", ... (%d more)", (int)(len - dlen));
    sprintf(p, "]");
    g_original_jni_table->ReleaseDoubleArrayElements(env, jarr, el, JNI_ABORT);
    set_reentrant_call(0);
    return buf;
}

char* vis_format_object_array(JNIEnv* env, void* arr) {
    if (arr == NULL || !vis_safe_to_call(env)) return strdup("null");
    set_reentrant_call(1);
    jobjectArray jarr = (jobjectArray)arr;
    jsize len = g_original_jni_table->GetArrayLength(env, jarr);
    if (len == 0) { set_reentrant_call(0); return strdup("[]"); }
    jsize dlen = len > MAX_ARRAY_ITEMS ? MAX_ARRAY_ITEMS : len;
    
    size_t capacity = 1024;
    char* buf = (char*)malloc(capacity);
    size_t pos = 0;
    pos += snprintf(buf + pos, capacity - pos, "[");
    
    for (jsize i = 0; i < dlen; i++) {
        if (i > 0) {
            if (pos + 4 >= capacity) { capacity *= 2; buf = (char*)realloc(buf, capacity); }
            pos += snprintf(buf + pos, capacity - pos, ", ");
        }
        jobject e = g_original_jni_table->GetObjectArrayElement(env, jarr, i);
        if (e) {
            set_reentrant_call(0); // Recursion handle
            char* s = vis_format_object_smart(env, e); 
            set_reentrant_call(1);
            size_t slen = strlen(s);
            if (pos + slen + 4 >= capacity) { 
                while (pos + slen + 4 >= capacity) capacity *= 2; 
                buf = (char*)realloc(buf, capacity); 
            }
            pos += snprintf(buf + pos, capacity - pos, "%s", s); free(s);
            g_original_jni_table->DeleteLocalRef(env, e);
        } else {
            if (pos + 8 >= capacity) { capacity *= 2; buf = (char*)realloc(buf, capacity); }
            pos += snprintf(buf + pos, capacity - pos, "null");
        }
    }
    if (len > MAX_ARRAY_ITEMS) {
        if (pos + 64 >= capacity) { capacity += 128; buf = (char*)realloc(buf, capacity); }
        pos += snprintf(buf + pos, capacity - pos, ", ... (%d more)", (int)(len - dlen));
    }
    snprintf(buf + pos, capacity - pos, "]");
    set_reentrant_call(0);
    return buf;
}

char* vis_format_array(JNIEnv* env, void* arr) {
    if (arr == NULL) return strdup("null");
    char* cn = vis_object_class_name(env, arr);
    if (!cn) return strdup("[unknown]");
    char* res = NULL;
    if (strncmp(cn, "[Z", 2) == 0) res = vis_format_boolean_array(env, arr);
    else if (strncmp(cn, "[B", 2) == 0) res = vis_format_byte_array(env, arr);
    else if (strncmp(cn, "[C", 2) == 0) res = vis_format_char_array(env, arr);
    else if (strncmp(cn, "[S", 2) == 0) res = vis_format_short_array(env, arr);
    else if (strncmp(cn, "[I", 2) == 0) res = vis_format_int_array(env, arr);
    else if (strncmp(cn, "[J", 2) == 0) res = vis_format_long_array(env, arr);
    else if (strncmp(cn, "[F", 2) == 0) res = vis_format_float_array(env, arr);
    else if (strncmp(cn, "[D", 2) == 0) res = vis_format_double_array(env, arr);
    else res = vis_format_object_array(env, arr);
    free(cn); return res;
}

int vis_is_string(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    if (!g_original_jni_table->FindClass || !g_original_jni_table->IsInstanceOf) { set_reentrant_call(0); return 0; }
    jclass sc = g_original_jni_table->FindClass(env, "java/lang/String");
    if (!sc) { g_original_jni_table->ExceptionClear(env); set_reentrant_call(0); return 0; }
    jboolean r = g_original_jni_table->IsInstanceOf(env, (jobject)obj, sc);
    g_original_jni_table->DeleteLocalRef(env, sc);
    set_reentrant_call(0);
    return r ? 1 : 0;
}

int vis_is_boolean_object(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    if (!g_original_jni_table->FindClass || !g_original_jni_table->IsInstanceOf) { set_reentrant_call(0); return 0; }
    jclass bc = g_original_jni_table->FindClass(env, "java/lang/Boolean");
    if (!bc) { g_original_jni_table->ExceptionClear(env); set_reentrant_call(0); return 0; }
    jboolean r = g_original_jni_table->IsInstanceOf(env, (jobject)obj, bc);
    g_original_jni_table->DeleteLocalRef(env, bc);
    set_reentrant_call(0);
    return r ? 1 : 0;
}

int vis_is_integer_object(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    if (!g_original_jni_table->FindClass || !g_original_jni_table->IsInstanceOf) { set_reentrant_call(0); return 0; }
    jclass ic = g_original_jni_table->FindClass(env, "java/lang/Integer");
    if (!ic) { g_original_jni_table->ExceptionClear(env); set_reentrant_call(0); return 0; }
    jboolean r = g_original_jni_table->IsInstanceOf(env, (jobject)obj, ic);
    g_original_jni_table->DeleteLocalRef(env, ic);
    set_reentrant_call(0);
    return r ? 1 : 0;
}

int vis_is_long_object(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    if (!g_original_jni_table->FindClass || !g_original_jni_table->IsInstanceOf) { set_reentrant_call(0); return 0; }
    jclass lc = g_original_jni_table->FindClass(env, "java/lang/Long");
    if (!lc) { g_original_jni_table->ExceptionClear(env); set_reentrant_call(0); return 0; }
    jboolean r = g_original_jni_table->IsInstanceOf(env, (jobject)obj, lc);
    g_original_jni_table->DeleteLocalRef(env, lc);
    set_reentrant_call(0);
    return r ? 1 : 0;
}

int vis_is_class(JNIEnv* env, void* obj) {
    if (obj == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    if (!g_original_jni_table->FindClass || !g_original_jni_table->IsInstanceOf) { set_reentrant_call(0); return 0; }
    jclass cc = g_original_jni_table->FindClass(env, "java/lang/Class");
    if (!cc) { g_original_jni_table->ExceptionClear(env); set_reentrant_call(0); return 0; }
    jboolean r = g_original_jni_table->IsInstanceOf(env, (jobject)obj, cc);
    g_original_jni_table->DeleteLocalRef(env, cc);
    set_reentrant_call(0);
    return r ? 1 : 0;
}

char* vis_format_boolean_object(JNIEnv* env, void* obj) {
    if (!vis_is_boolean_object(env, obj)) return NULL;
    set_reentrant_call(1);
    jclass bc = g_original_jni_table->GetObjectClass(env, (jobject)obj);
    jmethodID mid = g_original_jni_table->GetMethodID(env, bc, "booleanValue", "()Z");
    char* res = NULL;
    if (mid) res = vis_format_bool(g_original_jni_table->CallBooleanMethod(env, (jobject)obj, mid));
    else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, bc);
    set_reentrant_call(0);
    return res;
}

char* vis_format_integer_object(JNIEnv* env, void* obj) {
    if (!vis_is_integer_object(env, obj)) return NULL;
    set_reentrant_call(1);
    jclass ic = g_original_jni_table->GetObjectClass(env, (jobject)obj);
    jmethodID mid = g_original_jni_table->GetMethodID(env, ic, "intValue", "()I");
    char* res = NULL;
    if (mid) res = vis_format_int(g_original_jni_table->CallIntMethod(env, (jobject)obj, mid));
    else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, ic);
    set_reentrant_call(0);
    return res;
}

char* vis_format_long_object(JNIEnv* env, void* obj) {
    if (!vis_is_long_object(env, obj)) return NULL;
    set_reentrant_call(1);
    jclass lc = g_original_jni_table->GetObjectClass(env, (jobject)obj);
    jmethodID mid = g_original_jni_table->GetMethodID(env, lc, "longValue", "()J");
    char* res = NULL;
    if (mid) res = vis_format_long(g_original_jni_table->CallLongMethod(env, (jobject)obj, mid));
    else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, lc);
    set_reentrant_call(0);
    return res;
}

char* vis_class_name(JNIEnv* env, void* clazz_ptr) {
    if (clazz_ptr == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jclass clazz = (jclass)clazz_ptr;
    jclass cc = g_original_jni_table->GetObjectClass(env, clazz);
    jmethodID mid = g_original_jni_table->GetMethodID(env, cc, "getName", "()Ljava/lang/String;");
    char* res = NULL;
    if (mid) {
        jstring ns = (jstring)g_original_jni_table->CallObjectMethod(env, clazz, mid);
        if (ns) {
            const char* c = g_original_jni_table->GetStringUTFChars(env, ns, NULL);
            if (c) { res = strdup(c); g_original_jni_table->ReleaseStringUTFChars(env, ns, c); }
            g_original_jni_table->DeleteLocalRef(env, ns);
        }
    } else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, cc);
    set_reentrant_call(0);
    return res;
}

char* vis_object_class_name(JNIEnv* env, void* obj_ptr) {
    if (obj_ptr == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jclass c = g_original_jni_table->GetObjectClass(env, (jobject)obj_ptr);
    if (!c) { set_reentrant_call(0); return NULL; }
    set_reentrant_call(0);
    char* res = vis_class_name(env, c);
    set_reentrant_call(1);
    g_original_jni_table->DeleteLocalRef(env, c);
    set_reentrant_call(0);
    return res;
}

char* vis_object_tostring(JNIEnv* env, void* obj_ptr) {
    if (obj_ptr == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jclass c = g_original_jni_table->GetObjectClass(env, (jobject)obj_ptr);
    if (!c) { set_reentrant_call(0); return NULL; }
    jmethodID mid = g_original_jni_table->GetMethodID(env, c, "toString", "()Ljava/lang/String;");
    char* res = NULL;
    if (mid) {
        jstring s = (jstring)g_original_jni_table->CallObjectMethod(env, (jobject)obj_ptr, mid);
        if (s) {
            const char* ch = g_original_jni_table->GetStringUTFChars(env, s, NULL);
            if (ch) { res = strdup(ch); g_original_jni_table->ReleaseStringUTFChars(env, s, ch); }
            g_original_jni_table->DeleteLocalRef(env, s);
        }
    } else g_original_jni_table->ExceptionClear(env);
    g_original_jni_table->DeleteLocalRef(env, c);
    set_reentrant_call(0);
    return res;
}

char* vis_string_value(JNIEnv* env, void* str_ptr) {
    if (str_ptr == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jstring s = (jstring)str_ptr;
    const char* c = g_original_jni_table->GetStringUTFChars(env, s, NULL);
    char* res = NULL;
    if (c) {
        res = (char*)malloc(strlen(c) + 3);
        sprintf(res, "\"%s\"", c);
        g_original_jni_table->ReleaseStringUTFChars(env, s, c);
    }
    set_reentrant_call(0);
    return res;
}

char* vis_string_value_raw(JNIEnv* env, void* str_ptr) {
    if (str_ptr == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jstring s = (jstring)str_ptr;
    const char* c = g_original_jni_table->GetStringUTFChars(env, s, NULL);
    char* res = NULL;
    if (c) {
        res = strdup(c);
        g_original_jni_table->ReleaseStringUTFChars(env, s, c);
    }
    set_reentrant_call(0);
    return res;
}

/* ============================================================================
 * vis_encode_array_items — encodes array elements into the \x04-separated
 * wire format consumed by Go's buildJNIValue(KindArray, ...).
 *
 * Wire format returned (no outer brackets):
 *   itemSigChar \x04 item1 \x04 item2 ... [ \x04 +N ]
 *
 * itemSigChar is the first byte; items are separated by \x04.
 * If the array is longer than MAX_ARRAY_ITEMS, a trailing "+N" element is
 * appended where N is the number of omitted items.
 *
 * Returns a heap-allocated string; caller must free().
 * Returns strdup("") for NULL/empty arrays.
 * ============================================================================ */
#define VIS_MAX_ARRAY_ITEMS 16

/* Grow-buffer helpers (mirrors vis_encode_typed_args.c) */
static char *vea_append(char *buf, size_t *len, size_t *cap, const char *src, size_t n) {
    while (*len + n + 1 > *cap) {
        *cap = (*cap < 128) ? 256 : (*cap * 2);
        buf = (char*)realloc(buf, *cap);
    }
    memcpy(buf + *len, src, n);
    *len += n;
    buf[*len] = '\0';
    return buf;
}
#define VEA_LIT(b, l, c, s) vea_append(b, l, c, s, strlen(s))
#define VEA_CH(b, l, c, ch) do { char _c = (ch); b = vea_append(b, l, c, &_c, 1); } while(0)

char* vis_encode_array_items(JNIEnv* env, void* arr, char itemSigChar) {
    if (arr == NULL || !vis_safe_to_call(env)) return strdup("");

    size_t cap = 512, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    /* First byte = itemSigChar */
    VEA_CH(buf, &len, &cap, itemSigChar);

    set_reentrant_call(1);
    jsize total = g_original_jni_table->GetArrayLength(env, (jarray)arr);
    jsize count = total > VIS_MAX_ARRAY_ITEMS ? VIS_MAX_ARRAY_ITEMS : total;

    char tmp[64];

    switch (itemSigChar) {
    case 'Z': {
        jboolean *elems = g_original_jni_table->GetBooleanArrayElements(env, (jbooleanArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                buf = VEA_LIT(buf, &len, &cap, elems[i] ? "1" : "0");
            }
            g_original_jni_table->ReleaseBooleanArrayElements(env, (jbooleanArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'B': {
        jbyte *elems = g_original_jni_table->GetByteArrayElements(env, (jbyteArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%d", (int)elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseByteArrayElements(env, (jbyteArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'C': {
        jchar *elems = g_original_jni_table->GetCharArrayElements(env, (jcharArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%u", (unsigned)elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseCharArrayElements(env, (jcharArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'S': {
        jshort *elems = g_original_jni_table->GetShortArrayElements(env, (jshortArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%d", (int)elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseShortArrayElements(env, (jshortArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'I': {
        jint *elems = g_original_jni_table->GetIntArrayElements(env, (jintArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%d", (int)elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseIntArrayElements(env, (jintArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'J': {
        jlong *elems = g_original_jni_table->GetLongArrayElements(env, (jlongArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%lld", (long long)elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseLongArrayElements(env, (jlongArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'F': {
        jfloat *elems = g_original_jni_table->GetFloatArrayElements(env, (jfloatArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%g", (double)elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseFloatArrayElements(env, (jfloatArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'D': {
        jdouble *elems = g_original_jni_table->GetDoubleArrayElements(env, (jdoubleArray)arr, NULL);
        if (elems) {
            for (jsize i = 0; i < count; i++) {
                VEA_CH(buf, &len, &cap, '\x04');
                snprintf(tmp, sizeof(tmp), "%g", elems[i]);
                buf = VEA_LIT(buf, &len, &cap, tmp);
            }
            g_original_jni_table->ReleaseDoubleArrayElements(env, (jdoubleArray)arr, elems, JNI_ABORT);
        }
        break;
    }
    case 'L': {
        /* Object array — temporarily clear reentrant flag so vis_* helpers work */
        for (jsize i = 0; i < count; i++) {
            set_reentrant_call(0);
            jobject elem = g_original_jni_table->GetObjectArrayElement(env, (jobjectArray)arr, i);
            VEA_CH(buf, &len, &cap, '\x04');
            if (!elem) {
                buf = VEA_LIT(buf, &len, &cap, "null");
            } else if (vis_is_string(env, elem)) {
                char *sv = vis_string_value_raw(env, elem);
                if (sv) { buf = vea_append(buf, &len, &cap, sv, strlen(sv)); free(sv); }
                else buf = VEA_LIT(buf, &len, &cap, "");
            } else {
                char *cn = vis_object_class_name(env, elem);
                char *ts = vis_object_tostring(env, elem);
                if (cn) { buf = vea_append(buf, &len, &cap, cn, strlen(cn)); free(cn); }
                if (ts) {
                    VEA_CH(buf, &len, &cap, '\x03');
                    buf = vea_append(buf, &len, &cap, ts, strlen(ts));
                    free(ts);
                }
            }
            g_original_jni_table->DeleteLocalRef(env, elem);
            set_reentrant_call(1);
        }
        break;
    }
    default:
        break;
    }

    /* Append remainder count if truncated */
    if (total > VIS_MAX_ARRAY_ITEMS) {
        jsize remainder = total - VIS_MAX_ARRAY_ITEMS;
        VEA_CH(buf, &len, &cap, '\x04');
        snprintf(tmp, sizeof(tmp), "+%d", (int)remainder);
        buf = VEA_LIT(buf, &len, &cap, tmp);
    }

    set_reentrant_call(0);
    return buf;
}

/* ============================================================================
 * vis_encode_ptr_array_items — same wire format as vis_encode_array_items but
 * operates on a raw C pointer (for Get/SetArrayRegion buf argument).
 * Does NOT need JNI or reentrant flag management.
 * ============================================================================ */
char* vis_encode_ptr_array_items(const void* buf, jsize count, char itemSigChar) {
    if (!buf || count <= 0) return strdup("");

    size_t cap = 512, len = 0;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    /* First byte = itemSigChar */
    VEA_CH(out, &len, &cap, itemSigChar);

    jsize limit = count > VIS_MAX_ARRAY_ITEMS ? VIS_MAX_ARRAY_ITEMS : count;
    char tmp[64];

    for (jsize i = 0; i < limit; i++) {
        VEA_CH(out, &len, &cap, '\x04');
        switch (itemSigChar) {
        case 'Z': snprintf(tmp, sizeof(tmp), "%d", (int)((const jboolean*)buf)[i]); break;
        case 'B': snprintf(tmp, sizeof(tmp), "%d", (int)((const jbyte*)buf)[i]);    break;
        case 'C': snprintf(tmp, sizeof(tmp), "%u", (unsigned)((const jchar*)buf)[i]); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%d", (int)((const jshort*)buf)[i]);   break;
        case 'I': snprintf(tmp, sizeof(tmp), "%d", (int)((const jint*)buf)[i]);     break;
        case 'J': snprintf(tmp, sizeof(tmp), "%lld", (long long)((const jlong*)buf)[i]); break;
        case 'F': snprintf(tmp, sizeof(tmp), "%g", (double)((const jfloat*)buf)[i]); break;
        case 'D': snprintf(tmp, sizeof(tmp), "%g", ((const jdouble*)buf)[i]);       break;
        default:  snprintf(tmp, sizeof(tmp), "?");                                  break;
        }
        out = VEA_LIT(out, &len, &cap, tmp);
    }

    if (count > VIS_MAX_ARRAY_ITEMS) {
        VEA_CH(out, &len, &cap, '\x04');
        snprintf(tmp, sizeof(tmp), "+%d", (int)(count - VIS_MAX_ARRAY_ITEMS));
        out = VEA_LIT(out, &len, &cap, tmp);
    }

    return out;
}

char* vis_format_value(JNIEnv* env, uintptr_t value, const char* sig) {
    if (sig == NULL) return value == 0 ? strdup("null") : vis_format_object_smart(env, (void*)value);
    if (strcmp(sig, "Z") == 0) return vis_format_bool((jboolean)value);
    if (strcmp(sig, "B") == 0) return vis_format_byte((jbyte)value);
    if (strcmp(sig, "C") == 0) return vis_format_char((jchar)value);
    if (strcmp(sig, "S") == 0) return vis_format_short((jshort)value);
    if (strcmp(sig, "I") == 0) return vis_format_int((jint)value);
    if (strcmp(sig, "J") == 0) return vis_format_long((jlong)value);
    if (strcmp(sig, "F") == 0) return vis_format_float(*((jfloat*)&value));
    if (strcmp(sig, "D") == 0) { char b[64]; snprintf(b, sizeof(b), "0x%llx", (unsigned long long)value); return strdup(b); }
    if (strcmp(sig, "V") == 0) return vis_format_void();
    if (value == 0) return strdup("null");
    if (sig[0] == '[') return vis_format_array(env, (void*)value);
    if (strcmp(sig, "Ljava/lang/String;") == 0) {
        char* sv = vis_string_value_raw(env, (void*)value);
        if (sv) return sv;
        return strdup("");
    }
    if (strcmp(sig, "Ljava/lang/Class;") == 0) {
        char* cn = vis_class_name(env, (void*)value);
        if (cn) { return cn; }
        return strdup("?");
    }
    return vis_format_object_smart(env, (void*)value);
}

static int vis_is_instance_of(JNIEnv* env, jobject obj, const char* className) {
    if (obj == NULL || className == NULL || !vis_safe_to_call(env)) return 0;
    set_reentrant_call(1);
    jclass clazz = g_original_jni_table->FindClass(env, className);
    if (!clazz) { g_original_jni_table->ExceptionClear(env); set_reentrant_call(0); return 0; }
    jboolean res = g_original_jni_table->IsInstanceOf(env, obj, clazz);
    g_original_jni_table->DeleteLocalRef(env, clazz);
    set_reentrant_call(0);
    return res ? 1 : 0;
}

char* vis_format_object_smart(JNIEnv* env, void* obj_ptr) {
    if (obj_ptr == NULL) return strdup("null");
    jobject obj = (jobject)obj_ptr;

    if (!vis_safe_to_call(env)) {
        return strdup("Object");
    }

    if (vis_is_string(env, obj)) {
        char* val = vis_string_value_raw(env, obj);
        if (val) {
            return val;
        }
    }

    if (vis_is_long_object(env, obj)) return vis_format_long_object(env, obj);
    if (vis_is_integer_object(env, obj)) return vis_format_integer_object(env, obj);
    if (vis_is_boolean_object(env, obj)) return vis_format_boolean_object(env, obj);

    if (vis_is_class(env, obj)) {
        char* cn = vis_class_name(env, obj);
        if (cn) {
            return cn;
        }
    }

    if (vis_is_instance_of(env, obj, "android/view/InputDevice")) {
        set_reentrant_call(1);
        jclass clazz = g_original_jni_table->GetObjectClass(env, obj);
        jmethodID mid = g_original_jni_table->GetMethodID(env, clazz, "getName", "()Ljava/lang/String;");
        if (mid) {
            jstring name = (jstring)g_original_jni_table->CallObjectMethod(env, obj, mid);
            if (name) {
                const char* cname = g_original_jni_table->GetStringUTFChars(env, name, NULL);
                char* buf = (char*)malloc(strlen(cname) + 64);
                sprintf(buf, "InputDevice(%s)", cname);
                g_original_jni_table->ReleaseStringUTFChars(env, name, cname);
                g_original_jni_table->DeleteLocalRef(env, name);
                g_original_jni_table->DeleteLocalRef(env, clazz);
                set_reentrant_call(0);
                return buf;
            }
        }
        g_original_jni_table->DeleteLocalRef(env, clazz);
        set_reentrant_call(0);
    }
    
    if (vis_is_instance_of(env, obj, "java/security/cert/Certificate")) {
        set_reentrant_call(1);
        jclass clazz = g_original_jni_table->GetObjectClass(env, obj);
        jmethodID mid = g_original_jni_table->GetMethodID(env, clazz, "getType", "()Ljava/lang/String;");
        if (mid) {
            jstring type = (jstring)g_original_jni_table->CallObjectMethod(env, obj, mid);
            if (type) {
                const char* ctype = g_original_jni_table->GetStringUTFChars(env, type, NULL);
                char* buf = (char*)malloc(strlen(ctype) + 64);
                sprintf(buf, "Certificate(%s)", ctype);
                g_original_jni_table->ReleaseStringUTFChars(env, type, ctype);
                g_original_jni_table->DeleteLocalRef(env, type);
                g_original_jni_table->DeleteLocalRef(env, clazz);
                set_reentrant_call(0);
                return buf;
            }
        }
        g_original_jni_table->DeleteLocalRef(env, clazz);
        set_reentrant_call(0);
    }

    if (vis_is_instance_of(env, obj, "java/security/cert/X509Certificate") ||
        vis_is_instance_of(env, obj, "org/conscrypt/OpenSSLX509Certificate")) {
        set_reentrant_call(1);
        jclass clazz = g_original_jni_table->GetObjectClass(env, obj);
        jmethodID mid = g_original_jni_table->GetMethodID(env, clazz, "getIssuerX500Principal", "()Ljava/security/Principal;");
        if (mid) {
            jobject principal = g_original_jni_table->CallObjectMethod(env, obj, mid);
            if (principal) {
                char* pstr = vis_object_tostring(env, principal);
                char* buf = (char*)malloc(strlen(pstr) + 128);
                sprintf(buf, "X509Certificate(issuer=%s)", pstr);
                free(pstr);
                g_original_jni_table->DeleteLocalRef(env, principal);
                g_original_jni_table->DeleteLocalRef(env, clazz);
                set_reentrant_call(0);
                return buf;
            }
        }
        g_original_jni_table->DeleteLocalRef(env, clazz);
        set_reentrant_call(0);
    }

    if (vis_is_instance_of(env, obj, "android/preference/Preference$Key") || 
        vis_is_instance_of(env, obj, "androidx/preference/Preference$Key")) {
        char* val = vis_object_tostring(env, obj);
        if (val) {
            char* buf = (char*)malloc(strlen(val) + 64);
            sprintf(buf, "Preferences$Key(%s)", val);
            free(val);
            return buf;
        }
    }

    if (vis_is_instance_of(env, obj, "android/graphics/Matrix")) {
        return strdup("Matrix");
    }

    if (vis_is_instance_of(env, obj, "dalvik/system/PathClassLoader")) {
        return strdup("PathClassLoader");
    }

    if (vis_is_instance_of(env, obj, "android/view/WindowInsets")) {
        return strdup("WindowInsets");
    }

    char* className = vis_object_class_name(env, obj);
    char* toString = vis_object_tostring(env, obj);
    char b[512];
    if (className && toString) {
        snprintf(b, sizeof(b), "%s(%s)", className, toString);
    } else if (className) {
        snprintf(b, sizeof(b), "%s", className);
    } else {
        snprintf(b, sizeof(b), "Object");
    }
    free(className);
    free(toString);
    return strdup(b);
}

char* vis_format_arguments(JNIEnv* env, const char* sig, uintptr_t* args, int count) {
    if (!sig || sig[0] != '(' || !args || count <= 0) return strdup("()");
    char* buf = (char*)malloc(512); char* p = buf; p += sprintf(p, "(");
    const char* s = sig + 1; int idx = 0;
    while (*s != ')' && *s != '\0' && idx < count) {
        if (idx > 0) p += sprintf(p, ", ");
        char ts[256]; int tl = 0;
        if (*s == 'L') { const char* e = strchr(s, ';'); if (!e) break; tl = e - s + 1; strncpy(ts, s, tl); ts[tl] = '\0'; s = e + 1; }
        else if (*s == '[') { const char* st = s; while (*s == '[') s++; if (*s == 'L') { const char* e = strchr(s, ';'); if (!e) break; s = e + 1; } else s++; tl = s - st; strncpy(ts, st, tl); ts[tl] = '\0'; }
        else { ts[0] = *s++; ts[1] = '\0'; }
        char* f = vis_format_value(env, args[idx++], ts); p += sprintf(p, "%s", f); free(f);
    }
    sprintf(p, ")"); return buf;
}

int extract_va_args(const char* sig, va_list ap, uintptr_t* out, int max) {
    if (!sig || sig[0] != '(') return 0;
    const char* s = sig + 1; int n = 0;
    while (*s != ')' && *s != '\0' && n < max) {
        if (*s == 'L' || *s == '[') { out[n++] = (uintptr_t)va_arg(ap, jobject); if (*s == 'L') s = strchr(s, ';') + 1; else { while (*s == '[') s++; if (*s == 'L') s = strchr(s, ';') + 1; else s++; } }
        else switch (*s++) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': out[n++] = (uintptr_t)va_arg(ap, jint); break;
            case 'J': out[n++] = (uintptr_t)va_arg(ap, jlong); break;
            case 'F': { double f = va_arg(ap, double); float fl = (float)f; out[n++] = *((uintptr_t*)&fl); break; }
            case 'D': { double d = va_arg(ap, double); out[n++] = *((uintptr_t*)&d); break; }
        }
    }
    return n;
}

int extract_jvalue_args(const char* sig, const jvalue* args, uintptr_t* out, int max) {
    if (!sig || sig[0] != '(' || !args) return 0;
    const char* s = sig + 1; int n = 0;
    while (*s != ')' && *s != '\0' && n < max) {
        if (*s == 'L' || *s == '[') { out[n] = (uintptr_t)args[n].l; n++; if (*s == 'L') s = strchr(s, ';') + 1; else { while (*s == '[') s++; if (*s == 'L') s = strchr(s, ';') + 1; else s++; } }
        else switch (*s++) {
            case 'Z': out[n] = (uintptr_t)args[n].z; n++; break;
            case 'B': out[n] = (uintptr_t)args[n].b; n++; break;
            case 'C': out[n] = (uintptr_t)args[n].c; n++; break;
            case 'S': out[n] = (uintptr_t)args[n].s; n++; break;
            case 'I': out[n] = (uintptr_t)args[n].i; n++; break;
            case 'J': out[n] = (uintptr_t)args[n].j; n++; break;
            case 'F': out[n] = *((uintptr_t*)&args[n].f); n++; break;
            case 'D': out[n] = *((uintptr_t*)&args[n].d); n++; break;
        }
    }
    return n;
}
