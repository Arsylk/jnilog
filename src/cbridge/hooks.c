#include "hook_internal.h"
#include "event_pipe.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Lifecycle invariant for `g_original_jni_table` access in this file.
 *
 * `install_jni_hooks()` populates `g_saved_jni_table` and sets
 * `g_original_jni_table = &g_saved_jni_table` BEFORE any hooked entry replaces
 * a slot in the live JNI table. Once the live table dispatches into a
 * `hooked_*` function, `g_original_jni_table` is therefore guaranteed
 * non-NULL. `restore_jni_hooks()` swaps the live table back to the original
 * before the library unloads, so the hooked symbols are unreachable from
 * Java after restore.
 *
 * Lookup hooks (FindClass / Get*MethodID / Get*FieldID / RegisterNatives) do
 * still guard `g_original_jni_table->X` defensively because that path also
 * needs `log_missing_original()` diagnostics when an exotic ART build leaves
 * a slot NULL. The remaining hooks intentionally call through the slot
 * unconditionally — on any well-formed ART build every slot is populated, and
 * if a particular slot is genuinely NULL the resulting SIGSEGV pinpoints the
 * missing function in the tombstone rather than silently producing wrong
 * behaviour. This style inconsistency is intentional; new hooks should follow
 * the lookup-hook pattern when adding a slot that callers may legitimately
 * miss.
 * ============================================================================ */

#define PRIMITIVE_SIGCHAR_Boolean 'Z'
#define PRIMITIVE_SIGCHAR_Byte    'B'
#define PRIMITIVE_SIGCHAR_Char    'C'
#define PRIMITIVE_SIGCHAR_Short   'S'
#define PRIMITIVE_SIGCHAR_Int     'I'
#define PRIMITIVE_SIGCHAR_Long    'J'
#define PRIMITIVE_SIGCHAR_Float   'F'
#define PRIMITIVE_SIGCHAR_Double  'D'
#define PRIMITIVE_SIGCHAR(Name) PRIMITIVE_SIGCHAR_##Name

static jni_table_t  g_saved_jni_table;
static jni_table_t  g_hooked_jni_table;
static jni_table_t *g_live_jni_table    = NULL;
static int          g_jni_hooks_installed = 0;
static pthread_mutex_t g_install_lock   = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * Typed logging helpers for utility hooks (non-method-call hooks).
 *
 * These replace the old string-formatting LOG_SIMPLE_CALL / LOG_SIMPLE_RET /
 * LOG_OBJECT_CALL macros with typed equivalents.  Each helper calls
 * log_jni_call / log_jni_return with a wire_kind_t int so Go's
 * formatJNIValue() picks the right color with no heuristics.
 * ============================================================================ */

/*
 * _log_void_call — no meaningful receiver or args (GetVersion, etc.).
 */
#define LOG_VOID_CALL(Slot, Name, Caller) \
    char _cs[192]; address_of_r(Caller, _cs, sizeof(_cs)); \
    log_jni_call(JNI_SLOT(Slot), Name, \
                 WIRE_KIND_NULL, "", "", "", Name, "", 0, _cs)

/*
 * _log_ptr_call — single pointer argument (method/field/object IDs).
 * 'p' encoded in encoded_args as "p\x01hex\x02" so Go decodes as KindPointer.
 */
static inline void _log_ptr_arg_call(int slot, const char *name,
                                      uintptr_t ptr, const char *caller_str) {
    char enc[64];
    snprintf(enc, sizeof(enc), "p\x01" "0x%lx\x02", (unsigned long)ptr);
    log_jni_call(slot, name, WIRE_KIND_NULL, "", "", "", name, enc, 0, caller_str);
}
#define LOG_PTR_CALL(Slot, Name, Ptr, Caller) do { \
    char _cs[192]; address_of_r(Caller, _cs, sizeof(_cs)); \
    _log_ptr_arg_call(JNI_SLOT(Slot), Name, (uintptr_t)(Ptr), _cs); \
} while(0)

/*
 * _log_obj_arg_call — emit a one-jobject-arg call event with vis_* deferred.
 * Writes a "\x1A<slot>" placeholder instead of the rendered chunk; the Go
 * consumer renders via vis_* on its attached JNIEnv* and substitutes.  Saves
 * 3-5 JNI calls per logged event in exchange for one NewGlobalRef.
 */
static inline void _log_obj_arg_call(int slot, const char *name,
                                      JNIEnv *env, void *obj,
                                      const char *caller_str) {
    char enc[64];
    if (!obj) {
        snprintf(enc, sizeof(enc), "p\x01" "null\x02");
    } else {
        int n = event_pipe_defer_render_push(env, obj);
        if (n < 0) {
            snprintf(enc, sizeof(enc), "p\x01" "null\x02");
        } else {
            /* Deferred-render placeholder: marker "\x1A" + slot byte (slot+1).
             * The +1 keeps the byte non-zero so it survives this NUL-terminated
             * encoder string; the Go substituter decodes slot = byte-1 (F8). */
            snprintf(enc, sizeof(enc), "\x1A%c", n + 1);
        }
    }
    log_jni_call(slot, name, WIRE_KIND_NULL, "", "", "", name, enc, 0, caller_str);
}
#define LOG_OBJ_CALL(Env, Slot, Name, Obj, Caller) do { \
    char _cs[192]; address_of_r(Caller, _cs, sizeof(_cs)); \
    set_reentrant_call(1); \
    _log_obj_arg_call(JNI_SLOT(Slot), Name, Env, (void*)(Obj), _cs); \
    set_reentrant_call(0); \
} while(0)

/* Two-object argument call (IsSameObject, etc.).  Uses the same deferred-
 * render placeholder scheme as _log_obj_arg_call: each object becomes a
 * NewGlobalRef + "\x1A<n>" marker; the Go consumer renders + substitutes. */
static inline void _log_obj2_arg_call(int slot, const char *name,
                                       JNIEnv *env, void *obj1, void *obj2,
                                       const char *caller_str) {
    char enc[64];
    int pos = 0;
    void *objs[2] = {obj1, obj2};
    for (int i = 0; i < 2; i++) {
        void *o = objs[i];
        if (!o) {
            int n = snprintf(enc + pos, sizeof(enc) - pos, "p\x01" "null\x02");
            if (n > 0) pos += n;
        } else {
            int slotn = event_pipe_defer_render_push(env, o);
            if (slotn < 0) {
                int n = snprintf(enc + pos, sizeof(enc) - pos, "p\x01" "null\x02");
                if (n > 0) pos += n;
            } else {
                if (pos + 2 < (int)sizeof(enc)) {
                    enc[pos++] = '\x1A';
                    enc[pos++] = (char)(slotn + 1);   /* slot+1, NUL-safe (F8) */
                    enc[pos] = '\0';
                }
            }
        }
    }
    log_jni_call(slot, name, WIRE_KIND_NULL, "", "", "", name, enc, 0, caller_str);
}
#define LOG_OBJ2_CALL(Env, Slot, Name, Obj1, Obj2, Caller) do { \
    char _cs[192]; address_of_r(Caller, _cs, sizeof(_cs)); \
    set_reentrant_call(1); \
    _log_obj2_arg_call(JNI_SLOT(Slot), Name, Env, (void*)(Obj1), (void*)(Obj2), _cs); \
    set_reentrant_call(0); \
} while(0)

/* ── Return helpers ─────────────────────────────────────────────────────── */
#define LOG_VOID_RET(Slot, Name) \
    log_jni_return(JNI_SLOT(Slot), Name, WIRE_KIND_VOID, 0, "", "")

#define LOG_INT_RET(Slot, Name, Val) \
    log_jni_return(JNI_SLOT(Slot), Name, WIRE_KIND_INT, \
                   (uintptr_t)(uint32_t)(int32_t)(Val), "", "")

#define LOG_LONG_RET(Slot, Name, Val) \
    log_jni_return(JNI_SLOT(Slot), Name, WIRE_KIND_LONG, \
                   (uintptr_t)(uint64_t)(int64_t)(Val), "", "")

#define LOG_BOOL_RET(Slot, Name, Val) \
    log_jni_return(JNI_SLOT(Slot), Name, WIRE_KIND_BOOLEAN, \
                   (uintptr_t)((Val) != 0), "", "")

#define LOG_PTR_RET(Slot, Name, Val) \
    log_jni_return(JNI_SLOT(Slot), Name, WIRE_KIND_POINTER, \
                   (uintptr_t)(Val), "", "")

/* Object return — defers vis_* class/toString lookup to the Go-side consumer
 * thread.  We NewGlobalRef the object so the consumer (running on its own
 * AttachCurrentThread'd JNIEnv*) can call vis_* against it without touching
 * the hook thread's JNI dispatch path — keeping PairIP-measured JNI latency
 * at its native baseline.  Consumer issues the matching DeleteGlobalRef. */
