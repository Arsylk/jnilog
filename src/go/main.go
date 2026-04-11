package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -I${SRCDIR}/../shared -include ${SRCDIR}/ndk_compat.h
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
	"os"
	"strings"
	"sync"
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

var (
	threadStacks = make(map[int][]*callFrame)
	stacksMu     sync.Mutex
)

func pushCallFrame(tid int, frame *callFrame) {
	stacksMu.Lock()
	defer stacksMu.Unlock()
	threadStacks[tid] = append(threadStacks[tid], frame)
}

func popCallFrame(tid int) *callFrame {
	stacksMu.Lock()
	defer stacksMu.Unlock()
	stack := threadStacks[tid]
	if len(stack) == 0 {
		return nil
	}
	frame := stack[len(stack)-1]
	threadStacks[tid] = stack[:len(stack)-1]
	return frame
}

// goJNICallCallback is invoked at each JNI method call entry point.
//
//export goJNICallCallback
func goJNICallCallback(
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

	tid := int(C.jni_log_get_tid())
	pushCallFrame(tid, &callFrame{
		jniName:    C.GoString(jniName),
		mid:        uintptr(mid),
		className:  normalizeDots(C.GoString(className)),
		methodName: C.GoString(methodName),
		receiver:   receiver,
		args:       args,
		caller:     C.GoString(caller),
	})
}

// goJNIReturnCallback is invoked after the JNI method has returned.
//
//export goJNIReturnCallback
func goJNIReturnCallback(
	offset C.int,
	name *C.char,
	retKind C.int,
	retRaw C.uintptr_t,
	retStr *C.char,
	retExtra *C.char,
) {
	tid := int(C.jni_log_get_tid())
	frame := popCallFrame(tid)
	if frame == nil {
		return
	}

	result := buildReturnValue(int(retKind), uintptr(retRaw), C.GoString(retStr), C.GoString(retExtra))
	emitCallFull(int(offset), frame, result)
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

var (
	loggingReadyMu sync.RWMutex
	loggingReady   bool
)

//export goSetLoggingReady
func goSetLoggingReady(ready C.int) {
	loggingReadyMu.Lock()
	loggingReady = ready != 0
	loggingReadyMu.Unlock()
}

//export goGetLoggingReady
func goGetLoggingReady() C.int {
	loggingReadyMu.RLock()
	ready := loggingReady
	loggingReadyMu.RUnlock()
	if ready {
		return 1
	}
	return 0
}

//export goBridgeInit
func goBridgeInit() {
	goJNILogInit()

	// By the time goBridgeInit fires, the process has been renamed from
	// app_process64 to the actual package name.  Read it from cmdline and
	// push it into the C range tracker so c_seed_exec_ranges_from_maps can
	// filter by package path.
	//
	// /proc/self/cmdline contains argv[] entries joined by null bytes, e.g.:
	//   "com.termux\x00/system/bin/app_process64\x00..."
	// We must take only the FIRST token (argv[0]) — otherwise strings.Contains
	// on the full buffer would falsely match "app_process" in later argv entries.
	if cmdline, err := os.ReadFile("/proc/self/cmdline"); err == nil {
		raw := string(cmdline)
		// Extract argv[0]: everything before the first null byte.
		if idx := strings.IndexByte(raw, 0); idx >= 0 {
			raw = raw[:idx]
		}
		name := strings.TrimSpace(raw)
		logNativeInfo(fmt.Sprintf("goBridgeInit: cmdline argv[0]=%q", name))
		if name != "" && !strings.Contains(name, "zygote") && !strings.Contains(name, "app_process") {
			cName := C.CString(name)
			C.c_set_package_name(cName)
			C.free(unsafe.Pointer(cName))
			C.c_seed_exec_ranges_from_maps()
		} else {
			logNativeInfo(fmt.Sprintf("goBridgeInit: skipped package name (zygote/app_process/empty): %q", name))
		}
	} else {
		logNativeInfo("goBridgeInit: failed to read /proc/self/cmdline")
	}

	goSetLoggingReady(1)
	// Note: JNI hook initialization (init_jni_hooks) remains in C
	// and is called from C's bridge_init after this function returns
}

//export goBridgeCleanup
func goBridgeCleanup() {
	// Note: JNI hook restoration (restore_jni_hooks) remains in C
	// and is called from C's bridge_cleanup before this function
	goSetLoggingReady(0)
	goJNILogShutdown()
}

func main() {}
