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

char* vis_class_name(JNIEnv* env, void* clazz_ptr) {
    if (clazz_ptr == NULL || !vis_safe_to_call(env)) return NULL;
    set_reentrant_call(1);
    jclass clazz = (jclass)clazz_ptr;
    jclass cc = g_original_jni_table->GetObjectClass(env, clazz);
    /* GetObjectClass can return NULL on OOM or with a stale jclass; sibling
     * helpers (vis_object_class_name / vis_object_tostring_safe) check; this
     * one previously did not and would SIGSEGV inside GetMethodID. */
    if (!cc) {
        if (g_original_jni_table->ExceptionCheck(env)) g_original_jni_table->ExceptionClear(env);
        set_reentrant_call(0);
        return NULL;
    }
    jmethodID mid = g_original_jni_table->GetMethodID(env, cc, "getName", "()Ljava/lang/String;");
    char* res = NULL;
    if (mid) {
        jstring ns = (jstring)g_original_jni_table->CallObjectMethod(env, clazz, mid);
        /* Defensive: an overridden Class.getName() that throws would otherwise
         * leak its pending exception back into the target's flow when the
         * logger returns. Clear unconditionally — we already failed to read
         * the name; the caller can't recover the value either way. */
        if (g_original_jni_table->ExceptionCheck(env)) {
            g_original_jni_table->ExceptionClear(env);
        }
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

/* Classes whose toString() has destructive side effects (consumes the object).
 * We skip toString() for these and return NULL so the formatter uses class-name-only rendering. */
static const char* const vis_tostring_blocklist[] = {
    "com.facebook.react.bridge.WritableNativeMap",
    "com.facebook.react.bridge.ReadableNativeMap",
    "com.facebook.react.bridge.WritableNativeArray",
    "com.facebook.react.bridge.ReadableNativeArray",
    "com.facebook.react.bridge.NativeMap",
    "com.facebook.react.bridge.NativeArray",
    NULL
};

static int vis_is_tostring_blocked(const char* class_name) {
    if (!class_name) return 0;
    for (int i = 0; vis_tostring_blocklist[i] != NULL; i++) {
        if (strcmp(class_name, vis_tostring_blocklist[i]) == 0) return 1;
    }
    return 0;
}

char* vis_object_tostring(JNIEnv* env, void* obj_ptr) {
    return vis_object_tostring_safe(env, obj_ptr, NULL);
}

/* vis_object_tostring_safe — like vis_object_tostring but accepts a pre-resolved
 * class name to check against the blocklist. If class_name is NULL, resolves it. */
char* vis_object_tostring_safe(JNIEnv* env, void* obj_ptr, const char* class_name) {
    if (obj_ptr == NULL || !vis_safe_to_call(env)) return NULL;

    /* Check blocklist using provided or resolved class name */
    char* resolved_name = NULL;
    const char* check_name = class_name;
    if (!check_name) {
        resolved_name = vis_object_class_name(env, obj_ptr);
        check_name = resolved_name;
    }
    if (check_name && vis_is_tostring_blocked(check_name)) {
        free(resolved_name);
        return NULL;
    }
    free(resolved_name);

    set_reentrant_call(1);
    jclass c = g_original_jni_table->GetObjectClass(env, (jobject)obj_ptr);
    if (!c) { set_reentrant_call(0); return NULL; }
    jmethodID mid = g_original_jni_table->GetMethodID(env, c, "toString", "()Ljava/lang/String;");
    char* res = NULL;
    if (mid) {
        jstring s = (jstring)g_original_jni_table->CallObjectMethod(env, (jobject)obj_ptr, mid);
        if (g_original_jni_table->ExceptionCheck(env)) {
            /* toString() threw — clear and return NULL to avoid propagation (Req 8.3) */
            g_original_jni_table->ExceptionClear(env);
            if (s) g_original_jni_table->DeleteLocalRef(env, s);
            g_original_jni_table->DeleteLocalRef(env, c);
            set_reentrant_call(0);
            return NULL;
        }
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
        size_t need = strlen(c) + 3; /* "" + content + NUL */
        res = (char*)malloc(need);
        if (res) {
            (void)snprintf(res, need, "\"%s\"", c);
        }
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
 * If the array is longer than VIS_MAX_ARRAY_ITEMS, a trailing "+N" element is
 * appended where N is the number of omitted items.
 *
 * Returns a heap-allocated string; caller must free().
 * Returns strdup("") for NULL/empty arrays.
 * ============================================================================ */
#define VIS_MAX_ARRAY_ITEMS 16
 

/* Grow-buffer helpers (mirrors vis_encode_typed_args.c).
 * On realloc failure, frees the previous buffer and returns NULL — callers
 * must propagate NULL to their own caller. The VEA_* macros below propagate
 * automatically: they overwrite the buffer variable, and append-after-NULL
 * remains NULL because the early-return inside vea_append takes that path. */
static char *vea_append(char *buf, size_t *len, size_t *cap, const char *src, size_t n) {
    if (buf == NULL) return NULL;
    while (*len + n + 1 > *cap) {
        size_t new_cap = (*cap < 128) ? 256 : (*cap * 2);
        char *new_buf = (char*)realloc(buf, new_cap);
        if (new_buf == NULL) { free(buf); return NULL; }
        buf = new_buf;
        *cap = new_cap;
    }
    memcpy(buf + *len, src, n);
    *len += n;
    buf[*len] = '\0';
    return buf;
}
#define VEA_LIT(b, l, c, s) vea_append(b, l, c, s, strlen(s))
#define VEA_CH(b, l, c, ch) do { if (b) { char _c = (ch); b = vea_append(b, l, c, &_c, 1); } } while(0)

/* Append app-controlled *content* (a string value, class name, or toString
 * result), escaping the wire framing bytes \x01-\x04, the deferred-render marker
 * \x1A, and the escape byte \x05 itself as the two bytes (\x05, b^0x40) (F9).
 * Without this a Java string / toString() containing those bytes would corrupt
 * the \x01-\x04 frame split (and \x1A would be misread as a placeholder).
 * Escaping is identity for normal content (one batched vea_append, same output
 * as before); the Go side reverses it with unescapeWireContent. */
static char *vea_append_escaped(char *buf, size_t *len, size_t *cap,
                                const char *src, size_t n) {
    if (buf == NULL) return NULL;
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)src[i];
        if (b == 0x01 || b == 0x02 || b == 0x03 || b == 0x04 || b == 0x05 || b == 0x1A) {
            if (run) { buf = vea_append(buf, len, cap, src + i - run, run); run = 0;
                       if (buf == NULL) return NULL; }
            char esc[2] = { (char)0x05, (char)(b ^ 0x40) };
            buf = vea_append(buf, len, cap, esc, 2);
            if (buf == NULL) return NULL;
        } else {
            run++;
        }
    }
    if (run) buf = vea_append(buf, len, cap, src + n - run, run);
    return buf;
}
#define VEA_ESC(b, l, c, s) vea_append_escaped(b, l, c, s, strlen(s))

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
                snprintf(tmp, sizeof(tmp), "%.9g", (double)elems[i]); /* float: 9 sig digits round-trips (F22) */
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
                snprintf(tmp, sizeof(tmp), "%.17g", elems[i]); /* double: 17 sig digits round-trips (F22) */
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
            /* GetObjectArrayElement can throw ArrayIndexOutOfBoundsException
             * (e.g. if the array was concurrently mutated). Clear here so the
             * rest of the loop's vis_* helpers don't all no-op via
             * vis_safe_to_call, and so the exception doesn't propagate back
             * into the target's flow when the encoder returns. */
            if (g_original_jni_table->ExceptionCheck(env)) {
                g_original_jni_table->ExceptionClear(env);
                set_reentrant_call(1);
                break;
            }
            VEA_CH(buf, &len, &cap, '\x04');
            if (!elem) {
                buf = VEA_LIT(buf, &len, &cap, "null");
            } else if (vis_is_string(env, elem)) {
                char *sv = vis_string_value_raw(env, elem);
                if (sv) { buf = vea_append_escaped(buf, &len, &cap, sv, strlen(sv)); free(sv); }
                else buf = VEA_LIT(buf, &len, &cap, "");
            } else {
                char *cn = vis_object_class_name(env, elem);
                char *ts = vis_object_tostring_safe(env, elem, cn);
                if (cn) { buf = vea_append_escaped(buf, &len, &cap, cn, strlen(cn)); free(cn); }
                if (ts) {
                    VEA_CH(buf, &len, &cap, '\x03');
                    buf = vea_append_escaped(buf, &len, &cap, ts, strlen(ts));
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
        case 'F': snprintf(tmp, sizeof(tmp), "%.9g",  (double)((const jfloat*)buf)[i]); break; /* F22 */
        case 'D': snprintf(tmp, sizeof(tmp), "%.17g", ((const jdouble*)buf)[i]);       break; /* F22 */
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

/* Advance `*ps` past one JNI type descriptor (L…;, [..., or a primitive char).
 * Returns 0 on success, -1 if the descriptor is malformed (e.g. an L without
 * a terminating semicolon). Caller bails out of arg extraction on -1 so a
 * cached bad signature can't crash the hot path. */
static int sig_advance_one(const char** ps) {
    const char* s = *ps;
    if (*s == 'L') {
        const char* end = strchr(s, ';');
        if (!end) return -1;
        *ps = end + 1;
        return 0;
    }
    if (*s == '[') {
        while (*s == '[') s++;
        if (*s == 'L') {
            const char* end = strchr(s, ';');
            if (!end) return -1;
            *ps = end + 1;
        } else if (*s == '\0') {
            return -1;
        } else {
            *ps = s + 1;
        }
        return 0;
    }
    *ps = s + 1;
    return 0;
}

int extract_va_args(const char* sig, va_list ap, uintptr_t* out, int max) {
    if (!sig || sig[0] != '(') return 0;
    const char* s = sig + 1; int n = 0;
    while (*s != ')' && *s != '\0' && n < max) {
        if (*s == 'L' || *s == '[') {
            out[n++] = (uintptr_t)va_arg(ap, jobject);
            if (sig_advance_one(&s) != 0) break;
        } else {
            char k = *s;
            if (sig_advance_one(&s) != 0) break;
            switch (k) {
                case 'Z': case 'B': case 'C': case 'S': case 'I':
                    out[n++] = (uintptr_t)va_arg(ap, jint); break;
                case 'J':
                    out[n++] = (uintptr_t)va_arg(ap, jlong); break;
                case 'F': {
                    /* float is promoted to double through va_arg; pack the
                     * 4 single-precision bytes into the low half of uintptr_t. */
                    double f = va_arg(ap, double);
                    float fl = (float)f;
                    uint32_t bits;
                    memcpy(&bits, &fl, sizeof(bits));
                    out[n++] = (uintptr_t)bits;
                    break;
                }
                case 'D': {
                    double d = va_arg(ap, double);
                    uintptr_t bits = 0;
                    memcpy(&bits, &d, sizeof(d) < sizeof(bits) ? sizeof(d) : sizeof(bits));
                    out[n++] = bits;
                    break;
                }
                default:
                    /* Unknown type char — bail rather than misalign va_list. */
                    return n;
            }
        }
    }
    return n;
}

int extract_jvalue_args(const char* sig, const jvalue* args, uintptr_t* out, int max) {
    if (!sig || sig[0] != '(' || !args) return 0;
    const char* s = sig + 1; int n = 0;
    while (*s != ')' && *s != '\0' && n < max) {
        if (*s == 'L' || *s == '[') {
            out[n] = (uintptr_t)args[n].l; n++;
            if (sig_advance_one(&s) != 0) break;
        } else {
            char k = *s;
            if (sig_advance_one(&s) != 0) break;
            switch (k) {
                case 'Z': out[n] = (uintptr_t)args[n].z; n++; break;
                case 'B': out[n] = (uintptr_t)args[n].b; n++; break;
                case 'C': out[n] = (uintptr_t)args[n].c; n++; break;
                case 'S': out[n] = (uintptr_t)args[n].s; n++; break;
                case 'I': out[n] = (uintptr_t)args[n].i; n++; break;
                case 'J': out[n] = (uintptr_t)args[n].j; n++; break;
                case 'F': {
                    uint32_t bits;
                    memcpy(&bits, &args[n].f, sizeof(bits));
                    out[n++] = (uintptr_t)bits;
                    break;
                }
                case 'D': {
                    uintptr_t bits = 0;
                    memcpy(&bits, &args[n].d, sizeof(args[n].d) < sizeof(bits) ? sizeof(args[n].d) : sizeof(bits));
                    out[n++] = bits;
                    break;
                }
                default:
                    return n;
            }
        }
    }
    return n;
}

/* ============================================================================
 * vis_encode_typed_args — encodes extracted JNI arguments into wire format.
 * Wire format per argument: sigChar \x01 primaryValue [ \x03 extraValue ] \x02
 * Uses the vea_append/VEA_LIT/VEA_CH helpers defined above.
 * ============================================================================ */
char* vis_encode_typed_args(JNIEnv *env, const char *sig, uintptr_t *extracted, int count) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    if (!sig || count <= 0) return buf;

    const char *p = sig;
    if (*p == '(') p++;

    char tmp[64];

    for (int i = 0; i < count && *p && *p != ')'; i++) {
        char kind_ch = *p;

        /* Capture element type for arrays BEFORE advancing p */
        char elem_sig = 0;
        if (kind_ch == '[') {
            const char *peek = p + 1;
            while (*peek == '[') peek++;
            elem_sig = *peek;
        }

        /* Advance sig pointer past this type */
        if (kind_ch == 'L') {
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
        } else if (kind_ch == '[') {
            p++;
            while (*p == '[') p++;
            if (*p == 'L') {
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
            } else { p++; }
        } else { p++; }

        uintptr_t val = extracted[i];

        VEA_CH(buf, &len, &cap, kind_ch);
        VEA_CH(buf, &len, &cap, '\x01');

        switch (kind_ch) {
        case 'Z': snprintf(tmp, sizeof(tmp), "%d", (int)(val & 1)); buf = VEA_LIT(buf, &len, &cap, tmp); break;
        case 'B': snprintf(tmp, sizeof(tmp), "%d", (int)(int8_t)val); buf = VEA_LIT(buf, &len, &cap, tmp); break;
        case 'C': snprintf(tmp, sizeof(tmp), "%u", (unsigned)(uint16_t)val); buf = VEA_LIT(buf, &len, &cap, tmp); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%d", (int)(int16_t)val); buf = VEA_LIT(buf, &len, &cap, tmp); break;
        case 'I': snprintf(tmp, sizeof(tmp), "%d", (int)(int32_t)val); buf = VEA_LIT(buf, &len, &cap, tmp); break;
        case 'J': snprintf(tmp, sizeof(tmp), "%lld", (long long)(int64_t)val); buf = VEA_LIT(buf, &len, &cap, tmp); break;
        case 'F': {
            float fv; uint32_t u32 = (uint32_t)val;
            memcpy(&fv, &u32, sizeof(fv));
            snprintf(tmp, sizeof(tmp), "%.9g", (double)fv); /* float round-trips at 9 sig digits (F22) */
            buf = VEA_LIT(buf, &len, &cap, tmp); break;
        }
        case 'D': {
            /* val carries the raw 64-bit double bits in a uintptr_t; the memcpy
             * is only correct when uintptr_t is at least 8 bytes (LP64).  Fail
             * the build loudly rather than silently capture 4 of 8 bytes if this
             * is ever reused on an ILP32 target (F22). */
            _Static_assert(sizeof(uintptr_t) >= sizeof(double),
                           "double bits don't fit uintptr_t (need LP64)");
            double dv; memcpy(&dv, &val, sizeof(dv));
            snprintf(tmp, sizeof(tmp), "%.17g", dv); /* double round-trips at 17 sig digits (F22) */
            buf = VEA_LIT(buf, &len, &cap, tmp); break;
        }
        case '[': {
            if (val == 0 || !env) {
                buf = VEA_LIT(buf, &len, &cap, "");
            } else {
                char el = (elem_sig == 'L' || elem_sig == '[') ? 'L' : elem_sig;
                char *items = vis_encode_array_items(env, (void *)val, el);
                size_t ilen = items ? strlen(items) : 0;
                if (items && ilen > 0) buf = vea_append(buf, &len, &cap, items, ilen);
                free(items);
                VEA_CH(buf, &len, &cap, '\x02');
                kind_ch = 0;
            }
            break;
        }
        case 'L': {
            if (val == 0 || !env) break;
            void *obj = (void *)val;
            if (vis_is_string(env, obj)) {
                /* Patch the just-appended 'L' sigChar to 's' (string). If the
                 * earlier VEA_CH calls hit a realloc failure, buf is NULL and
                 * len was not advanced — guard the in-place rewrite. */
                if (buf && len >= 2) buf[len - 2] = 's';
                char *sv = vis_string_value_raw(env, obj);
                if (sv) { buf = vea_append_escaped(buf, &len, &cap, sv, strlen(sv)); free(sv); }
            } else if (vis_is_class(env, obj)) {
                if (buf && len >= 2) buf[len - 2] = 'c';
                char *cn = vis_class_name(env, obj);
                if (cn) { buf = vea_append_escaped(buf, &len, &cap, cn, strlen(cn)); free(cn); }
            } else {
                char *cn = vis_object_class_name(env, obj);
                char *ts = vis_object_tostring_safe(env, obj, cn);
                if (cn) buf = vea_append_escaped(buf, &len, &cap, cn, strlen(cn));
                if (ts) {
                    VEA_CH(buf, &len, &cap, '\x03');
                    buf = vea_append_escaped(buf, &len, &cap, ts, strlen(ts));
                    free(ts);
                }
                free(cn);
            }
            break;
        }
        default:
            snprintf(tmp, sizeof(tmp), "0x%lx", (unsigned long)val);
            buf = VEA_LIT(buf, &len, &cap, tmp);
            break;
        }

        if (kind_ch != 0) VEA_CH(buf, &len, &cap, '\x02');
    }

    return buf;
}