static inline void _log_obj_ret(int slot, const char *name,
                                 JNIEnv *env, void *obj) {
    if (!obj) {
        log_jni_return(slot, name, WIRE_KIND_NULL, 0, "", "");
        return;
    }
    /* Defer object rendering only when a consumer is attached to render + free
     * the gref; otherwise emit a void-shaped return (no gref minted) so we never
     * leak a ref the consumer will never drain (F1). */
    if (!event_pipe_consumer_ready()) {
        log_jni_return(slot, name, WIRE_KIND_NULL, 0, "", "");
        return;
    }
    /* NewGlobalRef: one JNI call, much cheaper than 4-5 vis_* JNI calls. */
    void *gref = (void*)(*env)->NewGlobalRef(env, obj);
    if (!gref) {
        /* OOM in the global ref table — fall back to a void return so the
         * call frame still gets emitted, just without the return payload. */
        log_jni_return(slot, name, WIRE_KIND_NULL, 0, "", "");
        return;
    }
    /* Pair with the matching call_id via the same TLS slot log_jni_return
     * normally uses — Go side consumes call_id from event header. */
    extern __thread uint64_t tls_last_call_id;
    extern int event_pipe_emit_obj_return(uint64_t, int32_t, uintptr_t, const char*);
    uint64_t cid = tls_last_call_id;
    tls_last_call_id = 0;
    if (event_pipe_emit_obj_return(cid, slot, (uintptr_t)gref, name ? name : "") != 0) {
        /* Datagram dropped — the consumer will never see (or free) this gref,
         * so release it here. */
        (*env)->DeleteGlobalRef(env, gref);
    }
}
#define LOG_OBJ_RET(Env, Slot, Name, Obj) do { \
    set_reentrant_call(1); \
    _log_obj_ret(JNI_SLOT(Slot), Name, Env, (void*)(Obj)); \
    set_reentrant_call(0); \
} while(0)

/* String return — content known directly */
#define LOG_STR_RET(Slot, Name, Content) \
    log_jni_return(JNI_SLOT(Slot), Name, WIRE_KIND_STRING, 0, \
                   (Content) ? (Content) : "", "")

/* ============================================================================
 * Lookup hooks (FindClass, GetMethodID, GetFieldID, etc.)
 * These emit lookup events, not call/return events.
 * ============================================================================ */

static jclass hooked_FindClass(JNIEnv *env, const char *name) {
  void *caller = __builtin_return_address(0);
  jclass result = NULL;
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->FindClass)
    result = g_original_jni_table->FindClass(env, name);
  else log_missing_original("FindClass", should_log);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_lookup("FindClass", name, "", (void*)result, name, cs);
    set_reentrant_call(0);
  }
  return result;
}

static jmethodID hooked_GetMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
  void *caller = __builtin_return_address(0);
  jmethodID result = NULL;
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->GetMethodID)
    result = g_original_jni_table->GetMethodID(env, clazz, name, sig);
  else log_missing_original("GetMethodID", should_log);
  if (result && sig) {
    char *cn = vis_class_name(env, clazz);
    cache_method_signature(result, name, sig, cn); free(cn);
  }
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_lookup_deferred(env, "GetMethodID", name, sig, clazz, cs);
    set_reentrant_call(0);
  }
  return result;
}

static jmethodID hooked_GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
  void *caller = __builtin_return_address(0);
  jmethodID result = NULL;
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->GetStaticMethodID)
    result = g_original_jni_table->GetStaticMethodID(env, clazz, name, sig);
  else log_missing_original("GetStaticMethodID", should_log);
  if (result && sig) {
    char *cn = vis_class_name(env, clazz);
    cache_method_signature(result, name, sig, cn); free(cn);
  }
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_lookup_deferred(env, "GetStaticMethodID", name, sig, clazz, cs);
    set_reentrant_call(0);
  }
  return result;
}

static jfieldID hooked_GetFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
  void *caller = __builtin_return_address(0);
  jfieldID result = NULL;
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->GetFieldID)
    result = g_original_jni_table->GetFieldID(env, clazz, name, sig);
  else log_missing_original("GetFieldID", should_log);
  if (result && sig) {
    char *cn = vis_class_name(env, clazz);
    cache_field_signature(result, name, sig, cn); free(cn);
  }
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_lookup_deferred(env, "GetFieldID", name, sig, clazz, cs);
    set_reentrant_call(0);
  }
  return result;
}

static jfieldID hooked_GetStaticFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
  void *caller = __builtin_return_address(0);
  jfieldID result = NULL;
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->GetStaticFieldID)
    result = g_original_jni_table->GetStaticFieldID(env, clazz, name, sig);
  else log_missing_original("GetStaticFieldID", should_log);
  if (result && sig) {
    char *cn = vis_class_name(env, clazz);
    cache_field_signature(result, name, sig, cn); free(cn);
  }
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_lookup_deferred(env, "GetStaticFieldID", name, sig, clazz, cs);
    set_reentrant_call(0);
  }
  return result;
}

static jobject hooked_NewGlobalRef(JNIEnv *env, jobject obj) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->NewGlobalRef(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, NewGlobalRef, "NewGlobalRef", obj, caller);
    LOG_OBJ_RET(env, NewGlobalRef, "NewGlobalRef", res);
  }
  return res;
}

static jint hooked_RegisterNatives(JNIEnv *env, jclass clazz,
                                    const JNINativeMethod *methods, jint nMethods) {
  void *caller = __builtin_return_address(0);
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  jint result = JNI_ERR;
  if (g_original_jni_table && g_original_jni_table->RegisterNatives)
    result = g_original_jni_table->RegisterNatives(env, clazz, methods, nMethods);
  else log_missing_original("RegisterNatives", should_log);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char *cn = vis_class_name(env, clazz);
    log_jni_register_natives(clazz, cn, methods, nMethods, cs);
    free(cn); set_reentrant_call(0);
  }
  return result;
}

/* ============================================================================
 * Array hooks — typed returns
 * ============================================================================ */

