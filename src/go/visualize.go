//go:build android

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -I${SRCDIR}/../shared -include ${SRCDIR}/ndk_compat.h -I/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
#cgo LDFLAGS: -ldl -llog
#include "bridge.h"
#include <stdlib.h>
*/
import "C"

import (
	"unsafe"
)

// visClassName resolves a jclass pointer to "java.lang.String" etc.
func visClassName(ptr uintptr) string {
	if ptr == 0 {
		return ""
	}
	env := C.vis_get_env()
	if env == nil {
		return ""
	}
	cstr := C.vis_class_name(env, unsafe.Pointer(ptr))
	if cstr == nil {
		return ""
	}
	result := C.GoString(cstr)
	C.free(unsafe.Pointer(cstr))
	return result
}

// visObjectClassName resolves a jobject to its class name.
func visObjectClassName(ptr uintptr) string {
	if ptr == 0 {
		return ""
	}
	env := C.vis_get_env()
	if env == nil {
		return ""
	}
	cstr := C.vis_object_class_name(env, unsafe.Pointer(ptr))
	if cstr == nil {
		return ""
	}
	result := C.GoString(cstr)
	C.free(unsafe.Pointer(cstr))
	return result
}
