package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -I${SRCDIR}/../shared
#cgo LDFLAGS: -ldl -llog
#include <stdlib.h>
*/
import "C"

import "unsafe"

// logNativeInfo and logNativeWarn write directly to logcat during early init
func logNativeInfo(msg string) {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	goLogNative(C.int(4), cMsg) // ANDROID_LOG_INFO
}

func logNativeWarn(msg string) {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	goLogNative(C.int(5), cMsg) // ANDROID_LOG_WARN
}