#define DEFINE_NEW_ARRAY_HOOKS(Name, CType, RetKind, UnionF)                    \
  CType##Array hooked_New##Name##Array(JNIEnv *env, jsize length) {             \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->New##Name##Array)  \
             ? g_original_jni_table->New##Name##Array(env, length) : (CType##Array)0; \
    }                                                                           \
    int should_log = should_log_from_caller(env, caller);                       \
    CType##Array result = (CType##Array)0;                                      \
    if (g_original_jni_table && g_original_jni_table->New##Name##Array)        \
      result = g_original_jni_table->New##Name##Array(env, length);             \
    if (should_log) {                                                           \
      set_reentrant_call(1);                                                    \
      char cs[192]; address_of_r(caller, cs, sizeof(cs));                      \
      char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02", (int)length);    \
      log_jni_call(JNI_SLOT(New##Name##Array), "New" #Name "Array",            \
                   WIRE_KIND_NULL, "", "", "", "New" #Name "Array", enc, 0, cs);\
      char *items = vis_encode_array_items(env, result, PRIMITIVE_SIGCHAR(Name)); \
      log_jni_return(JNI_SLOT(New##Name##Array), "New" #Name "Array",          \
                     WIRE_KIND_ARRAY, (uintptr_t)result, items ? items : "", ""); \
      free(items); set_reentrant_call(0);                                         \
    }                                                                           \
    return result;                                                              \
  }

#define INSTALL_NEW_ARRAY_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.New##Name##Array = hooked_New##Name##Array;

#define DEFINE_GET_ARRAY_ELEMENTS_HOOKS(Name, CType, RetKind, UnionF)           \
  CType* hooked_Get##Name##ArrayElements(JNIEnv *env, CType##Array array,      \
                                          jboolean *isCopy) {                  \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      return (g_original_jni_table && g_original_jni_table->Get##Name##ArrayElements) \
             ? g_original_jni_table->Get##Name##ArrayElements(env, array, isCopy) : NULL; \
    }                                                                           \
    int should_log = should_log_from_caller(env, caller);                       \
    CType *result = NULL;                                                       \
    if (g_original_jni_table && g_original_jni_table->Get##Name##ArrayElements)\
      result = g_original_jni_table->Get##Name##ArrayElements(env, array, isCopy); \
    if (should_log) {                                                           \
      set_reentrant_call(1);                                                    \
      char cs[192]; address_of_r(caller, cs, sizeof(cs));                      \
      char *_items = vis_encode_array_items(env, array, PRIMITIVE_SIGCHAR(Name)); \
      size_t _ilen = _items ? strlen(_items) : 0;                               \
      char *enc = (char*)malloc(_ilen + 8);                                     \
      if (enc) {                                                                \
        enc[0] = '['; enc[1] = '\x01';                                          \
        if (_items && _ilen > 0) memcpy(enc + 2, _items, _ilen);                \
        enc[2 + _ilen] = '\x02'; enc[3 + _ilen] = '\0';                         \
        log_jni_call(JNI_SLOT(Get##Name##ArrayElements), "Get" #Name "ArrayElements", \
                     WIRE_KIND_NULL, "", "", "", "Get" #Name "ArrayElements", enc, 0, cs); \
        free(enc);                                                              \
        log_jni_return(JNI_SLOT(Get##Name##ArrayElements), "Get" #Name "ArrayElements", \
                       WIRE_KIND_POINTER, (uintptr_t)result, "", "");           \
      }                                                                          \
      free(_items);                                                              \
      set_reentrant_call(0);                                                    \
    }                                                                           \
    return result;                                                              \
  }

#define INSTALL_GET_ARRAY_ELEMENTS_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.Get##Name##ArrayElements = hooked_Get##Name##ArrayElements;

/* ============================================================================
 * Release*ArrayElements — paired with Get*ArrayElements, closes the lifecycle.
 * Log mode (0=copy+free, JNI_COMMIT=1, JNI_ABORT=2) for leak detection.
 * ============================================================================ */
#define DEFINE_RELEASE_ARRAY_ELEMENTS_HOOKS(Name, CType, ...)                  \
  void hooked_Release##Name##ArrayElements(JNIEnv *env, CType##Array array,    \
                                           CType *elems, jint mode) {          \
    void *caller = __builtin_return_address(0);                                 \
    if (is_reentrant_call()) {                                                  \
      if (g_original_jni_table && g_original_jni_table->Release##Name##ArrayElements) \
        g_original_jni_table->Release##Name##ArrayElements(env, array, elems, mode); \
      return;                                                                   \
    }                                                                           \
    int should_log = should_log_from_caller(env, caller);                       \
    if (g_original_jni_table && g_original_jni_table->Release##Name##ArrayElements) \
      g_original_jni_table->Release##Name##ArrayElements(env, array, elems, mode); \
    if (should_log) {                                                           \
      set_reentrant_call(1);                                                    \
      char cs[192]; address_of_r(caller, cs, sizeof(cs));                      \
      char enc[64];                                                             \
      const char *modeStr = (mode == JNI_COMMIT) ? "commit"                    \
                          : (mode == JNI_ABORT)  ? "abort"                     \
                          :                       "free";                      \
      snprintf(enc, sizeof(enc), "I\x01%d\x02s\x01%s\x02", (int)mode, modeStr);\
      log_jni_call(JNI_SLOT(Release##Name##ArrayElements),                     \
                   "Release" #Name "ArrayElements",                             \
                   WIRE_KIND_NULL, "", "", "",                                  \
                   "Release" #Name "ArrayElements", enc, 0, cs);                \
      LOG_VOID_RET(Release##Name##ArrayElements, "Release" #Name "ArrayElements"); \
      set_reentrant_call(0);                                                    \
    }                                                                           \
  }

#define INSTALL_RELEASE_ARRAY_ELEMENTS_HOOKS(Name, CType, ...) \
  g_hooked_jni_table.Release##Name##ArrayElements = hooked_Release##Name##ArrayElements;

JNI_PRIMITIVE_ARRAY_TYPES(DEFINE_NEW_ARRAY_HOOKS)
JNI_PRIMITIVE_ARRAY_TYPES(DEFINE_GET_ARRAY_ELEMENTS_HOOKS)
JNI_PRIMITIVE_ARRAY_TYPES(DEFINE_RELEASE_ARRAY_ELEMENTS_HOOKS)

static jobjectArray hooked_NewObjectArray(JNIEnv *env, jsize length,
                                           jclass elementClass, jobject initialElement) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    return (g_original_jni_table && g_original_jni_table->NewObjectArray)
           ? g_original_jni_table->NewObjectArray(env, length, elementClass, initialElement) : NULL;
  }
  int should_log = should_log_from_caller(env, caller);
  jobjectArray result = NULL;
  if (g_original_jni_table && g_original_jni_table->NewObjectArray)
    result = g_original_jni_table->NewObjectArray(env, length, elementClass, initialElement);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02", (int)length);
    log_jni_call(JNI_SLOT(NewObjectArray), "NewObjectArray",
                 WIRE_KIND_NULL, "", "", "", "NewObjectArray", enc, 0, cs);
    char *arr = vis_encode_array_items(env, result, 'L');
    log_jni_return(JNI_SLOT(NewObjectArray), "NewObjectArray",
                   WIRE_KIND_ARRAY, (uintptr_t)result, arr ? arr : "", "");
    free(arr); set_reentrant_call(0);
  }
  return result;
}

static jobject hooked_GetObjectArrayElement(JNIEnv *env, jobjectArray array, jsize index) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    return (g_original_jni_table && g_original_jni_table->GetObjectArrayElement)
           ? g_original_jni_table->GetObjectArrayElement(env, array, index) : NULL;
  }
  int should_log = should_log_from_caller(env, caller);
  jobject result = NULL;
  if (g_original_jni_table && g_original_jni_table->GetObjectArrayElement)
    result = g_original_jni_table->GetObjectArrayElement(env, array, index);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char *arr = vis_encode_array_items(env, array, 'L');
    /* Encode array as '[' arg + index as 'I' arg */
    size_t _alen = arr ? strlen(arr) : 0;
    char *enc = (char*)malloc(_alen + 32);
    if (enc) {
      enc[0] = '['; enc[1] = '\x01';
      if (arr && _alen > 0) memcpy(enc + 2, arr, _alen);
      int _pos = 2 + (int)_alen;
      enc[_pos++] = '\x02';
      _pos += snprintf(enc + _pos, 24, "I\x01%d\x02", (int)index);
      enc[_pos] = '\0';
      log_jni_call(JNI_SLOT(GetObjectArrayElement), "GetObjectArrayElement",
                   WIRE_KIND_NULL, "", "", "", "GetObjectArrayElement", enc, 0, cs);
      free(enc);
      _log_obj_ret(JNI_SLOT(GetObjectArrayElement), "GetObjectArrayElement", env, result);
    }
    free(arr);
    set_reentrant_call(0);
  }
  return result;
}

/* ============================================================================
 * SetObjectArrayElement — Write a reference into an object array.
 * Mirrors GetObjectArrayElement: logs array repr, index, and the value being set.
 * ============================================================================ */
static void hooked_SetObjectArrayElement(JNIEnv *env, jobjectArray array,
                                          jsize index, jobject value) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    if (g_original_jni_table && g_original_jni_table->SetObjectArrayElement)
      g_original_jni_table->SetObjectArrayElement(env, array, index, value);
    return;
  }
  int should_log = should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->SetObjectArrayElement)
    g_original_jni_table->SetObjectArrayElement(env, array, index, value);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char *arr = vis_encode_array_items(env, array, 'L');
    /* Encode array as '[' arg, index as 'I' arg, then value.
     * Worst-case writes: '[\x01' + arr + '\x02' + "I\x01<int>\x02" (~15) +
     * "L\x01<200>\x03<200>\x02" (404) + NUL = arr + ~424. Allocate with a
     * generous safety margin and clamp every snprintf return against the
     * actual remaining capacity (snprintf returns would-have-written length,
     * not actual; naive `_pos += snprintf(...)` over-counts on truncation). */
    size_t _alen = arr ? strlen(arr) : 0;
    size_t enc_cap = _alen + 512;
    char *enc = (char*)malloc(enc_cap);
    if (!enc) { free(arr); set_reentrant_call(0); return; }
    enc[0] = '['; enc[1] = '\x01';
    if (arr && _alen > 0) memcpy(enc + 2, arr, _alen);
    int _pos = 2 + (int)_alen;
    enc[_pos++] = '\x02';
    /* Helper to append a snprintf result while clamping by remaining room. */
#define ENC_APPEND(fmt, ...) do { \
    int _room = (int)enc_cap - 1 - _pos; \
    if (_room <= 0) break; \
    int _w = snprintf(enc + _pos, (size_t)_room + 1, fmt, ##__VA_ARGS__); \
    if (_w < 0) break; \
    if (_w > _room) _w = _room; \
    _pos += _w; \
} while (0)
    ENC_APPEND("I\x01%d\x02", (int)index);
    /* Encode the value being set — defer vis_* to the Go consumer via
     * NewGlobalRef + "\x1A<n>" placeholder in the encoded_args. */
    if (!value) {
      ENC_APPEND("p\x01" "null\x02");
    } else {
      int slotn = event_pipe_defer_render_push(env, value);
      if (slotn < 0) {
        ENC_APPEND("p\x01" "null\x02");
      } else {
        ENC_APPEND("\x1A%c", slotn + 1);   /* slot+1, NUL-safe (F8) */
      }
    }
#undef ENC_APPEND
    enc[_pos] = '\0';
    free(arr);
    log_jni_call(JNI_SLOT(SetObjectArrayElement), "SetObjectArrayElement",
                 WIRE_KIND_NULL, "", "", "", "SetObjectArrayElement", enc, 0, cs);
    free(enc);
    LOG_VOID_RET(SetObjectArrayElement, "SetObjectArrayElement");
    set_reentrant_call(0);
  }
}

