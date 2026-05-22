package main

import (
	"strconv"
	"strings"
	"unsafe"
)

// JNIKind is the semantic type tag carried across the C→Go cgo boundary.
// Numeric values are stable — they match wire_kind_t in bridge.h.
type JNIKind uint8

const (
	KindNull    JNIKind = 0  // null reference
	KindVoid    JNIKind = 1  // void return
	KindBoolean JNIKind = 2  // Z
	KindByte    JNIKind = 3  // B
	KindChar    JNIKind = 4  // C
	KindShort   JNIKind = 5  // S
	KindInt     JNIKind = 6  // I
	KindLong    JNIKind = 7  // J
	KindFloat   JNIKind = 8  // F
	KindDouble  JNIKind = 9  // D
	KindString  JNIKind = 10 // java.lang.String — Str = UTF-8 content
	KindClass   JNIKind = 11 // jclass — Str = dotted class name
	KindObject  JNIKind = 12 // arbitrary jobject — Str = class name, Extra = toString
	KindArray   JNIKind = 13 // any jarray — Str = pre-rendered repr from C
	KindPointer JNIKind = 14 // method/field ID or raw ptr — Str = "0x..." hex
)

// JNIValue carries a fully-typed JNI value from hook interception to log rendering.
// Exactly one of Int, Float, Str is meaningful depending on Kind.
type JNIValue struct {
	Kind  JNIKind
	Int   int64      // Boolean, Byte, Char, Short, Int, Long; for Array: overflow count (+N more)
	Float float64    // Float, Double
	Str   string     // String (content), Class (name), Object (class name), Array/Pointer (repr)
	Extra string     // Object only: toString() result
	Items []JNIValue // Array only: per-element typed values
}

// NullValue is the zero JNIValue (kind = KindNull).
var NullValue = JNIValue{Kind: KindNull}

// VoidValue is returned by void methods.
var VoidValue = JNIValue{Kind: KindVoid}

// JNIKindFromReturnKind maps the C return_kind_t int (from bridge.h) to JNIKind.
// The C side passes this as a plain int through the cgo boundary.
func JNIKindFromReturnKind(rkind int) JNIKind {
	switch rkind {
	case 0:
		return KindObject
	case 1:
		return KindBoolean
	case 2:
		return KindByte
	case 3:
		return KindChar
	case 4:
		return KindShort
	case 5:
		return KindInt
	case 6:
		return KindLong
	case 7:
		return KindFloat
	case 8:
		return KindDouble
	case 9:
		return KindVoid
	default:
		return KindNull
	}
}

// JNIKindFromSigChar maps a single JNI descriptor character to JNIKind.
func JNIKindFromSigChar(ch byte) JNIKind {
	switch ch {
	case 'Z':
		return KindBoolean
	case 'B':
		return KindByte
	case 'C':
		return KindChar
	case 'S':
		return KindShort
	case 'I':
		return KindInt
	case 'J':
		return KindLong
	case 'F':
		return KindFloat
	case 'D':
		return KindDouble
	case 'V':
		return KindVoid
	case '[':
		return KindArray
	case 'L':
		return KindObject
	// Extensions used by vis_encode_typed_args and hooks.c helpers:
	case 'p':
		return KindPointer // raw address / native pointer
	case 's':
		return KindString // promoted java.lang.String (content already extracted)
	case 'c':
		return KindClass // promoted jclass (name already extracted)
	default:
		return KindNull
	}
}

// decodeArgs decodes the encoded arg string produced by C's vis_encode_typed_args().
//
// Wire format (one record per arg):
//
//	sigChar \x01 primaryValue [ \x03 extraValue ] \x02
//
// Where:
//   - sigChar   — single JNI descriptor char (Z, I, L, [, ...)
//   - primary   — decimal int for primitives; UTF-8 for strings; "class/Name" for objects
//   - extra     — toString() for objects (separated by \x03), absent for primitives
func decodeArgs(encoded string) []JNIValue {
	if encoded == "" {
		return nil
	}
	records := strings.Split(encoded, "\x02")
	out := make([]JNIValue, 0, len(records))
	for _, rec := range records {
		if rec == "" {
			continue
		}
		parts := strings.SplitN(rec, "\x01", 2)
		if len(parts) != 2 || len(parts[0]) == 0 {
			continue
		}
		kind := JNIKindFromSigChar(parts[0][0])
		out = append(out, buildJNIValue(kind, parts[1]))
	}
	return out
}

