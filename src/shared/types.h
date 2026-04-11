#ifndef JNILOG_TYPES_H
#define JNILOG_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JNI_TYPE_UNKNOWN = 0,
    JNI_TYPE_VOID,
    JNI_TYPE_BOOLEAN,
    JNI_TYPE_BYTE,
    JNI_TYPE_CHAR,
    JNI_TYPE_SHORT,
    JNI_TYPE_INT,
    JNI_TYPE_LONG,
    JNI_TYPE_FLOAT,
    JNI_TYPE_DOUBLE,
    JNI_TYPE_STRING,
    JNI_TYPE_CLASS,
    JNI_TYPE_ARRAY,
    JNI_TYPE_OBJECT,
} jni_value_type_t;

/* Bridge lifecycle (defined in bridge.c) */
extern void bridge_init(void);
extern void bridge_activate_go(void);
extern void bridge_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // JNILOG_TYPES_H