static jsize hooked_GetArrayLength(JNIEnv *env, jarray array) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    return (g_original_jni_table && g_original_jni_table->GetArrayLength)
           ? g_original_jni_table->GetArrayLength(env, array) : 0;
  }
  int should_log = should_log_from_caller(env, caller);
  jsize result = 0;
  if (g_original_jni_table && g_original_jni_table->GetArrayLength)
    result = g_original_jni_table->GetArrayLength(env, array);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char *_gcn = vis_object_class_name(env, array);
    char _gsig = 'L';
    if (_gcn && _gcn[0] == '[' && _gcn[1] != '\0' && _gcn[1] != 'L' && _gcn[1] != '[')
      _gsig = _gcn[1];
    free(_gcn);
    char *_garr = vis_encode_array_items(env, array, _gsig);
    size_t _glen = _garr ? strlen(_garr) : 0;
    char *enc = (char*)malloc(_glen + 8);
    if (enc) {
      enc[0] = '['; enc[1] = '\x01';
      if (_garr && _glen > 0) memcpy(enc + 2, _garr, _glen);
      enc[2 + _glen] = '\x02'; enc[3 + _glen] = '\0';
      log_jni_call(JNI_SLOT(GetArrayLength), "GetArrayLength",
                   WIRE_KIND_NULL, "", "", "", "GetArrayLength", enc, 0, cs);
      free(enc);
      LOG_INT_RET(GetArrayLength, "GetArrayLength", result);
    }
    free(_garr);
    set_reentrant_call(0);
  }
  return result;
}

/* Critical array/string sections */
void* hooked_GetPrimitiveArrayCritical(JNIEnv *env, jarray array, jboolean *isCopy) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    return (g_original_jni_table && g_original_jni_table->GetPrimitiveArrayCritical)
           ? g_original_jni_table->GetPrimitiveArrayCritical(env, array, isCopy) : NULL;
  }
  int should_log = should_log_from_caller(env, caller);
  void *result = NULL;
  if (g_original_jni_table && g_original_jni_table->GetPrimitiveArrayCritical)
    result = g_original_jni_table->GetPrimitiveArrayCritical(env, array, isCopy);
  set_jni_critical(is_jni_critical() + 1);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_call(JNI_SLOT(GetPrimitiveArrayCritical), "GetPrimitiveArrayCritical",
                 WIRE_KIND_NULL, "", "", "", "GetPrimitiveArrayCritical", "", 0, cs);
    LOG_PTR_RET(GetPrimitiveArrayCritical, "GetPrimitiveArrayCritical", result);
    set_reentrant_call(0);
  }
  return result;
}

void hooked_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray array, void *carray, jint mode) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    if (g_original_jni_table && g_original_jni_table->ReleasePrimitiveArrayCritical)
      g_original_jni_table->ReleasePrimitiveArrayCritical(env, array, carray, mode);
    return;
  }
  int should_log = should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->ReleasePrimitiveArrayCritical)
    g_original_jni_table->ReleasePrimitiveArrayCritical(env, array, carray, mode);
  int crit = is_jni_critical(); if (crit > 0) set_jni_critical(crit - 1);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "p\x01" "0x%lx\x02I\x01%d\x02",
                           (unsigned long)carray, (int)mode);
    log_jni_call(JNI_SLOT(ReleasePrimitiveArrayCritical), "ReleasePrimitiveArrayCritical",
                 WIRE_KIND_NULL, "", "", "", "ReleasePrimitiveArrayCritical", enc, 0, cs);
    LOG_VOID_RET(ReleasePrimitiveArrayCritical, "ReleasePrimitiveArrayCritical");
    set_reentrant_call(0);
  }
}

const jchar* hooked_GetStringCritical(JNIEnv *env, jstring string, jboolean *isCopy) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    return (g_original_jni_table && g_original_jni_table->GetStringCritical)
           ? g_original_jni_table->GetStringCritical(env, string, isCopy) : NULL;
  }
  int should_log = should_log_from_caller(env, caller);
  const jchar *result = NULL;
  if (g_original_jni_table && g_original_jni_table->GetStringCritical)
    result = g_original_jni_table->GetStringCritical(env, string, isCopy);
  set_jni_critical(is_jni_critical() + 1);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    log_jni_call(JNI_SLOT(GetStringCritical), "GetStringCritical",
                 WIRE_KIND_NULL, "", "", "", "GetStringCritical", "", 0, cs);
    LOG_PTR_RET(GetStringCritical, "GetStringCritical", result);
    set_reentrant_call(0);
  }
  return result;
}

void hooked_ReleaseStringCritical(JNIEnv *env, jstring string, const jchar *cstring) {
  void *caller = __builtin_return_address(0);
  if (is_reentrant_call()) {
    if (g_original_jni_table && g_original_jni_table->ReleaseStringCritical)
      g_original_jni_table->ReleaseStringCritical(env, string, cstring);
    return;
  }
  int should_log = should_log_from_caller(env, caller);
  if (g_original_jni_table && g_original_jni_table->ReleaseStringCritical)
    g_original_jni_table->ReleaseStringCritical(env, string, cstring);
  int crit = is_jni_critical(); if (crit > 0) set_jni_critical(crit - 1);
  if (should_log) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "p\x01" "0x%lx\x02", (unsigned long)cstring);
    log_jni_call(JNI_SLOT(ReleaseStringCritical), "ReleaseStringCritical",
                 WIRE_KIND_NULL, "", "", "", "ReleaseStringCritical", enc, 0, cs);
    LOG_VOID_RET(ReleaseStringCritical, "ReleaseStringCritical");
    set_reentrant_call(0);
  }
}

/* ============================================================================
 * Reference Operations
 * ============================================================================ */

void hooked_DeleteGlobalRef(JNIEnv *env, jobject globalRef) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, DeleteGlobalRef, "DeleteGlobalRef", globalRef, caller);
    LOG_VOID_RET(DeleteGlobalRef, "DeleteGlobalRef");
  }
  g_original_jni_table->DeleteGlobalRef(env, globalRef);
}

void hooked_DeleteLocalRef(JNIEnv *env, jobject localRef) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, DeleteLocalRef, "DeleteLocalRef", localRef, caller);
    LOG_VOID_RET(DeleteLocalRef, "DeleteLocalRef");
  }
  g_original_jni_table->DeleteLocalRef(env, localRef);
}

jobject hooked_NewLocalRef(JNIEnv *env, jobject ref) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->NewLocalRef(env, ref);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, NewLocalRef, "NewLocalRef", ref, caller);
    LOG_OBJ_RET(env, NewLocalRef, "NewLocalRef", res);
  }
  return res;
}

jweak hooked_NewWeakGlobalRef(JNIEnv *env, jobject obj) {
  void *caller = __builtin_return_address(0);
  jweak res = g_original_jni_table->NewWeakGlobalRef(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, NewWeakGlobalRef, "NewWeakGlobalRef", obj, caller);
    LOG_OBJ_RET(env, NewWeakGlobalRef, "NewWeakGlobalRef", res);
  }
  return res;
}

void hooked_DeleteWeakGlobalRef(JNIEnv *env, jweak ref) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, DeleteWeakGlobalRef, "DeleteWeakGlobalRef", ref, caller);
    LOG_VOID_RET(DeleteWeakGlobalRef, "DeleteWeakGlobalRef");
  }
  g_original_jni_table->DeleteWeakGlobalRef(env, ref);
}

jboolean hooked_IsSameObject(JNIEnv *env, jobject ref1, jobject ref2) {
  void *caller = __builtin_return_address(0);
  jboolean res = g_original_jni_table->IsSameObject(env, ref1, ref2);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ2_CALL(env, IsSameObject, "IsSameObject", ref1, ref2, caller);
    LOG_BOOL_RET(IsSameObject, "IsSameObject", res);
  }
  return res;
}

jint hooked_PushLocalFrame(JNIEnv *env, jint capacity) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->PushLocalFrame(env, capacity);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02", (int)capacity);
    log_jni_call(JNI_SLOT(PushLocalFrame), "PushLocalFrame",
                 WIRE_KIND_NULL, "", "", "", "PushLocalFrame", enc, 0, cs);
    LOG_INT_RET(PushLocalFrame, "PushLocalFrame", res);
  }
  return res;
}

jobject hooked_PopLocalFrame(JNIEnv *env, jobject result) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->PopLocalFrame(env, result);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, PopLocalFrame, "PopLocalFrame", result, caller);
    LOG_OBJ_RET(env, PopLocalFrame, "PopLocalFrame", res);
  }
  return res;
}

jint hooked_EnsureLocalCapacity(JNIEnv *env, jint capacity) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->EnsureLocalCapacity(env, capacity);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02", (int)capacity);
    log_jni_call(JNI_SLOT(EnsureLocalCapacity), "EnsureLocalCapacity",
                 WIRE_KIND_NULL, "", "", "", "EnsureLocalCapacity", enc, 0, cs);
    LOG_INT_RET(EnsureLocalCapacity, "EnsureLocalCapacity", res);
  }
  return res;
}

/* ============================================================================
 * String Operations
 * ============================================================================ */

