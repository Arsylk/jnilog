//go:build android

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge
#include <stdlib.h>
#include <unistd.h>
#include <jni.h>
#include "event_pipe.h"

// Helper: attach the calling OS thread to the JVM, return a JNIEnv*.
// Returns NULL on failure.
static void* attach_consumer_thread(void) {
    JavaVM *vms[1]; jsize n = 0;
    if (JNI_GetCreatedJavaVMs(vms, 1, &n) != JNI_OK || n < 1) return NULL;
    JNIEnv *env = NULL;
    if ((*vms[0])->AttachCurrentThreadAsDaemon(vms[0], &env, NULL) != JNI_OK) return NULL;
    return env;
}
*/
import "C"

import (
	"encoding/binary"
	"errors"
	"fmt"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// Wire format constants — must match event_pipe.h / event_pipe.c.
const (
	eventMagic         = 0x4A4E4945 // 'JNIE'
	eventHdrFixedBytes = 32

	evCall            = 1
	evReturn          = 2
	evLookup          = 3
	evObjReturn       = 4
	evFieldAccess     = 5
	evRegisterNatives = 6
)

// consumerEnv is the JNIEnv* the consumer goroutine owns (after
// AttachCurrentThreadAsDaemon).  Used to call vis_* off the hook thread.
// Set once at consumer goroutine start; never reassigned.
var consumerEnv unsafe.Pointer

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
	// Bind to a single OS thread for the lifetime of this goroutine so the
	// AttachCurrentThreadAsDaemon call below produces a JNIEnv* that stays
	// usable across all subsequent reads on this goroutine.
	runtime.LockOSThread()
	env := C.attach_consumer_thread()
	if env == nil {
		// Off-thread object rendering stays disabled.  The C side gates ALL
		// deferred-gref creation on event_pipe_consumer_ready(), which we leave
		// at 0 here, so no gref is ever minted that this loop could not free (F1).
		logNativeWarn("event_pipe: failed to attach consumer thread to JVM; off-thread rendering disabled")
	} else {
		consumerEnv = unsafe.Pointer(env)
		// Publish readiness AFTER consumerEnv is set (same goroutine), so the
		// first deferred gref the hook side mints is guaranteed renderable here.
		C.event_pipe_set_consumer_ready(1)
		logNativeInfo(fmt.Sprintf("event_pipe consumer attached to JVM, env=%p", env))
		go eventPipeDropMonitor()
	}

	// The reader runs for the lifetime of the process — there is no shutdown
	// path by design.  It returns only on a hard read error (which does not
	// happen for a process-lifetime AF_UNIX socketpair).  On that error we clear
	// the consumer-ready gate so the hook side stops minting grefs we can no
	// longer drain (F24).
	buf := make([]byte, int(C.EVENT_PIPE_MAX_BYTES))
	for {
		n, err := syscall.Read(fd, buf)
		if err != nil {
			if errors.Is(err, syscall.EINTR) {
				continue
			}
			C.event_pipe_set_consumer_ready(0)
			consumerEnv = nil
			logNativeWarn(fmt.Sprintf("event_pipe read failed: %v — reader exiting (drops=%d)",
				err, uint64(C.event_pipe_drops())))
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

// eventPipeDropMonitor periodically reports the cumulative event-drop count
// (writer socket full → consumer behind) so sustained backpressure is visible
// in logcat rather than silent (F24).  Runs for the process lifetime.
func eventPipeDropMonitor() {
	var last uint64
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for range t.C {
		d := uint64(C.event_pipe_drops())
		if d != last {
			logNativeWarn(fmt.Sprintf("event_pipe: %d events dropped total (+%d in last 30s; consumer behind)",
				d, d-last))
			last = d
		}
	}
}

// dispatchEvent parses one datagram and routes to the matching emit function.
// Layout matches event_pipe.h.
func dispatchEvent(data []byte) {
	eventType := data[4]
	// Keep these UNSIGNED — sentinel values like 0xFE (deferred-object field
	// kind) are intentionally above 127 and were losing their identity when
	// sign-extended through int8.
	receiverKind := int(data[5])
	retKind := int(data[6])
	nstrings := int(data[7])
	offset := int(int32(binary.LittleEndian.Uint32(data[8:12])))
	// data[12:16] reserved
	callID := binary.LittleEndian.Uint64(data[16:24])
	midOrRaw := binary.LittleEndian.Uint64(data[24:32])

	// Parse the N length-prefixed strings.
	strs, nstrBytes, ok := parseStringsCount(data[eventHdrFixedBytes:], nstrings)
	if !ok {
		logNativeWarn(fmt.Sprintf("event_pipe malformed strings (type=%d nstrings=%d)", eventType, nstrings))
		return
	}
	// Parse the sidecar (nrefs + refs).  If present, each "\x1A<n>" marker in
	// any string slot is substituted with the rendered chunk for ref n.
	sidecarStart := eventHdrFixedBytes + nstrBytes
	if sidecarStart < len(data) {
		nrefs := int(data[sidecarStart])
		refsStart := sidecarStart + 1
		if refsStart+nrefs*8 <= len(data) && nrefs > 0 {
			rendered := make([]string, nrefs)
			for i := 0; i < nrefs; i++ {
				gref := uintptr(binary.LittleEndian.Uint64(data[refsStart+i*8 : refsStart+(i+1)*8]))
				rendered[i] = renderRefChunk(gref)
			}
			for i := range strs {
				strs[i] = substitutePlaceholders(strs[i], rendered)
			}
		}
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
	case evObjReturn:
		if len(strs) != 1 {
			return
		}
		dispatchObjReturn(callID, offset, uintptr(midOrRaw), strs[0])
	case evFieldAccess:
		if len(strs) != 7 {
			return
		}
		// strs: name, receiver_str, receiver_extra, field_name,
		//       value_str, value_extra, caller
		//
		// retKind == 0xFE (FIELD_KIND_DEFERRED_OBJECT) is a hook-side sentinel
		// meaning "the value is an object — render it via vis_* on the
		// consumer env".  The actual gref lives in midOrRaw at this point.
		// The placeholder substitution above already swapped strs[4] for the
		// "X\x01str[\x03extra]\x02" chunk, so we just need to convert it
		// back into a kind+str+extra triple for buildReturnValue.
		if retKind == 0xFE {
			k, s, e := parseRenderedChunk(strs[4])
			retKind = k
			strs[4] = s
			strs[5] = e
		}
		receiver := decodeSingleReceiver(receiverKind, strs[1], strs[2])
		value := buildReturnValue(retKind, uintptr(midOrRaw), strs[4], strs[5])
		emitFieldAccess(offset, strs[0], receiver, strs[3], value, strs[6])
	case evRegisterNatives:
		if len(strs) != 3 {
			return
		}
		// strs: class_name, methods, caller; clazz rides in midOrRaw as an
		// opaque display id.
		emitRegisterNatives(uintptr(midOrRaw), strs[0], strs[1], strs[2])
	default:
		logNativeWarn(fmt.Sprintf("event_pipe unknown event type=%d", eventType))
	}
}

// dispatchObjReturn renders the jobject globalref on the consumer thread
// (vis_class_name / vis_object_tostring / vis_string_value), then routes to
// the existing emitCallFull pairing path so the final ANSI output matches
// the previous in-thread vis_* behavior byte-for-byte.
//
// The C helper takes ownership of gref and calls DeleteGlobalRef internally.
func dispatchObjReturn(callID uint64, offset int, gref uintptr, name string) {
	if consumerEnv == nil {
		// Unreachable in normal operation since F1: the C side only emits an
		// EV_OBJ_RETURN (and mints its gref) when event_pipe_consumer_ready()
		// is set, which this goroutine sets only AFTER consumerEnv is non-nil.
		// Kept as a defensive guard — emit the pairing with a null return rather
		// than dereferencing a nil env.
		pc, ok := pendingTake(callID)
		if !ok {
			return
		}
		emitCallFull(pc.offset, pc.frame, buildReturnValue(int(KindNull), 0, "", ""))
		return
	}
	var (
		cKind  C.int
		cStr   *C.char
		cExtra *C.char
	)
	C.event_pipe_render_obj(consumerEnv, C.uintptr_t(gref), &cKind, &cStr, &cExtra)
	str := ""
	extra := ""
	if cStr != nil {
		str = C.GoString(cStr)
		C.free(unsafe.Pointer(cStr))
	}
	if cExtra != nil {
		extra = C.GoString(cExtra)
		C.free(unsafe.Pointer(cExtra))
	}
	result := buildReturnValue(int(cKind), gref, str, extra)
	pc, ok := pendingTake(callID)
	if !ok {
		emitStandaloneReturn(offset, name, result)
		return
	}
	emitCallFull(pc.offset, pc.frame, result)
}

func parseStrings(buf []byte, n int) ([]string, bool) {
	out, _, ok := parseStringsCount(buf, n)
	return out, ok
}

// parseStringsCount returns the strings AND the byte count they consumed,
// so the caller can locate the sidecar that follows.
func parseStringsCount(buf []byte, n int) ([]string, int, bool) {
	out := make([]string, 0, n)
	pos := 0
	for i := 0; i < n; i++ {
		if pos+2 > len(buf) {
			return nil, 0, false
		}
		l := int(binary.LittleEndian.Uint16(buf[pos : pos+2]))
		pos += 2
		if pos+l > len(buf) {
			return nil, 0, false
		}
		out = append(out, string(buf[pos:pos+l]))
		pos += l
	}
	return out, pos, true
}

// renderRefChunk: render a single object globalref via the consumer thread's
// JNIEnv* and return the "X\x01str[\x03extra]\x02" chunk that the
// _log_obj_arg_call encoder would have produced for it inline.
func renderRefChunk(gref uintptr) string {
	if consumerEnv == nil || gref == 0 {
		return "p\x01null\x02"
	}
	var (
		cKind  C.int
		cStr   *C.char
		cExtra *C.char
	)
	C.event_pipe_render_obj(consumerEnv, C.uintptr_t(gref), &cKind, &cStr, &cExtra)
	str := ""
	extra := ""
	if cStr != nil {
		str = C.GoString(cStr)
		C.free(unsafe.Pointer(cStr))
	}
	if cExtra != nil {
		extra = C.GoString(cExtra)
		C.free(unsafe.Pointer(cExtra))
	}
	var sig byte
	switch int(cKind) {
	case 10: // WIRE_KIND_STRING
		sig = 's'
	case 11: // WIRE_KIND_CLASS
		sig = 'c'
	case 12: // WIRE_KIND_OBJECT
		sig = 'L'
	default:
		return "p\x01null\x02"
	}
	// Escape the content (F9) so a class name / toString() containing \x01-\x04,
	// \x1A, or the escape byte can't corrupt this chunk's framing once it is
	// substituted into encoded_args (decoded by buildJNIValue) or parsed as a
	// deferred receiver/field value (parseRenderedChunk) — both unescape.
	str = escapeWireContent(str)
	if extra != "" {
		return string(sig) + "\x01" + str + "\x03" + escapeWireContent(extra) + "\x02"
	}
	return string(sig) + "\x01" + str + "\x02"
}

// parseRenderedChunk takes a "X\x01str[\x03extra]\x02" chunk (the output
// of renderRefChunk) and decomposes it back into kind + str + extra so the
// consumer's typed-value builders can ingest it.  Used by EV_FIELD_ACCESS
// when the value_kind sentinel is 0xFE (deferred object).
func parseRenderedChunk(chunk string) (kind int, str string, extra string) {
	if len(chunk) < 2 {
		return 0, "", "" // WIRE_KIND_NULL
	}
	switch chunk[0] {
	case 's':
		kind = 10 // STRING
	case 'c':
		kind = 11 // CLASS
	case 'L':
		kind = 12 // OBJECT
	case 'p':
		kind = 0 // NULL
	default:
		return 0, "", ""
	}
	// Strip leading "X\x01" and trailing "\x02"
	if len(chunk) >= 3 && chunk[1] == '\x01' {
		body := chunk[2:]
		if len(body) > 0 && body[len(body)-1] == '\x02' {
			body = body[:len(body)-1]
		}
		// Split body on "\x03" for L kind (class \x03 toString), then reverse
		// the F9 content escaping — this chunk's consumers (decodeSingleReceiver
		// / buildReturnValue) read standalone fields and do NOT unescape.
		if idx := indexByte(body, '\x03'); idx >= 0 {
			str = unescapeWireContent(body[:idx])
			extra = unescapeWireContent(body[idx+1:])
		} else {
			str = unescapeWireContent(body)
		}
		if kind == 0 {
			str = ""
		}
	}
	return
}

func indexByte(s string, b byte) int {
	for i := 0; i < len(s); i++ {
		if s[i] == b {
			return i
		}
	}
	return -1
}

// substitutePlaceholders replaces every "\x1A<slot>" marker with the
// corresponding rendered chunk.  The slot is encoded by the C emit sites as a
// single byte holding slot+1 (the +1 keeps it non-zero so it survives the
// NUL-terminated C encoder string), so a raw byte — not an ASCII digit — and
// EVENT_PIPE_MAX_REFS is no longer capped at 10 (F8).
func substitutePlaceholders(s string, rendered []string) string {
	if len(rendered) == 0 || len(s) < 2 {
		return s
	}
	out := make([]byte, 0, len(s)+64)
	for i := 0; i < len(s); i++ {
		if s[i] == '\x1A' && i+1 < len(s) {
			slot := int(s[i+1]) - 1
			if slot >= 0 && slot < len(rendered) {
				out = append(out, rendered[slot]...)
				i++ // skip the slot byte
				continue
			}
		}
		out = append(out, s[i])
	}
	return string(out)
}

// pendingCalls backstop (F6).  Under sustained drop load the matching RETURN
// for a stored CALL can be lost (its datagram dropped), which would otherwise
// leave the call frame in pendingCalls forever and grow the Go heap unbounded.
// We keep only a window of the most-recent call-ids and evict older orphans,
// emitting each as a call-without-return (VoidValue → no "→" suffix) so the
// call is still visible and nothing is silently dropped.
//
// pendingCalls and these counters are touched ONLY by the single reader
// goroutine (dispatchCall/dispatchReturn/dispatchObjReturn) — no locking needed.
const (
	pendingWindow       = 4096     // keep frames within this many call-ids of the newest
	pendingEvictTrigger = 2 * pendingWindow
)

var (
	pendingNewest  uint64 // highest call_id stored so far
	pendingLen     int    // live entries in pendingCalls
	pendingEvicted uint64 // cumulative orphaned calls evicted
)

func pendingStore(callID uint64, pc *pendingCall) {
	pendingCalls.Store(callID, pc)
	pendingLen++
	if callID > pendingNewest {
		pendingNewest = callID
	}
	if pendingLen > pendingEvictTrigger {
		evictStalePending()
	}
}

// pendingTake removes and returns the frame for callID, if still present.
func pendingTake(callID uint64) (*pendingCall, bool) {
	v, ok := pendingCalls.LoadAndDelete(callID)
	if !ok {
		return nil, false
	}
	pendingLen--
	return v.(*pendingCall), true
}

func evictStalePending() {
	if pendingNewest <= pendingWindow {
		return
	}
	cutoff := pendingNewest - pendingWindow
	n := 0
	pendingCalls.Range(func(k, v any) bool {
		if k.(uint64) < cutoff {
			if _, ok := pendingCalls.LoadAndDelete(k.(uint64)); ok {
				pendingLen--
				pendingEvicted++
				n++
				pc := v.(*pendingCall)
				emitCallFull(pc.offset, pc.frame, VoidValue)
			}
		}
		return true
	})
	if n > 0 {
		logNativeWarn(fmt.Sprintf("event_pipe: evicted %d orphaned call frame(s) below call_id %d (%d total; matching returns likely dropped)",
			n, cutoff, pendingEvicted))
	}
}

func dispatchCall(callID uint64, offset int, receiverKind int, mid uintptr, strs []string) {
	// Slot order: jni_name, receiver_str, receiver_extra, class_name,
	//             method_name, encoded_args, caller
	//
	// receiverKind == 0xFE (deferred-object sentinel from hook_logging.c
	// classify_object) means strs[1] holds an already-rendered chunk that
	// needs decomposing back into kind/str/extra for decodeSingleReceiver.
	if receiverKind == 0xFE {
		k, s, e := parseRenderedChunk(strs[1])
		receiverKind = k
		strs[1] = s
		strs[2] = e
	}
	receiver := decodeSingleReceiver(receiverKind, strs[1], strs[2])
	args := decodeArgs(strs[5])
	pendingStore(callID, &pendingCall{
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
	pc, ok := pendingTake(callID)
	if !ok {
		result := buildReturnValue(retKind, retRaw, strs[1], strs[2])
		emitStandaloneReturn(offset, strs[0], result)
		return
	}
	result := buildReturnValue(retKind, retRaw, strs[1], strs[2])
	emitCallFull(pc.offset, pc.frame, result)
}

func dispatchLookup(clazz uintptr, strs []string) {
	// Slot order: lookup_type, name, sig, class_name, caller
	// Empty class_name + non-zero clazz = deferred render: clazz is a
	// NewGlobalRef'd jclass — call into vis_class_name on the consumer
	// thread to resolve the name, then DeleteGlobalRef.  We keep the
	// original gref pointer value in the `clazz` slot for display
	// (emitJNILookup formats it as the "→ 0x...." identifier suffix);
	// the gref itself is consumed by event_pipe_render_obj.
	className := strs[3]
	if className == "" && clazz != 0 && consumerEnv != nil {
		var (
			cKind  C.int
			cStr   *C.char
			cExtra *C.char
		)
		C.event_pipe_render_obj(consumerEnv, C.uintptr_t(clazz), &cKind, &cStr, &cExtra)
		if cStr != nil {
			className = C.GoString(cStr)
			C.free(unsafe.Pointer(cStr))
		}
		if cExtra != nil {
			C.free(unsafe.Pointer(cExtra))
		}
		// NOTE: clazz holds the pointer value of the (now-deleted) gref.
		// We preserve it as-is — emitJNILookup uses it purely as an opaque
		// display id, not for any JNI dispatch.
	}
	emitJNILookup(strs[0], strs[1], strs[2], clazz, className, strs[4])
}

// Silence unused-import warnings for the unsafe import (used implicitly by cgo).
var _ = unsafe.Pointer(nil)
