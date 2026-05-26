//go:build android

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge
#include <unistd.h>
#include "event_pipe.h"
*/
import "C"

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sync"
	"syscall"
	"unsafe"
)

// Wire format constants — must match event_pipe.h / event_pipe.c.
const (
	eventMagic         = 0x4A4E4945 // 'JNIE'
	eventHdrFixedBytes = 32

	evCall   = 1
	evReturn = 2
	evLookup = 3
)

var (
	eventReaderOnce sync.Once
)

// startEventPipeReader spawns the goroutine that drains the C→Go event
// socket and routes each event to the matching emit function.  Idempotent
// (safe to call multiple times, only the first spawns).
func startEventPipeReader() {
	eventReaderOnce.Do(func() {
		fd := int(C.event_pipe_consumer_fd())
		if fd < 0 {
			logNativeWarn("event_pipe disabled: no consumer fd")
			return
		}
		go eventPipeReadLoop(fd)
		logNativeInfo(fmt.Sprintf("event_pipe reader started on fd=%d", fd))
	})
}

func eventPipeReadLoop(fd int) {
	buf := make([]byte, int(C.EVENT_PIPE_MAX_BYTES))
	for {
		n, err := syscall.Read(fd, buf)
		if err != nil {
			if errors.Is(err, syscall.EINTR) {
				continue
			}
			logNativeWarn(fmt.Sprintf("event_pipe read failed: %v — reader exiting", err))
			return
		}
		if n < eventHdrFixedBytes {
			logNativeWarn(fmt.Sprintf("event_pipe short datagram: %d bytes", n))
			continue
		}
		if magic := binary.LittleEndian.Uint32(buf[0:4]); magic != eventMagic {
			logNativeWarn(fmt.Sprintf("event_pipe bad magic: 0x%08x", magic))
			continue
		}
		dispatchEvent(buf[:n])
	}
}

// dispatchEvent parses one datagram and routes to the matching emit function.
// Layout matches event_pipe.h.
func dispatchEvent(data []byte) {
	eventType := data[4]
	receiverKind := int(int8(data[5]))
	retKind := int(int8(data[6]))
	nstrings := int(data[7])
	offset := int(int32(binary.LittleEndian.Uint32(data[8:12])))
	// data[12:16] reserved
	callID := binary.LittleEndian.Uint64(data[16:24])
	midOrRaw := binary.LittleEndian.Uint64(data[24:32])

	// Parse the N length-prefixed strings.
	strs, ok := parseStrings(data[eventHdrFixedBytes:], nstrings)
	if !ok {
		logNativeWarn(fmt.Sprintf("event_pipe malformed strings (type=%d nstrings=%d)", eventType, nstrings))
		return
	}

	switch eventType {
	case evCall:
		if len(strs) != 7 {
			return
		}
		dispatchCall(callID, offset, receiverKind, uintptr(midOrRaw), strs)
	case evReturn:
		if len(strs) != 3 {
			return
		}
		dispatchReturn(callID, offset, retKind, uintptr(midOrRaw), strs)
	case evLookup:
		if len(strs) != 5 {
			return
		}
		dispatchLookup(uintptr(midOrRaw), strs)
	default:
		logNativeWarn(fmt.Sprintf("event_pipe unknown event type=%d", eventType))
	}
}

func parseStrings(buf []byte, n int) ([]string, bool) {
	out := make([]string, 0, n)
	pos := 0
	for i := 0; i < n; i++ {
		if pos+2 > len(buf) {
			return nil, false
		}
		l := int(binary.LittleEndian.Uint16(buf[pos : pos+2]))
		pos += 2
		if pos+l > len(buf) {
			return nil, false
		}
		out = append(out, string(buf[pos:pos+l]))
		pos += l
	}
	return out, true
}

func dispatchCall(callID uint64, offset int, receiverKind int, mid uintptr, strs []string) {
	// Slot order: jni_name, receiver_str, receiver_extra, class_name,
	//             method_name, encoded_args, caller
	receiver := decodeSingleReceiver(receiverKind, strs[1], strs[2])
	args := decodeArgs(strs[5])
	pendingCalls.Store(callID, &pendingCall{
		offset: offset,
		frame: &callFrame{
			jniName:    strs[0],
			mid:        mid,
			className:  normalizeDots(strs[3]),
			methodName: strs[4],
			receiver:   receiver,
			args:       args,
			caller:     strs[6],
		},
	})
}

func dispatchReturn(callID uint64, offset int, retKind int, retRaw uintptr, strs []string) {
	// Slot order: name, ret_str, ret_extra
	v, ok := pendingCalls.LoadAndDelete(callID)
	if !ok {
		result := buildReturnValue(retKind, retRaw, strs[1], strs[2])
		emitStandaloneReturn(offset, strs[0], result)
		return
	}
	pc := v.(*pendingCall)
	result := buildReturnValue(retKind, retRaw, strs[1], strs[2])
	emitCallFull(pc.offset, pc.frame, result)
}

func dispatchLookup(clazz uintptr, strs []string) {
	// Slot order: lookup_type, name, sig, class_name, caller
	emitJNILookup(strs[0], strs[1], strs[2], clazz, strs[3], strs[4])
}

// Silence unused-import warnings for the unsafe import (used implicitly by cgo).
var _ = unsafe.Pointer(nil)