jstring hooked_NewString(JNIEnv *env, const jchar *unicode, jsize len) {
  void *caller = __builtin_return_address(0);
  jstring res = g_original_jni_table->NewString(env, unicode, len);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02", (int)len);
    log_jni_call(JNI_SLOT(NewString), "NewString",
                 WIRE_KIND_NULL, "", "", "", "NewString", enc, 0, cs);
    char *sv = vis_string_value_raw(env, res);
    log_jni_return(JNI_SLOT(NewString), "NewString",
                   WIRE_KIND_STRING, 0, sv ? sv : "", "");
    free(sv); set_reentrant_call(0);
  }
  return res;
}

jsize hooked_GetStringLength(JNIEnv *env, jstring str) {
  void *caller = __builtin_return_address(0);
  jsize res = g_original_jni_table->GetStringLength(env, str);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(GetStringLength, "GetStringLength", str, caller);
    LOG_INT_RET(GetStringLength, "GetStringLength", res);
  }
  return res;
}

const jchar* hooked_GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  void *caller = __builtin_return_address(0);
  const jchar *res = g_original_jni_table->GetStringChars(env, str, isCopy);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(GetStringChars, "GetStringChars", str, caller);
    LOG_PTR_RET(GetStringChars, "GetStringChars", res);
  }
  return res;
}

void hooked_ReleaseStringChars(JNIEnv *env, jstring str, const jchar *chars) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(ReleaseStringChars, "ReleaseStringChars", str, caller);
    LOG_VOID_RET(ReleaseStringChars, "ReleaseStringChars");
  }
  g_original_jni_table->ReleaseStringChars(env, str, chars);
}

jstring hooked_NewStringUTF(JNIEnv *env, const char *utf) {
  void *caller = __builtin_return_address(0);
  jstring res = g_original_jni_table->NewStringUTF(env, utf);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    /* Encode the UTF string content as KindString arg */
    char enc[512];
    int ulen = utf ? (int)strlen(utf) : 0;
    if (ulen > 400) ulen = 400;
    enc[0] = 's'; enc[1] = '\x01';
    if (utf) memcpy(enc+2, utf, ulen);
    enc[2+ulen] = '\x02'; enc[3+ulen] = '\0';
    log_jni_call(JNI_SLOT(NewStringUTF), "NewStringUTF",
                 WIRE_KIND_NULL, "", "", "", "NewStringUTF", enc, 0, cs);
    log_jni_return(JNI_SLOT(NewStringUTF), "NewStringUTF",
                   WIRE_KIND_POINTER, (uintptr_t)res, "", "");
    set_reentrant_call(0);
  }
  return res;
}

jsize hooked_GetStringUTFLength(JNIEnv *env, jstring str) {
  void *caller = __builtin_return_address(0);
  jsize res = g_original_jni_table->GetStringUTFLength(env, str);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, GetStringUTFLength, "GetStringUTFLength", str, caller);
    LOG_INT_RET(GetStringUTFLength, "GetStringUTFLength", res);
  }
  return res;
}

const char* hooked_GetStringUTFChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  void *caller = __builtin_return_address(0);
  const char *res = g_original_jni_table->GetStringUTFChars(env, str, isCopy);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char *sv = vis_string_value_raw(env, (void*)str);
    char enc[512];
    snprintf(enc, sizeof(enc), "s\x01%.400s\x02", sv ? sv : "");
    free(sv);
    log_jni_call(JNI_SLOT(GetStringUTFChars), "GetStringUTFChars",
                 WIRE_KIND_NULL, "", "", "", "GetStringUTFChars", enc, 0, cs);
    LOG_PTR_RET(GetStringUTFChars, "GetStringUTFChars", res);
    set_reentrant_call(0);
  }
  return res;
}

void hooked_ReleaseStringUTFChars(JNIEnv *env, jstring str, const char *chars) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(ReleaseStringUTFChars, "ReleaseStringUTFChars", str, caller);
    LOG_VOID_RET(ReleaseStringUTFChars, "ReleaseStringUTFChars");
  }
  g_original_jni_table->ReleaseStringUTFChars(env, str, chars);
}

void hooked_GetStringRegion(JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf) {
  void *caller = __builtin_return_address(0);
  g_original_jni_table->GetStringRegion(env, str, start, len, buf);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02I\x01%d\x02", (int)start, (int)len);
    log_jni_call(JNI_SLOT(GetStringRegion), "GetStringRegion",
                 WIRE_KIND_NULL, "", "", "", "GetStringRegion", enc, 0, cs);
    LOG_VOID_RET(GetStringRegion, "GetStringRegion");
  }
}

void hooked_GetStringUTFRegion(JNIEnv *env, jstring str, jsize start, jsize len, char *buf) {
  void *caller = __builtin_return_address(0);
  g_original_jni_table->GetStringUTFRegion(env, str, start, len, buf);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[64]; snprintf(enc, sizeof(enc), "I\x01%d\x02I\x01%d\x02", (int)start, (int)len);
    log_jni_call(JNI_SLOT(GetStringUTFRegion), "GetStringUTFRegion",
                 WIRE_KIND_NULL, "", "", "", "GetStringUTFRegion", enc, 0, cs);
    LOG_VOID_RET(GetStringUTFRegion, "GetStringUTFRegion");
  }
}

/* ============================================================================
 * Array Region Operations
 * ============================================================================ */
