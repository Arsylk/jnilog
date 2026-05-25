//go:build android

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -include ${SRCDIR}/ndk_compat.h
#cgo LDFLAGS: -ldl
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <android/log.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "bridge.h"

#ifndef GO_LOGCAT_WRITE_DEFINED
#define GO_LOGCAT_WRITE_DEFINED
static inline void go_logcat_write(int priority, const char* message) {
  __android_log_write(priority, "JNILogPayload", message);
}
#endif

#ifndef JNI_LOG_GET_TID_DEFINED
#define JNI_LOG_GET_TID_DEFINED
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

// Use gettid() which is available in Android's unistd.h
static inline int jni_log_get_tid() {
  return (int)gettid();
}
#endif

#ifndef __ANDROID__

  static inline void go_logcat_write(int priority, const char* message) {
    (void)priority;
    if (message) { fprintf(stderr, "%s\n", message); }
  }
#endif
*/
import "C"

import (
	"fmt"
	"sync"
	"sync/atomic"
	"unsafe"
)

func writeLogcat(priority int, message string) {
	if message == "" {
		return
	}

	cMessage := C.CString(message)
	defer C.free(unsafe.Pointer(cMessage))

	C.go_logcat_write(C.int(priority), cMessage)
}

//export goJNILogInit
func goJNILogInit() {
	emitInfo("Go runtime initialized")
}

//export goJNILogShutdown
func goJNILogShutdown() {
	emitInfo("Go runtime shutdown")
}

//export goLogCallback
func goLogCallback(message *C.char) {
	if message == nil {
		return
	}
	emitInfoStdout(C.GoString(message))
}

// callFrame holds everything captured at the JNI call site.
// Fields use JNIValue so the renderer has full type info without string parsing.
type callFrame struct {
	jniName    string   // e.g. "CallObjectMethod"
	mid        uintptr  // jmethodID
	className  string   // dotted, e.g. "com.example.Foo"
	methodName string   // e.g. "doSomething"
	receiver   JNIValue // this/self (KindNull for static)
	args       []JNIValue
	caller     string
}

// pendingCall holds a call-site frame plus its offset until the matching
// return arrives via goJNIReturnCallback.
type pendingCall struct {
	frame  *callFrame
	offset int
}

// pendingCalls is keyed by the call_id assigned in C (atomic uint64 counter,
// stashed per-thread on the C side, passed through both callbacks).  Using a
// per-call unique key avoids the per-tid stack push/pop pattern that the
// previous design needed a global Mutex for.
var pendingCalls sync.Map // map[uint64]*pendingCall

//export goJNICallCallback
func goJNICallCallback(
	callID C.uint64_t,
	offset C.int,
	jniName *C.char,
	receiverKind C.int,
	receiverStr *C.char,
	receiverExtra *C.char,
	className *C.char,
	methodName *C.char,
	encodedArgs *C.char,
	mid C.uintptr_t,
	caller *C.char,
) {
	receiver := decodeSingleReceiver(int(receiverKind), C.GoString(receiverStr), C.GoString(receiverExtra))
	args := decodeArgs(C.GoString(encodedArgs))
	pendingCalls.Store(uint64(callID), &pendingCall{
		offset: int(offset),
		frame: &callFrame{
			jniName:    C.GoString(jniName),
			mid:        uintptr(mid),
			className:  normalizeDots(C.GoString(className)),
			methodName: C.GoString(methodName),
			receiver:   receiver,
			args:       args,
			caller:     C.GoString(caller),
		},
	})
}

//export goJNIReturnCallback
func goJNIReturnCallback(
	callID C.uint64_t,
	offset C.int,
	name *C.char,
	retKind C.int,
	retRaw C.uintptr_t,
	retStr *C.char,
	retExtra *C.char,
) {
	v, ok := pendingCalls.LoadAndDelete(uint64(callID))
	if !ok {
		// No matching call (e.g. log_jni_return fired without a prior
		// log_jni_call — common for void/passthrough hook bodies).
		// Emit as standalone return for those that have a payload.
		result := buildReturnValue(int(retKind), uintptr(retRaw),
			C.GoString(retStr), C.GoString(retExtra))
		emitStandaloneReturn(int(offset), C.GoString(name), result)
		return
	}
	pc := v.(*pendingCall)
	result := buildReturnValue(int(retKind), uintptr(retRaw),
		C.GoString(retStr), C.GoString(retExtra))
	emitCallFull(pc.offset, pc.frame, result)
}

//export goJNILookupCallback
func goJNILookupCallback(lookupType *C.char, name *C.char, sig *C.char, clazz C.uintptr_t, className *C.char, caller *C.char) {
	emitJNILookup(
		C.GoString(lookupType),
		C.GoString(name),
		C.GoString(sig),
		uintptr(clazz),
		C.GoString(className),
		C.GoString(caller),
	)
}

//export goJNIRegisterNativesCallback
func goJNIRegisterNativesCallback(clazz C.uintptr_t, className *C.char, methods *C.char, caller *C.char) {
	emitRegisterNatives(uintptr(clazz), C.GoString(className), C.GoString(methods), C.GoString(caller))
}

// goJNIFieldCallback handles Get/SetField and Get/SetStaticField.
//
//export goJNIFieldCallback
func goJNIFieldCallback(
	offset C.int,
	name *C.char,
	receiverKind C.int,
	receiverStr *C.char,
	receiverExtra *C.char,
	fieldName *C.char,
	valueKind C.int,
	valueRaw C.uintptr_t,
	valueStr *C.char,
	valueExtra *C.char,
	caller *C.char,
) {
	receiver := decodeSingleReceiver(int(receiverKind), C.GoString(receiverStr), C.GoString(receiverExtra))
	value := buildReturnValue(int(valueKind), uintptr(valueRaw), C.GoString(valueStr), C.GoString(valueExtra))

	emitFieldAccess(int(offset), C.GoString(name), receiver, C.GoString(fieldName), value, C.GoString(caller))
}

//export goLogNative
func goLogNative(priority C.int, message *C.char) {
	if message == nil {
		return
	}
	msg := C.GoString(message)
	switch int(priority) {
	case 6: // Error
		writeLine(logLevelError, fmt.Sprintf("%s %s", f.formatLogcatPrefix("error", ansiRed), msg))
	case 5: // Warn
		writeLine(logLevelWarn, fmt.Sprintf("%s %s", f.formatLogcatPrefix("warn", ansiYellow), msg))
	default: // Info/Debug
		emitInfo(fmt.Sprintf("%s %s", f.formatLogcatPrefix("log", ansiGreen), msg))
	}
}

//export goLogNativeInfo
func goLogNativeInfo(message *C.char) {
	goLogNative(4, message) // kLogPriorityInfo
}

//export goLogNativeWarn
func goLogNativeWarn(message *C.char) {
	goLogNative(5, message) // kLogPriorityWarn
}

//export goLogNativeError
func goLogNativeError(message *C.char) {
	goLogNative(6, message) // kLogPriorityError
}

// loggingReady is the hottest gate in the system — every C log entry point
// queries it once. An atomic.Bool load is a single-instruction acquire vs.
// the RWMutex round-trip that lived here previously.
var loggingReady atomic.Bool

//export goSetLoggingReady
func goSetLoggingReady(ready C.int) {
	loggingReady.Store(ready != 0)
}

//export goGetLoggingReady
func goGetLoggingReady() C.int {
	if loggingReady.Load() {
		return 1
	}
	return 0
}

func main() {}
