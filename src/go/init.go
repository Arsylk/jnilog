package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge -I${SRCDIR}/../shared -include ${SRCDIR}/ndk_compat.h -I/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
#include <stdint.h>
#include "bridge.h"
*/
import "C"

import (
	"sync"
	"sync/atomic"
)

var (
	initOnce     sync.Once
	runtimeReady int32 // atomic
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
