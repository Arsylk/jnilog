//go:build android

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -include ${SRCDIR}/ndk_compat.h
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
	"unsafe"
)

var (
	initOnce       sync.Once
	bridgeInitOnce sync.Once
)

// InitOnce performs one-shot Go-side range-tracking init. The full "Go side
// ready" signal lives in `loggingReady` (main.go) — there is no separate
// runtime-ready flag, since the only consumer that ever mattered is the
// `goGetLoggingReady` gate used by every C log entry point.
func InitOnce() {
	initOnce.Do(func() {
		logNativeInfo("Go init_once: starting range tracking")
		C.c_init_range_tracking()
		logNativeInfo("Go init_once: range tracking initialized")
	})
}

//export goInitOnce
func goInitOnce() {
	logNativeInfo("Go goInitOnce: go side")
	InitOnce()
}

// goBridgeInit is called from C's bridge_activate_go() via pthread_once.
//
// Fork model (F2): the embedded Go runtime does NOT survive a raw fork() — only
// the forking thread continues in the child, so the scheduler, this reader, and
// every other goroutine are gone.  Re-running goBridgeInit in such a child would
// crash, not recover, so neither the C pthread_once nor this sync.Once attempts
// to.  In the gozinject model that is moot: gozinject traps setArgV0 and dlopens
// the payload into the already-forked, *specialized* app child, so the Go
// runtime is created fresh in that process and goBridgeInit runs exactly once
// there.  Per-process identity that does not need the Go runtime — package name
// and exec-range seeding — is re-resolved PID-aware on the C side (rangeset's
// c_seed_exec_ranges_from_maps), which is what a forked child actually needs.
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
		startEventPipeReader()
	})
}

// goBridgeCleanup is called from C's bridge_cleanup_with_env() during shutdown.
// It clears the logging-ready flag and announces shutdown.
//
//export goBridgeCleanup
func goBridgeCleanup() {
	goSetLoggingReady(0)
	goJNILogShutdown()
}
