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
  __android_log_write(priority, "JniLog", message);
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

// goJNILogInit / goJNILogShutdown announce Go-runtime lifecycle.  Called only
// from goBridgeInit / goBridgeCleanup on the Go side (no C caller), so they are
// plain functions rather than cgo exports.
func goJNILogInit() {
	emitInfo("Go runtime initialized")
}

func goJNILogShutdown() {
	emitInfo("Go runtime shutdown")
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
// return arrives.  Calls and returns are paired by call_id and delivered via
// the event_pipe socket reader (see event_pipe.go: dispatchCall/dispatchReturn).
type pendingCall struct {
	frame  *callFrame
	offset int
}

// pendingCalls is keyed by the call_id assigned in C (atomic uint64 counter,
// stashed per-thread on the C side, carried in the EV_CALL/EV_RETURN records).
// A per-call unique key avoids the per-tid stack push/pop pattern that the
// previous design needed a global Mutex for.
var pendingCalls sync.Map // map[uint64]*pendingCall

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