#define DEFINE_ARRAY_REGION_HOOKS(Name, CType, ...) \
void hooked_Get##Name##ArrayRegion(JNIEnv *env, CType##Array array, jsize start, jsize len, CType *buf) { \
  void *caller = __builtin_return_address(0); \
  g_original_jni_table->Get##Name##ArrayRegion(env, array, start, len, buf); \
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) { \
    set_reentrant_call(1); \
    char cs[192]; address_of_r(caller, cs, sizeof(cs)); \
    char *_ritems = vis_encode_ptr_array_items(buf, len, PRIMITIVE_SIGCHAR(Name)); \
    size_t _rilen = _ritems ? strlen(_ritems) : 0; \
    char *enc = (char*)malloc(_rilen + 48); \
    if (enc) { \
      int _rpos = snprintf(enc, 48, "I\x01%d\x02I\x01%d\x02[\x01", (int)start, (int)len); \
      if (_rpos < 0) _rpos = 0; else if (_rpos > 47) _rpos = 47; \
      if (_ritems && _rilen > 0) memcpy(enc + _rpos, _ritems, _rilen); \
      _rpos += (int)_rilen; \
      enc[_rpos++] = '\x02'; enc[_rpos] = '\0'; \
      log_jni_call(JNI_SLOT(Get##Name##ArrayRegion), "Get" #Name "ArrayRegion", \
                   WIRE_KIND_NULL, "", "", "", "Get" #Name "ArrayRegion", enc, 0, cs); \
      free(enc); \
      LOG_VOID_RET(Get##Name##ArrayRegion, "Get" #Name "ArrayRegion"); \
    } \
    free(_ritems); \
    set_reentrant_call(0); \
  } \
} \
void hooked_Set##Name##ArrayRegion(JNIEnv *env, CType##Array array, jsize start, jsize len, const CType *buf) { \
  void *caller = __builtin_return_address(0); \
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) { \
    set_reentrant_call(1); \
    char cs[192]; address_of_r(caller, cs, sizeof(cs)); \
    char *_sitems = vis_encode_ptr_array_items(buf, len, PRIMITIVE_SIGCHAR(Name)); \
    size_t _silen = _sitems ? strlen(_sitems) : 0; \
    char *enc = (char*)malloc(_silen + 48); \
    if (enc) { \
      int _spos = snprintf(enc, 48, "I\x01%d\x02I\x01%d\x02[\x01", (int)start, (int)len); \
      if (_spos < 0) _spos = 0; else if (_spos > 47) _spos = 47; \
      if (_sitems && _silen > 0) memcpy(enc + _spos, _sitems, _silen); \
      _spos += (int)_silen; \
      enc[_spos++] = '\x02'; enc[_spos] = '\0'; \
      log_jni_call(JNI_SLOT(Set##Name##ArrayRegion), "Set" #Name "ArrayRegion", \
                   WIRE_KIND_NULL, "", "", "", "Set" #Name "ArrayRegion", enc, 0, cs); \
      free(enc); \
      LOG_VOID_RET(Set##Name##ArrayRegion, "Set" #Name "ArrayRegion"); \
    } \
    free(_sitems); \
    set_reentrant_call(0); \
  } \
  g_original_jni_table->Set##Name##ArrayRegion(env, array, start, len, buf); \
}
JNI_PRIMITIVE_ARRAY_TYPES(DEFINE_ARRAY_REGION_HOOKS)

/* ============================================================================
 * Exception Operations
 * ============================================================================ */
jint hooked_Throw(JNIEnv *env, jthrowable obj) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->Throw(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, Throw, "Throw", obj, caller); LOG_INT_RET(Throw, "Throw", res);
  }
  return res;
}
jint hooked_ThrowNew(JNIEnv *env, jclass clazz, const char *msg) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->ThrowNew(env, clazz, msg);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char *cn = vis_class_name(env, clazz);
    char enc[1024];
    snprintf(enc, sizeof(enc), "c\x01%.500s\x02s\x01%.400s\x02", cn ? cn : "", msg ? msg : "");
    free(cn);
    log_jni_call(JNI_SLOT(ThrowNew), "ThrowNew",
                 WIRE_KIND_NULL, "", "", "", "ThrowNew", enc, 0, cs);
    LOG_INT_RET(ThrowNew, "ThrowNew", res);
    set_reentrant_call(0);
  }
  return res;
}
jthrowable hooked_ExceptionOccurred(JNIEnv *env) {
  void *caller = __builtin_return_address(0);
  jthrowable res = g_original_jni_table->ExceptionOccurred(env);
  if (res && !is_reentrant_call() && should_log_from_caller(env, caller)) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    /* The exception is still pending — JNI calls (GetObjectClass, toString)
     * will fail while it's live.  Save it, clear it temporarily, resolve
     * the display strings, then re-throw so the caller gets it back. */
    g_original_jni_table->ExceptionClear(env);

    log_jni_call(JNI_SLOT(ExceptionOccurred), "ExceptionOccurred",
                 WIRE_KIND_NULL, "", "", "", "ExceptionOccurred", "", 0, cs);
    _log_obj_ret(JNI_SLOT(ExceptionOccurred), "ExceptionOccurred", env, res);

    /* _log_obj_ret runs vis_* helpers which may themselves throw (OOM during
     * GetMethodID, etc.). If a secondary exception is now pending, our
     * Throw(res) below would silently overwrite it. Clear so the original
     * exception we re-throw is the one the caller's ExceptionOccurred peek
     * actually saw. */
    if (g_original_jni_table->ExceptionCheck(env)) {
      g_original_jni_table->ExceptionClear(env);
    }
    /* Re-throw — ExceptionOccurred is a peek, not a clear/consume */
    g_original_jni_table->Throw(env, res);
    set_reentrant_call(0);
  }
  return res;
}
void hooked_ExceptionDescribe(JNIEnv *env) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_VOID_CALL(ExceptionDescribe, "ExceptionDescribe", caller);
    LOG_VOID_RET(ExceptionDescribe, "ExceptionDescribe");
  }
  g_original_jni_table->ExceptionDescribe(env);
}
void hooked_ExceptionClear(JNIEnv *env) {
  void *caller = __builtin_return_address(0);
  int should_log = !is_reentrant_call() && should_log_from_caller(env, caller);
  if (should_log) {
    /* Peek at the pending exception, then ACTUALLY clear it before resolving
     * vis_* helpers — they all bail via vis_safe_to_call while an exception
     * is pending, so the previous code resolved nothing and always logged
     * an empty class name + empty toString. Since the user asked to clear,
     * we don't need to re-throw afterward. */
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    jthrowable exc = g_original_jni_table->ExceptionOccurred(env);
    if (exc) {
      g_original_jni_table->ExceptionClear(env);
      char *cn = vis_object_class_name(env, exc);
      char *ts = vis_object_tostring(env, exc);
      char enc[1024];
      int pos = 0;
      const int cap = (int)sizeof(enc);
      if (vis_is_string(env, exc)) {
        char *sv = vis_string_value_raw(env, exc);
        pos = snprintf(enc, sizeof(enc), "s\x01%.400s", sv ? sv : "");
        free(sv);
      } else {
        pos = snprintf(enc, sizeof(enc), "L\x01%.400s", cn ? cn : "");
      }
      if (pos < 0) pos = 0;
      if (pos > cap - 1) pos = cap - 1;
      if (ts && pos < cap - 2) {
        enc[pos++] = '\x03';
        int w = snprintf(enc + pos, (size_t)(cap - 1 - pos), "%.400s", ts);
        if (w < 0) w = 0;
        if (w > cap - 1 - pos) w = cap - 1 - pos;
        pos += w;
      }
      /* Wire records terminate with \x02 — earlier code omitted this on the
       * toString branch, so the Go decoder would mis-parse the trailing field. */
      if (pos < cap - 1) enc[pos++] = '\x02';
      enc[pos] = '\0';
      free(cn); free(ts);
      log_jni_call(JNI_SLOT(ExceptionClear), "ExceptionClear",
                   WIRE_KIND_NULL, "", "", "", "ExceptionClear", enc, 0, cs);
      g_original_jni_table->DeleteLocalRef(env, exc);
    } else {
      log_jni_call(JNI_SLOT(ExceptionClear), "ExceptionClear",
                   WIRE_KIND_NULL, "", "", "", "ExceptionClear", "", 0, cs);
    }
    LOG_VOID_RET(ExceptionClear, "ExceptionClear");
    set_reentrant_call(0);
  } else {
    /* Not logging — defer to the original which clears as expected. */
    g_original_jni_table->ExceptionClear(env);
  }
}
void hooked_FatalError(JNIEnv *env, const char *msg) {
  void *caller = __builtin_return_address(0);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[512]; snprintf(enc, sizeof(enc), "s\x01%.400s\x02", msg ? msg : "");
    log_jni_call(JNI_SLOT(FatalError), "FatalError",
                 WIRE_KIND_NULL, "", "", "", "FatalError", enc, 0, cs);
  }
  g_original_jni_table->FatalError(env, msg);
}
jboolean hooked_ExceptionCheck(JNIEnv *env) {
  void *caller = __builtin_return_address(0);
  jboolean res = g_original_jni_table->ExceptionCheck(env);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    set_reentrant_call(1);
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    if (res) {
      /* Encode 'true' as call arg, then resolve the exception object as return value. */
      char enc[32];
      snprintf(enc, sizeof(enc), "Z\x01%d\x02", (int)res);
      log_jni_call(JNI_SLOT(ExceptionCheck), "ExceptionCheck",
                   WIRE_KIND_NULL, "", "", "", "ExceptionCheck", enc, 0, cs);
      jthrowable exc = g_original_jni_table->ExceptionOccurred(env);
      _log_obj_ret(JNI_SLOT(ExceptionCheck), "ExceptionCheck", env, exc);
      if (exc) g_original_jni_table->DeleteLocalRef(env, exc);
    } else {
      log_jni_call(JNI_SLOT(ExceptionCheck), "ExceptionCheck",
                   WIRE_KIND_NULL, "", "", "", "ExceptionCheck", "", 0, cs);
      LOG_BOOL_RET(ExceptionCheck, "ExceptionCheck", res);
    }
    set_reentrant_call(0);
  }
  return res;
}

/* ============================================================================
 * Object & Class Operations
 * ============================================================================ */
jobject hooked_AllocObject(JNIEnv *env, jclass clazz) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->AllocObject(env, clazz);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, AllocObject, "AllocObject", clazz, caller);
    LOG_OBJ_RET(env, AllocObject, "AllocObject", res);
  }
  return res;
}
jboolean hooked_IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz) {
  void *caller = __builtin_return_address(0);
  jboolean res = g_original_jni_table->IsInstanceOf(env, obj, clazz);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ2_CALL(env, IsInstanceOf, "IsInstanceOf", obj, clazz, caller);
    LOG_BOOL_RET(IsInstanceOf, "IsInstanceOf", res);
  }
  return res;
}
jclass hooked_DefineClass(JNIEnv *env, const char *name, jobject loader, const jbyte *buf, jsize bufLen) {
  void *caller = __builtin_return_address(0);
  jclass res = g_original_jni_table->DefineClass(env, name, loader, buf, bufLen);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[256]; snprintf(enc, sizeof(enc), "c\x01%.200s\x02I\x01%d\x02",
                             name ? name : "", (int)bufLen);
    log_jni_call(JNI_SLOT(DefineClass), "DefineClass",
                 WIRE_KIND_NULL, "", "", "", "DefineClass", enc, 0, cs);
    LOG_OBJ_RET(env, DefineClass, "DefineClass", res);
  }
  return res;
}
jclass hooked_GetSuperclass(JNIEnv *env, jclass clazz) {
  void *caller = __builtin_return_address(0);
  jclass res = g_original_jni_table->GetSuperclass(env, clazz);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, GetSuperclass, "GetSuperclass", clazz, caller);
    LOG_OBJ_RET(env, GetSuperclass, "GetSuperclass", res);
  }
  return res;
}
jboolean hooked_IsAssignableFrom(JNIEnv *env, jclass clazz1, jclass clazz2) {
  void *caller = __builtin_return_address(0);
  jboolean res = g_original_jni_table->IsAssignableFrom(env, clazz1, clazz2);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ2_CALL(env, IsAssignableFrom, "IsAssignableFrom", clazz1, clazz2, caller);
    LOG_BOOL_RET(IsAssignableFrom, "IsAssignableFrom", res);
  }
  return res;
}
jclass hooked_GetObjectClass(JNIEnv *env, jobject obj) {
  void *caller = __builtin_return_address(0);
  jclass res = g_original_jni_table->GetObjectClass(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, GetObjectClass, "GetObjectClass", obj, caller);
    LOG_OBJ_RET(env, GetObjectClass, "GetObjectClass", res);
  }
  return res;
}
jobjectRefType hooked_GetObjectRefType(JNIEnv *env, jobject obj) {
  void *caller = __builtin_return_address(0);
  jobjectRefType res = g_original_jni_table->GetObjectRefType(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(GetObjectRefType, "GetObjectRefType", obj, caller);
    LOG_INT_RET(GetObjectRefType, "GetObjectRefType", (jint)res);
  }
  return res;
}

