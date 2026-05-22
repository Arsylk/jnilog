//go:build android

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -I${SRCDIR}/../shared -include ${SRCDIR}/ndk_compat.h -I/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
#include <stdint.h>
#include <stdlib.h>
#include "bridge.h"
*/
import "C"

import (
	"fmt"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"unsafe"
)

var (
	initOnce       sync.Once
	bridgeInitOnce sync.Once
	runtimeReady   int32 // atomic
)

func InitOnce() {
	initOnce.Do(func() {
		logNativeInfo("Go init_once: starting range tracking")
		C.c_init_range_tracking()
		logNativeInfo("Go init_once: range tracking initialized")
		atomic.StoreInt32(&runtimeReady, 1)
	})
}

//export goInitOnce
func goInitOnce() {
	logNativeInfo("Go goInitOnce: go side")
	InitOnce()
}

//export goSetReady
func goSetReady(ready C.int) {
	if ready != 0 {
		atomic.StoreInt32(&runtimeReady, 1)
	} else {
		atomic.StoreInt32(&runtimeReady, 0)
	}
}

//export goGetReady
func goGetReady() C.int {
	if atomic.LoadInt32(&runtimeReady) != 0 {
		return 1
	}
	return 0
}

// goBridgeInit is called from C's bridge_activate_go() via pthread_once.
// We additionally guard with sync.Once on the Go side so that the Go runtime's
// fresh state after fork() correctly re-initializes (requirement 15.5).
//
//export goBridgeInit
func goBridgeInit() {
	bridgeInitOnce.Do(func() {
		goJNILogInit()
		loadConfig()

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
			if name != "" && !strings.Contains(name, "zygote") && !strings.Contains(name, "app_process") && !strings.Contains(name, "<pre-initialized>") {
				cName := C.CString(name)
				C.c_set_package_name(cName)
				C.free(unsafe.Pointer(cName))
				C.c_seed_exec_ranges_from_maps()
			} else {
				logNativeInfo(fmt.Sprintf("goBridgeInit: skipped package name (zygote/app_process/<pre-initialized>/empty): %q", name))
			}
		} else {
			logNativeInfo("goBridgeInit: failed to read /proc/self/cmdline")
		}

		goSetLoggingReady(1)
	})
}

// goBridgeCleanup is called from C's bridge_cleanup_with_env() during shutdown.
// It clears the logging-ready flag and announces shutdown.
//
//export goBridgeCleanup
func goBridgeCleanup() {
	goSetLoggingReady(0)
	atomic.StoreInt32(&runtimeReady, 0)
	goJNILogShutdown()
}