// buildReturnValue constructs a typed JNIValue from the components C sends back.
// rkind is wire_kind_t as int (WIRE_KIND_* from bridge.h), which matches JNIKind
// constants directly. log_jni_return always passes WIRE_KIND_* values (return_kind_to_wire
// is applied on the C side before the call reaches Go).
func buildReturnValue(rkind int, raw uintptr, strVal string, extraVal string) JNIValue {
	kind := JNIKind(rkind)
	switch kind {
	case KindNull:
		return NullValue
	case KindVoid:
		return VoidValue
	case KindBoolean:
		return JNIValue{Kind: KindBoolean, Int: int64(raw & 1)}
	case KindByte:
		return JNIValue{Kind: KindByte, Int: int64(int8(raw))}
	case KindChar:
		return JNIValue{Kind: KindChar, Int: int64(uint16(raw))}
	case KindShort:
		return JNIValue{Kind: KindShort, Int: int64(int16(raw))}
	case KindInt:
		return JNIValue{Kind: KindInt, Int: int64(int32(raw))}
	case KindLong:
		return JNIValue{Kind: KindLong, Int: int64(raw)}
	case KindFloat:
		bits := uint32(raw)
		fv := *(*float32)(unsafe.Pointer(&bits))
		return JNIValue{Kind: KindFloat, Float: float64(fv)}
	case KindDouble:
		dv := *(*float64)(unsafe.Pointer(&raw))
		return JNIValue{Kind: KindDouble, Float: dv}
	case KindString:
		return JNIValue{Kind: KindString, Str: strVal}
	case KindClass:
		return JNIValue{Kind: KindClass, Str: normalizeDots(strVal)}
	case KindObject:
		if strVal == "" && raw == 0 {
			return NullValue
		}
		return JNIValue{Kind: KindObject, Str: normalizeDots(strVal), Extra: extraVal}
	case KindArray:
		return buildJNIValue(KindArray, strVal)
	case KindPointer:
		if raw == 0 {
			return NullValue
		}
		return JNIValue{Kind: KindPointer, Str: formatHex(raw)}
	}
	return NullValue
}

// decodeSingleReceiver builds the receiver JNIValue from call callback params.
// rkind is wire_kind_t as int.
func decodeSingleReceiver(rkind int, strVal string, extraVal string) JNIValue {
	switch JNIKind(rkind) {
	case KindNull, KindVoid:
		return NullValue
	case KindObject:
		if strVal == "" {
			return NullValue
		}
		return JNIValue{Kind: KindObject, Str: normalizeDots(strVal), Extra: extraVal}
	case KindClass:
		return JNIValue{Kind: KindClass, Str: normalizeDots(strVal)}
	case KindString:
		return JNIValue{Kind: KindString, Str: strVal}
	default:
		return NullValue
	}
}

// buildJNIValue parses a raw string value for the given kind.
// raw is always plain text — no prior colorization.
func buildJNIValue(kind JNIKind, raw string) JNIValue {
	v := JNIValue{Kind: kind}
	switch kind {
	case KindNull, KindVoid:
		// nothing to parse
	case KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong:
		v.Int, _ = strconv.ParseInt(raw, 10, 64)
	case KindFloat, KindDouble:
		v.Float, _ = strconv.ParseFloat(raw, 64)
	case KindString:
		v.Str = raw
	case KindClass:
		v.Str = normalizeDots(raw)
	case KindObject:
		// Null sentinel from vis_encode_array_items('L'): literal "null"
		if raw == "null" {
			return NullValue
		}
		// raw = "class/Name\x03toStringValue" or just "class/Name"
		if idx := strings.IndexByte(raw, '\x03'); idx >= 0 {
			v.Str = normalizeDots(raw[:idx])
			v.Extra = raw[idx+1:]
		} else {
			v.Str = normalizeDots(raw)
		}
	case KindArray:
		v.Str = raw
		// Wire format: sigChar \x04 item1 \x04 item2 ... [\x04 +N]
		// First byte is the element sigChar; items are each preceded by \x04.
		if len(raw) > 1 {
			itemKind := JNIKindFromSigChar(raw[0])
			// raw[1] is always the first \x04 separator; split from raw[2:] to get items
			parts := strings.Split(raw[2:], "\x04")
			for _, part := range parts {
				if strings.HasPrefix(part, "+") {
					if n, err := strconv.ParseInt(part[1:], 10, 64); err == nil {
						v.Int = n
						continue
					}
				}
				v.Items = append(v.Items, buildJNIValue(itemKind, part))
			}
		}
	case KindPointer:
		v.Str = raw
	}
	return v
}

// normalizeDots converts JNI slash-separated class names to dotted form.
func normalizeDots(s string) string {
	return strings.ReplaceAll(s, "/", ".")
}

func formatHex(v uintptr) string {
	return "0x" + strconv.FormatUint(uint64(v), 16)
}