/* ============================================================================
 * Direct Buffer Operations
 * ============================================================================ */
jobject hooked_NewDirectByteBuffer(JNIEnv *env, void* address, jlong capacity) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->NewDirectByteBuffer(env, address, capacity);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    char cs[192]; address_of_r(caller, cs, sizeof(cs));
    char enc[128];
    snprintf(enc, sizeof(enc), "p\x01" "0x%lx\x02J\x01%lld\x02",
             (unsigned long)address, (long long)capacity);
    log_jni_call(JNI_SLOT(NewDirectByteBuffer), "NewDirectByteBuffer",
                 WIRE_KIND_NULL, "", "", "", "NewDirectByteBuffer", enc, 0, cs);
    LOG_OBJ_RET(env, NewDirectByteBuffer, "NewDirectByteBuffer", res);
  }
  return res;
}
void* hooked_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
  void *caller = __builtin_return_address(0);
  void *res = g_original_jni_table->GetDirectBufferAddress(env, buf);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(GetDirectBufferAddress, "GetDirectBufferAddress", buf, caller);
    LOG_PTR_RET(GetDirectBufferAddress, "GetDirectBufferAddress", res);
  }
  return res;
}
jlong hooked_GetDirectBufferCapacity(JNIEnv *env, jobject buf) {
  void *caller = __builtin_return_address(0);
  jlong res = g_original_jni_table->GetDirectBufferCapacity(env, buf);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(GetDirectBufferCapacity, "GetDirectBufferCapacity", buf, caller);
    LOG_LONG_RET(GetDirectBufferCapacity, "GetDirectBufferCapacity", res);
  }
  return res;
}

/* ============================================================================
 * Monitor / Misc Operations
 * ============================================================================ */
jint hooked_MonitorEnter(JNIEnv *env, jobject obj) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->MonitorEnter(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(MonitorEnter, "MonitorEnter", obj, caller); LOG_INT_RET(MonitorEnter, "MonitorEnter", res);
  }
  return res;
}
jint hooked_MonitorExit(JNIEnv *env, jobject obj) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->MonitorExit(env, obj);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(MonitorExit, "MonitorExit", obj, caller); LOG_INT_RET(MonitorExit, "MonitorExit", res);
  }
  return res;
}
jint hooked_GetVersion(JNIEnv *env) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->GetVersion(env);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_VOID_CALL(GetVersion, "GetVersion", caller); LOG_INT_RET(GetVersion, "GetVersion", res);
  }
  return res;
}
jint hooked_GetJavaVM(JNIEnv *env, JavaVM **vm) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->GetJavaVM(env, vm);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_VOID_CALL(GetJavaVM, "GetJavaVM", caller); LOG_INT_RET(GetJavaVM, "GetJavaVM", res);
  }
  return res;
}
jint hooked_UnregisterNatives(JNIEnv *env, jclass clazz) {
  void *caller = __builtin_return_address(0);
  jint res = g_original_jni_table->UnregisterNatives(env, clazz);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(UnregisterNatives, "UnregisterNatives", clazz, caller);
    LOG_INT_RET(UnregisterNatives, "UnregisterNatives", res);
  }
  return res;
}
jobject hooked_ToReflectedMethod(JNIEnv *env, jclass cls, jmethodID methodID, jboolean isStatic) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->ToReflectedMethod(env, cls, methodID, isStatic);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(ToReflectedMethod, "ToReflectedMethod", methodID, caller);
    LOG_OBJ_RET(env, ToReflectedMethod, "ToReflectedMethod", res);
  }
  return res;
}
jmethodID hooked_FromReflectedMethod(JNIEnv *env, jobject method) {
  void *caller = __builtin_return_address(0);
  jmethodID res = g_original_jni_table->FromReflectedMethod(env, method);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, FromReflectedMethod, "FromReflectedMethod", method, caller);
    LOG_PTR_RET(FromReflectedMethod, "FromReflectedMethod", res);
  }
  return res;
}
jfieldID hooked_FromReflectedField(JNIEnv *env, jobject field) {
  void *caller = __builtin_return_address(0);
  jfieldID res = g_original_jni_table->FromReflectedField(env, field);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_OBJ_CALL(env, FromReflectedField, "FromReflectedField", field, caller);
    LOG_PTR_RET(FromReflectedField, "FromReflectedField", res);
  }
  return res;
}
jobject hooked_ToReflectedField(JNIEnv *env, jclass cls, jfieldID fieldID, jboolean isStatic) {
  void *caller = __builtin_return_address(0);
  jobject res = g_original_jni_table->ToReflectedField(env, cls, fieldID, isStatic);
  if (!is_reentrant_call() && should_log_from_caller(env, caller)) {
    LOG_PTR_CALL(ToReflectedField, "ToReflectedField", fieldID, caller);
    LOG_OBJ_RET(env, ToReflectedField, "ToReflectedField", res);
  }
  return res;
}

/* ============================================================================
 * Install macros and build_hooked_table
 * ============================================================================ */
#define INSTALL_INSTANCE_NONVOID_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.Call##Name##Method  = hooked_Call##Name##Method;  \
  g_hooked_jni_table.Call##Name##MethodV = hooked_Call##Name##MethodV; \
  g_hooked_jni_table.Call##Name##MethodA = hooked_Call##Name##MethodA;
#define INSTALL_STATIC_NONVOID_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.CallStatic##Name##Method  = hooked_CallStatic##Name##Method;  \
  g_hooked_jni_table.CallStatic##Name##MethodV = hooked_CallStatic##Name##MethodV; \
  g_hooked_jni_table.CallStatic##Name##MethodA = hooked_CallStatic##Name##MethodA;
#define INSTALL_INSTANCE_VOID_HOOKS(Name, CType, RetKind) \
  g_hooked_jni_table.Call##Name##Method  = hooked_Call##Name##Method;  \
  g_hooked_jni_table.Call##Name##MethodV = hooked_Call##Name##MethodV; \
  g_hooked_jni_table.Call##Name##MethodA = hooked_Call##Name##MethodA;
#define INSTALL_STATIC_VOID_HOOKS(Name, CType, RetKind) \
  g_hooked_jni_table.CallStatic##Name##Method  = hooked_CallStatic##Name##Method;  \
  g_hooked_jni_table.CallStatic##Name##MethodV = hooked_CallStatic##Name##MethodV; \
  g_hooked_jni_table.CallStatic##Name##MethodA = hooked_CallStatic##Name##MethodA;
#define INSTALL_NONVIRTUAL_NONVOID_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.CallNonvirtual##Name##Method  = hooked_CallNonvirtual##Name##Method;  \
  g_hooked_jni_table.CallNonvirtual##Name##MethodV = hooked_CallNonvirtual##Name##MethodV; \
  g_hooked_jni_table.CallNonvirtual##Name##MethodA = hooked_CallNonvirtual##Name##MethodA;
#define INSTALL_NONVIRTUAL_VOID_HOOKS(Name, CType, RetKind) \
  g_hooked_jni_table.CallNonvirtual##Name##Method  = hooked_CallNonvirtual##Name##Method;  \
  g_hooked_jni_table.CallNonvirtual##Name##MethodV = hooked_CallNonvirtual##Name##MethodV; \
  g_hooked_jni_table.CallNonvirtual##Name##MethodA = hooked_CallNonvirtual##Name##MethodA;
#define INSTALL_FIELD_GET_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.Get##Name##Field = hooked_Get##Name##Field;
#define INSTALL_FIELD_SET_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.Set##Name##Field = hooked_Set##Name##Field;
#define INSTALL_STATIC_FIELD_GET_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.GetStatic##Name##Field = hooked_GetStatic##Name##Field;
#define INSTALL_STATIC_FIELD_SET_HOOKS(Name, CType, RetKind, UnionF) \
  g_hooked_jni_table.SetStatic##Name##Field = hooked_SetStatic##Name##Field;
#define INSTALL_ARRAY_REGION_HOOKS(Name, CType, ...) \
  g_hooked_jni_table.Get##Name##ArrayRegion = hooked_Get##Name##ArrayRegion; \
  g_hooked_jni_table.Set##Name##ArrayRegion = hooked_Set##Name##ArrayRegion;

static void build_hooked_table(void) {
  memcpy(&g_hooked_jni_table, &g_saved_jni_table, sizeof(g_hooked_jni_table));
  g_hooked_jni_table.FindClass          = hooked_FindClass;
  g_hooked_jni_table.GetMethodID        = hooked_GetMethodID;
  g_hooked_jni_table.GetStaticMethodID  = hooked_GetStaticMethodID;
  g_hooked_jni_table.GetFieldID         = hooked_GetFieldID;
  g_hooked_jni_table.GetStaticFieldID   = hooked_GetStaticFieldID;
  g_hooked_jni_table.RegisterNatives    = hooked_RegisterNatives;
  g_hooked_jni_table.NewGlobalRef       = hooked_NewGlobalRef;
  g_hooked_jni_table.DeleteGlobalRef    = hooked_DeleteGlobalRef;
  g_hooked_jni_table.DeleteLocalRef     = hooked_DeleteLocalRef;
  g_hooked_jni_table.NewLocalRef        = hooked_NewLocalRef;
  g_hooked_jni_table.NewWeakGlobalRef   = hooked_NewWeakGlobalRef;
  g_hooked_jni_table.DeleteWeakGlobalRef = hooked_DeleteWeakGlobalRef;
  g_hooked_jni_table.IsSameObject       = hooked_IsSameObject;
  g_hooked_jni_table.PushLocalFrame     = hooked_PushLocalFrame;
  g_hooked_jni_table.PopLocalFrame      = hooked_PopLocalFrame;
  g_hooked_jni_table.EnsureLocalCapacity = hooked_EnsureLocalCapacity;
  g_hooked_jni_table.NewString          = hooked_NewString;
  g_hooked_jni_table.GetStringLength    = hooked_GetStringLength;
  g_hooked_jni_table.GetStringChars     = hooked_GetStringChars;
  g_hooked_jni_table.ReleaseStringChars = hooked_ReleaseStringChars;
  g_hooked_jni_table.NewStringUTF       = hooked_NewStringUTF;
  g_hooked_jni_table.GetStringUTFLength = hooked_GetStringUTFLength;
  g_hooked_jni_table.GetStringUTFChars  = hooked_GetStringUTFChars;
  g_hooked_jni_table.ReleaseStringUTFChars = hooked_ReleaseStringUTFChars;
  g_hooked_jni_table.GetStringRegion    = hooked_GetStringRegion;
  g_hooked_jni_table.GetStringUTFRegion = hooked_GetStringUTFRegion;
  g_hooked_jni_table.NewObject          = hooked_NewObject;
  g_hooked_jni_table.NewObjectV         = hooked_NewObjectV;
  g_hooked_jni_table.NewObjectA         = hooked_NewObjectA;
  JNI_INSTANCE_NONVOID_TYPES(INSTALL_INSTANCE_NONVOID_HOOKS)
  JNI_STATIC_NONVOID_TYPES(INSTALL_STATIC_NONVOID_HOOKS)
  JNI_INSTANCE_NONVOID_TYPES(INSTALL_NONVIRTUAL_NONVOID_HOOKS)
  JNI_VOID_TYPES(INSTALL_INSTANCE_VOID_HOOKS)
  JNI_VOID_TYPES(INSTALL_STATIC_VOID_HOOKS)
  JNI_VOID_TYPES(INSTALL_NONVIRTUAL_VOID_HOOKS)
  JNI_FIELD_TYPES(INSTALL_FIELD_GET_HOOKS)
  JNI_FIELD_TYPES(INSTALL_FIELD_SET_HOOKS)
  JNI_FIELD_TYPES(INSTALL_STATIC_FIELD_GET_HOOKS)
  JNI_FIELD_TYPES(INSTALL_STATIC_FIELD_SET_HOOKS)
  JNI_PRIMITIVE_ARRAY_TYPES(INSTALL_NEW_ARRAY_HOOKS)
  JNI_PRIMITIVE_ARRAY_TYPES(INSTALL_GET_ARRAY_ELEMENTS_HOOKS)
  JNI_PRIMITIVE_ARRAY_TYPES(INSTALL_RELEASE_ARRAY_ELEMENTS_HOOKS)
  g_hooked_jni_table.NewObjectArray         = hooked_NewObjectArray;
  g_hooked_jni_table.GetObjectArrayElement  = hooked_GetObjectArrayElement;
  g_hooked_jni_table.SetObjectArrayElement  = hooked_SetObjectArrayElement;
  g_hooked_jni_table.GetArrayLength         = hooked_GetArrayLength;
  g_hooked_jni_table.GetPrimitiveArrayCritical    = hooked_GetPrimitiveArrayCritical;
  g_hooked_jni_table.ReleasePrimitiveArrayCritical = hooked_ReleasePrimitiveArrayCritical;
  g_hooked_jni_table.GetStringCritical      = hooked_GetStringCritical;
  g_hooked_jni_table.ReleaseStringCritical  = hooked_ReleaseStringCritical;
  JNI_PRIMITIVE_ARRAY_TYPES(INSTALL_ARRAY_REGION_HOOKS)
  g_hooked_jni_table.Throw             = hooked_Throw;
  g_hooked_jni_table.ThrowNew          = hooked_ThrowNew;
  g_hooked_jni_table.ExceptionOccurred = hooked_ExceptionOccurred;
  g_hooked_jni_table.ExceptionDescribe = hooked_ExceptionDescribe;
  g_hooked_jni_table.ExceptionClear    = hooked_ExceptionClear;
  g_hooked_jni_table.FatalError        = hooked_FatalError;
  g_hooked_jni_table.ExceptionCheck    = hooked_ExceptionCheck;
  g_hooked_jni_table.AllocObject       = hooked_AllocObject;
  g_hooked_jni_table.IsInstanceOf      = hooked_IsInstanceOf;
  g_hooked_jni_table.DefineClass       = hooked_DefineClass;
  g_hooked_jni_table.GetSuperclass     = hooked_GetSuperclass;
  g_hooked_jni_table.IsAssignableFrom  = hooked_IsAssignableFrom;
  g_hooked_jni_table.GetObjectClass    = hooked_GetObjectClass;
  g_hooked_jni_table.GetObjectRefType  = hooked_GetObjectRefType;
  g_hooked_jni_table.NewDirectByteBuffer      = hooked_NewDirectByteBuffer;
  g_hooked_jni_table.GetDirectBufferAddress   = hooked_GetDirectBufferAddress;
  g_hooked_jni_table.GetDirectBufferCapacity  = hooked_GetDirectBufferCapacity;
  g_hooked_jni_table.MonitorEnter      = hooked_MonitorEnter;
  g_hooked_jni_table.MonitorExit       = hooked_MonitorExit;
  g_hooked_jni_table.GetVersion        = hooked_GetVersion;
  g_hooked_jni_table.GetJavaVM         = hooked_GetJavaVM;
  g_hooked_jni_table.UnregisterNatives = hooked_UnregisterNatives;
  g_hooked_jni_table.ToReflectedMethod  = hooked_ToReflectedMethod;
  g_hooked_jni_table.FromReflectedMethod = hooked_FromReflectedMethod;
  g_hooked_jni_table.FromReflectedField = hooked_FromReflectedField;
  g_hooked_jni_table.ToReflectedField   = hooked_ToReflectedField;
}

static int patch_live_table(void) {
  prot_region_t region = jni_table_region(g_live_jni_table, sizeof(*g_live_jni_table));
  if (protect_region(&region, PROT_READ | PROT_WRITE) != 0) return -1;
  memcpy(g_live_jni_table, &g_hooked_jni_table, sizeof(g_hooked_jni_table));
  (void)protect_region(&region, PROT_READ);
  return 0;
}

int install_jni_hooks(void *env_ptr) {
  JNIEnv *env = (JNIEnv *)env_ptr;
  if (!env) return -1;
  pthread_mutex_lock(&g_install_lock);
  if (g_jni_hooks_installed) { pthread_mutex_unlock(&g_install_lock); return 0; }
  g_live_jni_table = *(jni_table_t **)env;
  memcpy(&g_saved_jni_table, g_live_jni_table, sizeof(g_saved_jni_table));
  g_original_jni_table = &g_saved_jni_table;
  build_hooked_table();
  if (patch_live_table() != 0) { pthread_mutex_unlock(&g_install_lock); return -1; }
  g_jni_hooks_installed = 1;
  pthread_mutex_unlock(&g_install_lock);
  return 0;
}

int restore_jni_hooks(void *env_ptr) {
  JNIEnv *env = (JNIEnv *)env_ptr;
  if (!env) return -1;
  pthread_mutex_lock(&g_install_lock);
  if (!g_jni_hooks_installed) { pthread_mutex_unlock(&g_install_lock); return 0; }
  prot_region_t region = jni_table_region(g_live_jni_table, sizeof(*g_live_jni_table));
  if (protect_region(&region, PROT_READ | PROT_WRITE) == 0) {
    memcpy(g_live_jni_table, &g_saved_jni_table, sizeof(g_saved_jni_table));
    (void)protect_region(&region, PROT_READ);
  }
  g_jni_hooks_installed = 0;
  pthread_mutex_unlock(&g_install_lock);
  return 0;
}
