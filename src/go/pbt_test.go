package main

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"testing"

	"pgregory.net/rapid"
)

// TestPlaceholder verifies that the rapid PBT library is correctly imported
// and the test infrastructure compiles and runs.
func TestPlaceholder(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		n := rapid.IntRange(0, 100).Draw(t, "n")
		if n < 0 || n > 100 {
			t.Fatalf("generated value %d out of range [0, 100]", n)
		}
	})
}

// ============================================================================
// Wire Protocol Round-Trip Property Test (Property 1)
// Feature: jni-call-logger, Property 1: Wire Protocol Round-Trip
// **Validates: Requirements 2.1, 2.2, 2.5**
// ============================================================================

// sigCharForKind returns the wire protocol sigChar for a given JNIKind.
func sigCharForKind(kind JNIKind) byte {
	switch kind {
	case KindNull:
		return 'n'
	case KindVoid:
		return 'V'
	case KindBoolean:
		return 'Z'
	case KindByte:
		return 'B'
	case KindChar:
		return 'C'
	case KindShort:
		return 'S'
	case KindInt:
		return 'I'
	case KindLong:
		return 'J'
	case KindFloat:
		return 'F'
	case KindDouble:
		return 'D'
	case KindString:
		return 's'
	case KindClass:
		return 'c'
	case KindObject:
		return 'L'
	case KindArray:
		return '['
	case KindPointer:
		return 'p'
	default:
		return 'n'
	}
}

// encodeArgs mirrors C-side vis_encode_typed_args logic.
// It encodes a slice of JNIValues into the wire protocol format:
//
//	sigChar \x01 primaryValue [\x03 extraValue] \x02
//
// Array items are encoded as:
//
//	itemSigChar \x04 item1 \x04 item2 ... [\x04 +N]
func encodeArgs(values []JNIValue) string {
	var buf strings.Builder
	for _, v := range values {
		sig := sigCharForKind(v.Kind)
		buf.WriteByte(sig)
		buf.WriteByte('\x01')
		buf.WriteString(encodePrimaryValue(v))
		if v.Kind == KindObject && v.Extra != "" {
			buf.WriteByte('\x03')
			buf.WriteString(escapeWireContent(v.Extra)) // mirror C vea_append_escaped (F9)
		}
		buf.WriteByte('\x02')
	}
	return buf.String()
}

// encodePrimaryValue encodes the primary value portion for a JNIValue.
func encodePrimaryValue(v JNIValue) string {
	switch v.Kind {
	case KindNull, KindVoid:
		return ""
	case KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong:
		return strconv.FormatInt(v.Int, 10)
	case KindFloat:
		return strconv.FormatFloat(v.Float, 'g', -1, 64)
	case KindDouble:
		return strconv.FormatFloat(v.Float, 'g', -1, 64)
	case KindString:
		return escapeWireContent(v.Str) // mirror C vea_append_escaped (F9)
	case KindClass:
		// Class names are stored with dots; wire uses slashes
		return escapeWireContent(strings.ReplaceAll(v.Str, ".", "/"))
	case KindObject:
		// Object primary is class name (slash-separated on wire)
		return escapeWireContent(strings.ReplaceAll(v.Str, ".", "/"))
	case KindArray:
		return encodeArrayValue(v)
	case KindPointer:
		return v.Str
	default:
		return ""
	}
}

// encodeArrayValue encodes array items in wire format:
// itemSigChar \x04 item1 \x04 item2 ... [\x04 +N]
func encodeArrayValue(v JNIValue) string {
	if len(v.Items) == 0 && v.Int == 0 {
		// Empty array with no overflow — empty string
		return ""
	}

	var buf strings.Builder

	// Determine element sigChar from first item, or use a default
	var elemSig byte
	if len(v.Items) > 0 {
		elemSig = sigCharForKind(v.Items[0].Kind)
	} else {
		// No items but has overflow count — use 'I' as default element type
		elemSig = 'I'
	}
	buf.WriteByte(elemSig)

	for _, item := range v.Items {
		buf.WriteByte('\x04')
		buf.WriteString(encodeItemValue(item))
	}

	// Overflow count (+N)
	if v.Int > 0 {
		buf.WriteByte('\x04')
		buf.WriteString(fmt.Sprintf("+%d", v.Int))
	}

	return buf.String()
}

// encodeItemValue encodes a single array item value.
func encodeItemValue(v JNIValue) string {
	switch v.Kind {
	case KindNull:
		return "null"
	case KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong:
		return strconv.FormatInt(v.Int, 10)
	case KindFloat:
		return strconv.FormatFloat(v.Float, 'g', -1, 64)
	case KindDouble:
		return strconv.FormatFloat(v.Float, 'g', -1, 64)
	case KindString:
		return escapeWireContent(v.Str) // mirror C vea_append_escaped (F9)
	case KindClass:
		return escapeWireContent(strings.ReplaceAll(v.Str, ".", "/"))
	case KindObject:
		if v.Extra != "" {
			return escapeWireContent(strings.ReplaceAll(v.Str, ".", "/")) + "\x03" + escapeWireContent(v.Extra)
		}
		return escapeWireContent(strings.ReplaceAll(v.Str, ".", "/"))
	case KindPointer:
		return v.Str
	default:
		return ""
	}
}

// TestWireProtocolRoundTrip verifies Property 1: Wire Protocol Round-Trip.
// For any list of typed JNI values covering all 15 kinds, encoding into wire
// format and decoding via decodeArgs() produces equivalent JNIValue structs.
//
// **Validates: Requirements 2.1, 2.2, 2.5**
func TestWireProtocolRoundTrip(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		// Generate a list of 0-10 JNI values
		nArgs := rapid.IntRange(0, 10).Draw(t, "nArgs")
		values := make([]JNIValue, nArgs)
		for i := 0; i < nArgs; i++ {
			values[i] = genJNIValue(t)
		}

		// Encode to wire format
		encoded := encodeArgs(values)

		// Decode back
		decoded := decodeArgs(encoded)

		// Assert lengths match
		if len(decoded) != len(values) {
			t.Fatalf("length mismatch: encoded %d values, decoded %d\nencoded string: %q",
				len(values), len(decoded), encoded)
		}

		// Assert each value matches
		for i := range values {
			assertJNIValueEqual(t, i, values[i], decoded[i])
		}
	})
}

// TestWireProtocolRoundTripControlBytes verifies F9: string values and
// toString() extras whose bytes include the frame delimiters \x01-\x04, the
// escape byte \x05, the deferred-render marker \x1A, newlines, or multi-byte
// Unicode survive the encode→decode round-trip intact. Before F9 these bytes
// were emitted raw and the \x01-\x04 split corrupted decoding; genSafeString
// excluded them, masking the bug.
func TestWireProtocolRoundTripControlBytes(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		n := rapid.IntRange(0, 6).Draw(t, "nArgs")
		values := make([]JNIValue, n)
		for i := range values {
			if rapid.Bool().Draw(t, fmt.Sprintf("isObj%d", i)) {
				// Object: tricky bytes in both the class name and the toString
				// extra exercise the \x03 class/extra split too.
				values[i] = JNIValue{Kind: KindObject, Str: "pkg/" + genTrickyString(t), Extra: genTrickyString(t)}
			} else {
				values[i] = JNIValue{Kind: KindString, Str: genTrickyString(t)}
			}
		}
		encoded := encodeArgs(values)
		decoded := decodeArgs(encoded)
		if len(decoded) != len(values) {
			t.Fatalf("length mismatch: %d vs %d (encoded=%q)", len(decoded), len(values), encoded)
		}
		for i := range values {
			switch values[i].Kind {
			case KindString:
				if decoded[i].Str != values[i].Str {
					t.Fatalf("arg[%d] string round-trip: got %q want %q", i, decoded[i].Str, values[i].Str)
				}
			case KindObject:
				// Class name '/' ↔ '.' normalization is lossy by design, so only
				// the (non-normalized) toString extra is asserted byte-exact —
				// its survival proves the \x03 split held despite control bytes
				// in the class name.
				if decoded[i].Extra != values[i].Extra {
					t.Fatalf("arg[%d] object extra round-trip: got %q want %q", i, decoded[i].Extra, values[i].Extra)
				}
			}
		}
	})
}

// assertJNIValueEqual checks that two JNIValues are equivalent after round-trip.
func assertJNIValueEqual(t *rapid.T, idx int, expected, actual JNIValue) {
	prefix := fmt.Sprintf("arg[%d]", idx)

	// Kind must match
	if expected.Kind != actual.Kind {
		t.Fatalf("%s: Kind mismatch: expected %d, got %d", prefix, expected.Kind, actual.Kind)
	}

	switch expected.Kind {
	case KindNull, KindVoid:
		// No fields to check beyond Kind

	case KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong:
		if expected.Int != actual.Int {
			t.Fatalf("%s: Int mismatch: expected %d, got %d", prefix, expected.Int, actual.Int)
		}

	case KindFloat:
		if !floatEqual(expected.Float, actual.Float) {
			t.Fatalf("%s: Float mismatch: expected %g, got %g", prefix, expected.Float, actual.Float)
		}

	case KindDouble:
		if !floatEqual(expected.Float, actual.Float) {
			t.Fatalf("%s: Float mismatch: expected %g, got %g", prefix, expected.Float, actual.Float)
		}

	case KindString:
		if expected.Str != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expected.Str, actual.Str)
		}

	case KindClass:
		// Class names are normalized to dots on decode
		expectedStr := normalizeDots(expected.Str)
		if expectedStr != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expectedStr, actual.Str)
		}

	case KindObject:
		expectedStr := normalizeDots(expected.Str)
		if expectedStr != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expectedStr, actual.Str)
		}
		if expected.Extra != actual.Extra {
			t.Fatalf("%s: Extra mismatch: expected %q, got %q", prefix, expected.Extra, actual.Extra)
		}

	case KindArray:
		// Check overflow count (stored in Int)
		if expected.Int != actual.Int {
			t.Fatalf("%s: Int (overflow) mismatch: expected %d, got %d", prefix, expected.Int, actual.Int)
		}
		// Check items
		if len(expected.Items) != len(actual.Items) {
			t.Fatalf("%s: Items length mismatch: expected %d, got %d", prefix, len(expected.Items), len(actual.Items))
		}
		for j := range expected.Items {
			assertArrayItemEqual(t, idx, j, expected.Items[j], actual.Items[j])
		}

	case KindPointer:
		if expected.Str != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expected.Str, actual.Str)
		}
	}
}

// assertArrayItemEqual checks that two array item JNIValues are equivalent.
func assertArrayItemEqual(t *rapid.T, argIdx, itemIdx int, expected, actual JNIValue) {
	prefix := fmt.Sprintf("arg[%d].Items[%d]", argIdx, itemIdx)

	if expected.Kind != actual.Kind {
		t.Fatalf("%s: Kind mismatch: expected %d, got %d", prefix, expected.Kind, actual.Kind)
	}

	switch expected.Kind {
	case KindNull:
		// OK

	case KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong:
		if expected.Int != actual.Int {
			t.Fatalf("%s: Int mismatch: expected %d, got %d", prefix, expected.Int, actual.Int)
		}

	case KindFloat:
		if !floatEqual(expected.Float, actual.Float) {
			t.Fatalf("%s: Float mismatch: expected %g, got %g", prefix, expected.Float, actual.Float)
		}

	case KindDouble:
		if !floatEqual(expected.Float, actual.Float) {
			t.Fatalf("%s: Float mismatch: expected %g, got %g", prefix, expected.Float, actual.Float)
		}

	case KindString:
		if expected.Str != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expected.Str, actual.Str)
		}

	case KindClass:
		expectedStr := normalizeDots(expected.Str)
		if expectedStr != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expectedStr, actual.Str)
		}

	case KindObject:
		expectedStr := normalizeDots(expected.Str)
		if expectedStr != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expectedStr, actual.Str)
		}
		if expected.Extra != actual.Extra {
			t.Fatalf("%s: Extra mismatch: expected %q, got %q", prefix, expected.Extra, actual.Extra)
		}

	case KindPointer:
		if expected.Str != actual.Str {
			t.Fatalf("%s: Str mismatch: expected %q, got %q", prefix, expected.Str, actual.Str)
		}
	}
}

// floatEqual compares two float64 values, handling NaN and Inf correctly.
// After round-trip through string formatting, we compare the string representations.
func floatEqual(a, b float64) bool {
	// Both NaN
	if math.IsNaN(a) && math.IsNaN(b) {
		return true
	}
	// Both Inf with same sign
	if math.IsInf(a, 1) && math.IsInf(b, 1) {
		return true
	}
	if math.IsInf(a, -1) && math.IsInf(b, -1) {
		return true
	}
	// Compare via string representation (same as encoding path)
	sa := strconv.FormatFloat(a, 'g', -1, 64)
	sb := strconv.FormatFloat(b, 'g', -1, 64)
	return sa == sb
}

// ============================================================================
// Wire Protocol Resilience Property Test (Property 2)
// ============================================================================

// TestWireProtocolResilience verifies Property 2: Wire Protocol Resilience.
// For any arbitrary byte string (including malformed data), decodeArgs() never
// panics and returns a valid []JNIValue slice where each element has a recognized
// JNIKind value in the range 0–14.
//
// **Validates: Requirements 2.6, 2.7**
func TestWireProtocolResilience(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		// Generate an arbitrary byte string — may contain any bytes including
		// control characters, missing \x01 separators, unrecognized sigChars,
		// empty records, and random binary content.
		data := rapid.SliceOf(rapid.Byte()).Draw(t, "data")
		encoded := string(data)

		// decodeArgs must never panic on arbitrary input.
		var result []JNIValue
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Fatalf("decodeArgs panicked on input %q: %v", encoded, r)
				}
			}()
			result = decodeArgs(encoded)
		}()

		// Every returned element must have a valid Kind in range [0, 14].
		for i, v := range result {
			if v.Kind > 14 {
				t.Fatalf("element %d has invalid Kind %d (expected 0–14) for input %q", i, v.Kind, encoded)
			}
		}
	})
}

// ============================================================================
// Type-Directed Value Formatting Property Test (Property 3)
// ============================================================================

// expectedANSICodeForKind returns the ANSI escape code expected for a given
// JNIKind when formatted with colorEnabled=true.
// For KindPointer, the expected code depends on whether the pointer is null/zero.
// For KindObject, the expected code depends on whether Str is empty (null → Magenta)
// or non-empty (class name → Cyan).
func expectedANSICodeForKind(v JNIValue) string {
	switch v.Kind {
	case KindNull:
		return "\x1b[35m" // Magenta
	case KindVoid:
		return "\x1b[90m" // Gray
	case KindBoolean:
		return "\x1b[35m" // Magenta
	case KindByte:
		return "\x1b[35m" // Magenta
	case KindChar:
		return "\x1b[33m" // Yellow
	case KindShort:
		return "\x1b[35m" // Magenta
	case KindInt:
		return "\x1b[35m" // Magenta
	case KindLong:
		return "\x1b[35m" // Magenta
	case KindFloat:
		return "\x1b[35m" // Magenta
	case KindDouble:
		return "\x1b[35m" // Magenta
	case KindString:
		return "\x1b[33m" // Yellow
	case KindClass:
		return "\x1b[36m" // Cyan
	case KindObject:
		if v.Str == "" {
			return "\x1b[35m" // Magenta (null object)
		}
		return "\x1b[36m" // Cyan (class name portion)
	case KindArray:
		return "\x1b[34m" // Blue (brackets)
	case KindPointer:
		if v.Str == "" || v.Str == "0x0" || v.Str == "0" {
			return "\x1b[35m" // Magenta (null pointer)
		}
		return "\x1b[38;2;180;190;254m" // Lavender (non-zero pointer)
	default:
		return "\x1b[90m" // Gray fallback
	}
}

// TestTypeDirectedValueFormatting verifies Property 3: Type-Directed Value Formatting.
// For any JNIValue with valid Kind (0–14), formatJNIValue() with colorEnabled=true
// produces: (a) output containing the designated ANSI code for that Kind,
// (b) every ANSI escape terminated by \x1b[0m, (c) non-empty output.
//
// **Validates: Requirements 3.1–3.6, 3.9, 3.11, 3.12, 18.1–18.11**
func TestTypeDirectedValueFormatting(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		v := genJNIValue(t)
		formatter := lineFormatter{colorEnabled: true}
		output := formatter.formatJNIValue(v)

		// (c) Output must be non-empty
		if output == "" {
			t.Fatalf("formatJNIValue produced empty output for Kind %d, value: %+v", v.Kind, v)
		}

		// (a) Output must contain the designated ANSI code for this Kind
		expectedCode := expectedANSICodeForKind(v)
		if !strings.Contains(output, expectedCode) {
			t.Fatalf("formatJNIValue output missing expected ANSI code %q for Kind %d.\nOutput: %q\nValue: %+v",
				expectedCode, v.Kind, output, v)
		}

		// (b) Every ANSI escape sequence must be terminated by \x1b[0m (reset)
		// Find all ANSI escape sequences and verify each non-reset one is followed
		// by a reset before the end of the string.
		assertAllEscapesTerminated(t, output, v)
	})
}

// assertAllEscapesTerminated verifies that every ANSI escape sequence (that is not
// itself the reset sequence) is eventually followed by a \x1b[0m reset.
func assertAllEscapesTerminated(t *rapid.T, output string, v JNIValue) {
	reset := "\x1b[0m"
	escPrefix := "\x1b["

	i := 0
	for i < len(output) {
		idx := strings.Index(output[i:], escPrefix)
		if idx < 0 {
			break
		}
		escStart := i + idx

		// Find the end of this escape sequence (terminated by a letter)
		escEnd := escStart + 2 // skip \x1b[
		for escEnd < len(output) && !((output[escEnd] >= 'A' && output[escEnd] <= 'Z') || (output[escEnd] >= 'a' && output[escEnd] <= 'z')) {
			escEnd++
		}
		if escEnd < len(output) {
			escEnd++ // include the terminating letter
		}

		escSeq := output[escStart:escEnd]

		// If this is the reset sequence itself, skip it
		if escSeq == reset {
			i = escEnd
			continue
		}

		// This is a non-reset ANSI escape — verify a reset follows somewhere after it
		remainder := output[escEnd:]
		if !strings.Contains(remainder, reset) {
			t.Fatalf("ANSI escape %q at position %d is not terminated by reset (\\x1b[0m).\nOutput: %q\nKind: %d\nValue: %+v",
				escSeq, escStart, output, v.Kind, v)
		}

		i = escEnd
	}
}

// ============================================================================
// NO_COLOR Property Test (Property 6)
// ============================================================================

// TestNoColorDisablesANSI verifies Property 6: NO_COLOR Disables All ANSI Escapes.
// For any JNIValue with valid Kind, formatJNIValue() on a lineFormatter{colorEnabled: false}
// produces output containing zero \x1b[ sequences.
//
// **Validates: Requirements 3.10, 5.3**
func TestNoColorDisablesANSI(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		v := genJNIValue(t)
		f := lineFormatter{colorEnabled: false}
		output := f.formatJNIValue(v)

		if strings.Contains(output, "\x1b[") {
			t.Fatalf("formatJNIValue with colorEnabled=false produced ANSI escape sequence.\nKind: %d\nOutput: %q\nValue: %+v", v.Kind, output, v)
		}
	})
}

// NOTE: the former TestCallFrameStackLIFO (Property 10) and concurrency_test.go
// were removed in the F5 test-honesty pass: they exercised a thread-local
// pushCallFrame/popCallFrame LIFO stack that production no longer has. Call/
// return pairing is now done by call_id (C-side per-thread call-id stack in
// bridge.c + Go-side pendingCalls in event_pipe.go); testing the deleted mirror
// asserted nothing about the shipping code.

// ============================================================================
// Property 11: JNI Signature Parsing Correctness
// ============================================================================

// **Validates: Requirements 17.1, 17.2, 17.3, 17.4, 17.6, 17.7**
//
// For any valid JNI method signature composed of valid type descriptors,
// parseJNISignature output contains: (a) an opening `(` and closing `)` with
// `: ` before the return type, (b) parameter types separated by `, `,
// (c) all object type names with `/` converted to `.` and the `L` prefix and
// `;` suffix removed, (d) array types with `[]` suffixes matching the number
// of `[` prefixes in the descriptor.

// genJNITypeDescriptor generates a valid JNI type descriptor from the grammar.
// It returns the descriptor string and the expected plain type name.
func genJNITypeDescriptor(t *rapid.T, allowVoid bool) (descriptor string, expectedName string) {
	// Primitive types (and void if allowed)
	primitives := []struct {
		desc string
		name string
	}{
		{"Z", "boolean"},
		{"B", "byte"},
		{"C", "char"},
		{"S", "short"},
		{"I", "int"},
		{"J", "long"},
		{"F", "float"},
		{"D", "double"},
	}
	if allowVoid {
		primitives = append(primitives, struct {
			desc string
			name string
		}{"V", "void"})
	}

	// Choose type category: 0=primitive, 1=object, 2=array
	typeCategory := rapid.IntRange(0, 2).Draw(t, "typeCategory")

	switch typeCategory {
	case 0: // Primitive
		idx := rapid.IntRange(0, len(primitives)-1).Draw(t, "primIdx")
		return primitives[idx].desc, primitives[idx].name

	case 1: // Object type Lpackage/Class;
		nParts := rapid.IntRange(1, 4).Draw(t, "objParts")
		parts := make([]string, nParts)
		dottedParts := make([]string, nParts)
		for i := 0; i < nParts; i++ {
			part := rapid.StringMatching(`[a-zA-Z][a-zA-Z0-9]{1,10}`).Draw(t, fmt.Sprintf("objPart%d", i))
			parts[i] = part
			dottedParts[i] = part
		}
		slashName := strings.Join(parts, "/")
		dottedName := strings.Join(dottedParts, ".")
		return "L" + slashName + ";", dottedName

	case 2: // Array type [<element>
		// Array dimensions: 1-3
		dims := rapid.IntRange(1, 3).Draw(t, "arrayDims")
		// Element type: primitive or object (no nested arrays to keep it simple)
		elemCategory := rapid.IntRange(0, 1).Draw(t, "elemCategory")
		var elemDesc, elemName string
		if elemCategory == 0 {
			// Primitive element
			primIdx := rapid.IntRange(0, len(primitives)-2).Draw(t, "arrPrimIdx") // exclude void
			elemDesc = primitives[primIdx].desc
			elemName = primitives[primIdx].name
		} else {
			// Object element
			nParts := rapid.IntRange(1, 3).Draw(t, "arrObjParts")
			parts := make([]string, nParts)
			dottedParts := make([]string, nParts)
			for i := 0; i < nParts; i++ {
				part := rapid.StringMatching(`[a-zA-Z][a-zA-Z0-9]{1,8}`).Draw(t, fmt.Sprintf("arrObjPart%d", i))
				parts[i] = part
				dottedParts[i] = part
			}
			elemDesc = "L" + strings.Join(parts, "/") + ";"
			elemName = strings.Join(dottedParts, ".")
		}
		prefix := strings.Repeat("[", dims)
		suffix := strings.Repeat("[]", dims)
		return prefix + elemDesc, elemName + suffix
	}

	// Fallback (unreachable)
	return "I", "int"
}

// genJNIMethodSignature generates a valid JNI method signature like "(ILjava/lang/String;)V"
// and returns the signature along with expected parameter names and return type name.
func genJNIMethodSignature(t *rapid.T) (sig string, paramNames []string, retName string) {
	nParams := rapid.IntRange(0, 5).Draw(t, "nParams")
	paramNames = make([]string, nParams)

	var sigBuilder strings.Builder
	sigBuilder.WriteByte('(')
	for i := 0; i < nParams; i++ {
		desc, name := genJNITypeDescriptor(t, false) // params cannot be void
		sigBuilder.WriteString(desc)
		paramNames[i] = name
	}
	sigBuilder.WriteByte(')')

	retDesc, retN := genJNITypeDescriptor(t, true) // return can be void
	sigBuilder.WriteString(retDesc)

	return sigBuilder.String(), paramNames, retN
}

// paramDescriptors splits a JNI method signature's parameter list into the
// individual raw param descriptors (e.g. "(I[Ljava/lang/String;)V" → ["I",
// "[Ljava/lang/String;"]). Independent of the production parser — used to check
// rendered array dimensions against the raw `[` prefixes (F5).
func paramDescriptors(sig string) []string {
	open := strings.IndexByte(sig, '(')
	closeP := strings.IndexByte(sig, ')')
	if open < 0 || closeP < 0 || closeP < open {
		return nil
	}
	body := sig[open+1 : closeP]
	var out []string
	for i := 0; i < len(body); {
		start := i
		for i < len(body) && body[i] == '[' {
			i++ // array dimensions
		}
		if i >= len(body) {
			break
		}
		if body[i] == 'L' {
			for i < len(body) && body[i] != ';' {
				i++
			}
			if i < len(body) {
				i++ // include the ';'
			}
		} else {
			i++ // single primitive char
		}
		out = append(out, body[start:i])
	}
	return out
}

// stripANSI removes all ANSI escape sequences from a string.
func stripANSI(s string) string {
	var result strings.Builder
	i := 0
	for i < len(s) {
		if i+1 < len(s) && s[i] == '\x1b' && s[i+1] == '[' {
			// Skip until we find the terminating letter
			j := i + 2
			for j < len(s) && !((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z')) {
				j++
			}
			if j < len(s) {
				j++ // skip the terminating letter
			}
			i = j
		} else {
			result.WriteByte(s[i])
			i++
		}
	}
	return result.String()
}

func TestJNISignatureParsing(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		sig, paramNames, retName := genJNIMethodSignature(t)

		output := parseJNISignature(sig)
		plain := stripANSI(output)

		// (a) Output contains opening `(` and closing `)` with `: ` before return type
		if !strings.Contains(plain, "(") {
			t.Fatalf("output missing '(' for sig %q: %q", sig, plain)
		}
		if !strings.Contains(plain, ")") {
			t.Fatalf("output missing ')' for sig %q: %q", sig, plain)
		}
		if !strings.Contains(plain, "): ") {
			t.Fatalf("output missing '): ' before return type for sig %q: %q", sig, plain)
		}

		// (b) Parameters separated by `, `
		// Extract the content between ( and ):
		parenOpen := strings.Index(plain, "(")
		colonSep := strings.Index(plain, "): ")
		if parenOpen < 0 || colonSep < 0 || colonSep <= parenOpen {
			t.Fatalf("cannot find param section in output for sig %q: %q", sig, plain)
		}
		paramSection := plain[parenOpen+1 : colonSep]

		if len(paramNames) == 0 {
			// Empty params: paramSection should be empty
			if strings.TrimSpace(paramSection) != "" {
				t.Fatalf("expected empty params but got %q for sig %q", paramSection, sig)
			}
		} else if len(paramNames) == 1 {
			// Single param: should match the expected name
			if strings.TrimSpace(paramSection) != paramNames[0] {
				t.Fatalf("expected single param %q but got %q for sig %q", paramNames[0], paramSection, sig)
			}
		} else {
			// Multiple params: should be separated by ", "
			parts := strings.Split(paramSection, ", ")
			if len(parts) != len(paramNames) {
				t.Fatalf("expected %d params but got %d parts in %q for sig %q",
					len(paramNames), len(parts), paramSection, sig)
			}
			for i, part := range parts {
				if part != paramNames[i] {
					t.Fatalf("param %d: expected %q but got %q for sig %q",
						i, paramNames[i], part, sig)
				}
			}
		}

		// (c) Verify return type appears after "): "
		retSection := plain[colonSep+3:] // after "): "
		if retSection != retName {
			t.Fatalf("expected return type %q but got %q for sig %q", retName, retSection, sig)
		}

		// (d) Verify `/` is not present in the output (converted to `.`)
		if strings.Contains(plain, "/") {
			t.Fatalf("output still contains '/' (should be converted to '.') for sig %q: %q", sig, plain)
		}

		// (e) The rendered `[]`-suffix count for each param MUST equal the number
		// of array-dimension `[` prefixes in that param's raw descriptor. The old
		// check only asserted the param name appeared somewhere — it never tied
		// the suffix count to the prefix count, so a renderer that dropped or
		// duplicated a dimension would pass. Re-parse the raw sig independently
		// and compare counts (F5 test nit).
		rawParams := paramDescriptors(sig)
		if len(rawParams) != len(paramNames) {
			t.Fatalf("param-count mismatch for sig %q: %d raw descriptors vs %d names",
				sig, len(rawParams), len(paramNames))
		}
		for i, raw := range rawParams {
			wantDims := 0
			for wantDims < len(raw) && raw[wantDims] == '[' {
				wantDims++
			}
			gotDims := strings.Count(paramNames[i], "[]")
			if gotDims != wantDims {
				t.Fatalf("param %d (descriptor %q): rendered %q has %d []-suffixes, "+
					"raw descriptor has %d [-prefixes (sig %q)",
					i, raw, paramNames[i], gotDims, wantDims, sig)
			}
		}
	})
}

// ============================================================================
// Array Rendering Respects Configured Limit Property Test (Property 7)
// ============================================================================

// TestArrayRenderingLimit verifies Property 7: Array Rendering Respects Configured Limit.
// For any KindArray JNIValue with N items and configured max M:
// (a) if N ≤ M, the formatted output contains no "+X more" suffix
// (b) if N > M, the formatted output contains "+K more" where K = N − M
//
// **Validates: Requirements 6.1, 6.2, 6.4, 6.7**
func TestArrayRenderingLimit(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		// Generate total number of items (0–100) and max limit (1–50)
		totalItems := rapid.IntRange(0, 100).Draw(t, "totalItems")
		maxItems := rapid.IntRange(1, 50).Draw(t, "maxItems")

		// Generate array element kind (non-array, non-void to avoid deep recursion)
		elemKinds := []JNIKind{
			KindNull, KindBoolean, KindByte, KindChar, KindShort,
			KindInt, KindLong, KindFloat, KindDouble, KindString,
			KindClass, KindObject, KindPointer,
		}
		elemKindIdx := rapid.IntRange(0, len(elemKinds)-1).Draw(t, "elemKindIdx")
		elemKind := elemKinds[elemKindIdx]

		// Simulate what the C layer does: keep min(totalItems, maxItems) items,
		// set overflow count = max(0, totalItems - maxItems)
		renderedCount := totalItems
		if renderedCount > maxItems {
			renderedCount = maxItems
		}
		overflow := totalItems - renderedCount // = max(0, totalItems - maxItems)

		// Build the array items
		items := make([]JNIValue, renderedCount)
		for i := 0; i < renderedCount; i++ {
			items[i] = genJNIValueOfKind(t, elemKind)
		}

		// Construct the KindArray JNIValue as it would arrive from the wire decoder
		arrayValue := JNIValue{
			Kind:  KindArray,
			Int:   int64(overflow),
			Items: items,
		}

		// Format the value
		formatter := lineFormatter{colorEnabled: false}
		output := formatter.formatJNIValue(arrayValue)

		// Property assertions
		if totalItems == 0 {
			// (a) Empty array: should render as "[]" with no "+X more"
			if strings.Contains(output, "more") {
				t.Fatalf("empty array (N=0, M=%d) should not contain 'more' suffix.\nOutput: %q",
					maxItems, output)
			}
			if output != "[]" {
				t.Fatalf("empty array should render as '[]', got: %q", output)
			}
		} else if totalItems <= maxItems {
			// (a) N ≤ M: no "+X more" suffix
			if strings.Contains(output, "more") {
				t.Fatalf("array with N=%d ≤ M=%d should not contain 'more' suffix.\nOutput: %q",
					totalItems, maxItems, output)
			}
		} else {
			// (b) N > M: output contains "+K more" where K = N − M
			expectedOverflow := totalItems - maxItems
			expectedSuffix := fmt.Sprintf("+%d more", expectedOverflow)
			if !strings.Contains(output, expectedSuffix) {
				t.Fatalf("array with N=%d > M=%d should contain %q suffix.\nOutput: %q",
					totalItems, maxItems, expectedSuffix, output)
			}
		}
	})
}

// ============================================================================
// Unit Tests for Uri, File, and ComponentName Parsers (Task 10.3)
// **Validates: Requirements 7.4, 7.5**
// ============================================================================

// TestFormatUriFileComponentName verifies that formatObject renders:
// - android.net.Uri as Uri("content") with content in Yellow
// - java.io.File as File("content") with content in Yellow
// - android.content.ComponentName as ComponentName{"content"} with content in Yellow
func TestFormatUriFileComponentName(t *testing.T) {
	formatter := lineFormatter{colorEnabled: true}
	formatterNoColor := lineFormatter{colorEnabled: false}

	// --- Test case 1: android.net.Uri ---
	uriOutput := formatter.formatObject("android.net.Uri", "content://com.example/path")
	uriPlain := stripANSI(uriOutput)

	// Structural check: ClassName("content")
	expectedUriPlain := `android.net.Uri("content://com.example/path")`
	if uriPlain != expectedUriPlain {
		t.Fatalf("Uri plain output mismatch:\n  got:  %q\n  want: %q", uriPlain, expectedUriPlain)
	}
	// Color check: content should be in Yellow
	if !strings.Contains(uriOutput, ansiYellow+"content://com.example/path"+ansiReset) {
		t.Fatalf("Uri output missing Yellow-colored content.\nOutput: %q", uriOutput)
	}

	// Verify no-color mode produces no ANSI
	uriNoColor := formatterNoColor.formatObject("android.net.Uri", "content://com.example/path")
	if strings.Contains(uriNoColor, "\x1b[") {
		t.Fatalf("Uri no-color output contains ANSI escapes: %q", uriNoColor)
	}

	// --- Test case 2: java.io.File ---
	fileOutput := formatter.formatObject("java.io.File", "/data/local/tmp/file.txt")
	filePlain := stripANSI(fileOutput)

	expectedFilePlain := `java.io.File("/data/local/tmp/file.txt")`
	if filePlain != expectedFilePlain {
		t.Fatalf("File plain output mismatch:\n  got:  %q\n  want: %q", filePlain, expectedFilePlain)
	}
	// Color check: content should be in Yellow
	if !strings.Contains(fileOutput, ansiYellow+"/data/local/tmp/file.txt"+ansiReset) {
		t.Fatalf("File output missing Yellow-colored content.\nOutput: %q", fileOutput)
	}

	// Verify no-color mode produces no ANSI
	fileNoColor := formatterNoColor.formatObject("java.io.File", "/data/local/tmp/file.txt")
	if strings.Contains(fileNoColor, "\x1b[") {
		t.Fatalf("File no-color output contains ANSI escapes: %q", fileNoColor)
	}

	// --- Test case 3: android.content.ComponentName ---
	compOutput := formatter.formatObject("android.content.ComponentName", "com.example/.MainActivity")
	compPlain := stripANSI(compOutput)

	expectedCompPlain := `android.content.ComponentName{"com.example/.MainActivity"}`
	if compPlain != expectedCompPlain {
		t.Fatalf("ComponentName plain output mismatch:\n  got:  %q\n  want: %q", compPlain, expectedCompPlain)
	}
	// Color check: content should be in Yellow
	if !strings.Contains(compOutput, ansiYellow+"com.example/.MainActivity"+ansiReset) {
		t.Fatalf("ComponentName output missing Yellow-colored content.\nOutput: %q", compOutput)
	}

	// Verify no-color mode produces no ANSI
	compNoColor := formatterNoColor.formatObject("android.content.ComponentName", "com.example/.MainActivity")
	if strings.Contains(compNoColor, "\x1b[") {
		t.Fatalf("ComponentName no-color output contains ANSI escapes: %q", compNoColor)
	}
}

// TestJNISignatureParsingStandaloneField verifies Property 11 for standalone
// field descriptors (Requirement 17.7): a descriptor without method parentheses
// is parsed as a single type name.
//
// **Validates: Requirements 17.7**
func TestJNISignatureParsingStandaloneField(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		desc, expectedName := genJNITypeDescriptor(t, false) // no void for field types

		output := parseJNISignature(desc)
		plain := stripANSI(output)

		// Standalone field descriptor should produce just the type name
		// (no parentheses, no ": " separator)
		if plain != expectedName {
			t.Fatalf("standalone descriptor %q: expected %q but got %q", desc, expectedName, plain)
		}

		// Should not contain `/` (converted to `.`)
		if strings.Contains(plain, "/") {
			t.Fatalf("standalone descriptor output contains '/': %q for desc %q", plain, desc)
		}
	})
}

// ============================================================================
// Unit Tests for Intent Parser (Requirement 7.1)
// ============================================================================

// TestFormatIntent verifies the formatIntent function with representative
// toString values from android.content.Intent objects.
//
// **Validates: Requirements 7.1**
func TestFormatIntent(t *testing.T) {
	formatter := lineFormatter{colorEnabled: false}

	t.Run("full intent with act/dat/cmp/extras", func(t *testing.T) {
		toString := "Intent { act=android.intent.action.VIEW dat=content://contacts/people/1 cmp=com.example/.MainActivity (has extras) }"
		output := formatter.formatIntent(toString)

		// Should contain the class name
		if !strings.Contains(output, "android") || !strings.Contains(output, "content") || !strings.Contains(output, "Intent") {
			t.Fatalf("output missing class name components.\nOutput: %q", output)
		}

		// Should contain act= field with its value
		if !strings.Contains(output, "act=") {
			t.Fatalf("output missing 'act=' field.\nOutput: %q", output)
		}
		if !strings.Contains(output, "android.intent.action.VIEW") {
			t.Fatalf("output missing act value 'android.intent.action.VIEW'.\nOutput: %q", output)
		}

		// Should contain dat= field with its value
		if !strings.Contains(output, "dat=") {
			t.Fatalf("output missing 'dat=' field.\nOutput: %q", output)
		}
		if !strings.Contains(output, "content://contacts/people/1") {
			t.Fatalf("output missing dat value 'content://contacts/people/1'.\nOutput: %q", output)
		}

		// Should contain cmp= field with its value
		if !strings.Contains(output, "cmp=") {
			t.Fatalf("output missing 'cmp=' field.\nOutput: %q", output)
		}
		if !strings.Contains(output, "com.example/.MainActivity") {
			t.Fatalf("output missing cmp value 'com.example/.MainActivity'.\nOutput: %q", output)
		}

		// Should contain "has extras" indicator
		if !strings.Contains(output, "has extras") {
			t.Fatalf("output missing 'has extras' indicator.\nOutput: %q", output)
		}

		// Should contain separators: { and }
		if !strings.Contains(output, "{") || !strings.Contains(output, "}") {
			t.Fatalf("output missing '{' or '}' separators.\nOutput: %q", output)
		}

		// Should contain comma separators between fields
		if !strings.Contains(output, ", ") {
			t.Fatalf("output missing ', ' separator between fields.\nOutput: %q", output)
		}
	})

	t.Run("intent with only act field", func(t *testing.T) {
		toString := "Intent { act=android.intent.action.MAIN }"
		output := formatter.formatIntent(toString)

		// Should contain act= field
		if !strings.Contains(output, "act=") {
			t.Fatalf("output missing 'act=' field.\nOutput: %q", output)
		}
		if !strings.Contains(output, "android.intent.action.MAIN") {
			t.Fatalf("output missing act value 'android.intent.action.MAIN'.\nOutput: %q", output)
		}

		// Should NOT contain dat= or cmp= fields
		if strings.Contains(output, "dat=") {
			t.Fatalf("output should not contain 'dat=' for intent with only act.\nOutput: %q", output)
		}
		if strings.Contains(output, "cmp=") {
			t.Fatalf("output should not contain 'cmp=' for intent with only act.\nOutput: %q", output)
		}

		// Should NOT contain "has extras"
		if strings.Contains(output, "has extras") {
			t.Fatalf("output should not contain 'has extras' for intent without extras.\nOutput: %q", output)
		}

		// Should contain { and } separators
		if !strings.Contains(output, "{") || !strings.Contains(output, "}") {
			t.Fatalf("output missing '{' or '}' separators.\nOutput: %q", output)
		}

		// Should NOT contain comma separator (only one field)
		if strings.Contains(output, ", ") {
			t.Fatalf("output should not contain ', ' separator for single-field intent.\nOutput: %q", output)
		}
	})

	t.Run("empty intent (no fields)", func(t *testing.T) {
		toString := "Intent { }"
		output := formatter.formatIntent(toString)

		// Should contain the class name
		if !strings.Contains(output, "android") || !strings.Contains(output, "content") || !strings.Contains(output, "Intent") {
			t.Fatalf("output missing class name components.\nOutput: %q", output)
		}

		// Should NOT contain act=, dat=, cmp=, or "has extras"
		if strings.Contains(output, "act=") {
			t.Fatalf("output should not contain 'act=' for empty intent.\nOutput: %q", output)
		}
		if strings.Contains(output, "dat=") {
			t.Fatalf("output should not contain 'dat=' for empty intent.\nOutput: %q", output)
		}
		if strings.Contains(output, "cmp=") {
			t.Fatalf("output should not contain 'cmp=' for empty intent.\nOutput: %q", output)
		}
		if strings.Contains(output, "has extras") {
			t.Fatalf("output should not contain 'has extras' for empty intent.\nOutput: %q", output)
		}

		// Should still contain { and } separators (empty braces)
		if !strings.Contains(output, "{") || !strings.Contains(output, "}") {
			t.Fatalf("output missing '{' or '}' separators.\nOutput: %q", output)
		}
	})
}

// ============================================================================
// Unit Tests for Bundle Parser (formatBundle)
// ============================================================================

// TestFormatBundle verifies the formatBundle function renders Bundle objects
// correctly based on their toString content.
//
// **Validates: Requirements 7.2, 7.3**
func TestFormatBundle(t *testing.T) {
	formatter := lineFormatter{colorEnabled: true}
	formatterNoColor := lineFormatter{colorEnabled: false}

	t.Run("empty bundle renders empty", func(t *testing.T) {
		// Bundle[{}] is the toString for an empty Bundle
		output := formatter.formatBundle("Bundle[{}]")
		plain := stripANSI(output)

		if !strings.Contains(plain, "empty") {
			t.Fatalf("expected 'empty' in output for empty bundle, got: %q", plain)
		}
		// Should NOT contain "…" for empty bundles
		if strings.Contains(plain, "…") {
			t.Fatalf("empty bundle should not contain '…', got: %q", plain)
		}
		// Should contain the class name
		if !strings.Contains(plain, "android.os.Bundle") {
			t.Fatalf("expected class name 'android.os.Bundle' in output, got: %q", plain)
		}
		// Verify "empty" is rendered in magenta when color is enabled
		if !strings.Contains(output, ansiMagenta+"empty"+ansiReset) {
			t.Fatalf("expected 'empty' in magenta, got: %q", output)
		}
	})

	t.Run("non-empty bundle renders ellipsis", func(t *testing.T) {
		// Bundle[mParcelledData.dataSize=N] is the toString for a non-empty Bundle
		output := formatter.formatBundle("Bundle[mParcelledData.dataSize=128]")
		plain := stripANSI(output)

		if !strings.Contains(plain, "…") {
			t.Fatalf("expected '…' in output for non-empty bundle, got: %q", plain)
		}
		// Should NOT contain "empty" for non-empty bundles
		if strings.Contains(plain, "empty") {
			t.Fatalf("non-empty bundle should not contain 'empty', got: %q", plain)
		}
		// Should contain the class name
		if !strings.Contains(plain, "android.os.Bundle") {
			t.Fatalf("expected class name 'android.os.Bundle' in output, got: %q", plain)
		}
		// Verify "…" is rendered in gray when color is enabled
		if !strings.Contains(output, ansiGray+"…"+ansiReset) {
			t.Fatalf("expected '…' in gray, got: %q", output)
		}
	})

	t.Run("empty bundle no color", func(t *testing.T) {
		output := formatterNoColor.formatBundle("Bundle[{}]")

		if strings.Contains(output, "\x1b[") {
			t.Fatalf("expected no ANSI codes with colorEnabled=false, got: %q", output)
		}
		if !strings.Contains(output, "empty") {
			t.Fatalf("expected 'empty' in no-color output, got: %q", output)
		}
	})

	t.Run("non-empty bundle no color", func(t *testing.T) {
		output := formatterNoColor.formatBundle("Bundle[mParcelledData.dataSize=256]")

		if strings.Contains(output, "\x1b[") {
			t.Fatalf("expected no ANSI codes with colorEnabled=false, got: %q", output)
		}
		if !strings.Contains(output, "…") {
			t.Fatalf("expected '…' in no-color output, got: %q", output)
		}
	})

	t.Run("bundle with neither empty nor Bundle[ prefix", func(t *testing.T) {
		// Edge case: toString doesn't match either pattern
		output := formatter.formatBundle("some random string")
		plain := stripANSI(output)

		// Should still contain the class name and braces
		if !strings.Contains(plain, "android.os.Bundle") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}
		// Should contain neither "empty" nor "…"
		if strings.Contains(plain, "empty") || strings.Contains(plain, "…") {
			t.Fatalf("unexpected content for unmatched toString, got: %q", plain)
		}
	})
}

// ============================================================================
// Unit Test: Sink Panic Recovery (Task 9.2)
// ============================================================================

// panickingSink is a test sink that panics on every Write call.
type panickingSink struct{}

func (panickingSink) Write(event logEvent) {
	panic("intentional panic from panickingSink")
}

// recordingSink is a test sink that records all events it receives.
type recordingSink struct {
	events []logEvent
}

func (r *recordingSink) Write(event logEvent) {
	r.events = append(r.events, event)
}

func (r *recordingSink) lastLine() string {
	if len(r.events) == 0 {
		return ""
	}
	return r.events[len(r.events)-1].line
}

func (r *recordingSink) reset() {
	r.events = r.events[:0]
}

// TestSinkPanicRecovery verifies that a panicking sink does not prevent
// subsequent sinks from receiving events (Requirement 16.7).
//
// **Validates: Requirements 16.7**
func TestSinkPanicRecovery(t *testing.T) {
	// Create a panicking sink and a recording sink
	panicSink := panickingSink{}
	recorder := &recordingSink{}

	// Register panicking sink FIRST, then recording sink
	logger := newMultiOutputLogger(panicSink, recorder)

	// Write an event
	event := logEvent{level: logLevelInfo, line: "test event after panic"}
	logger.Write(event)

	// The recording sink must still receive the event despite the first sink panicking
	if len(recorder.events) != 1 {
		t.Fatalf("expected recording sink to receive 1 event, got %d", len(recorder.events))
	}
	if recorder.events[0].line != "test event after panic" {
		t.Fatalf("expected event line %q, got %q", "test event after panic", recorder.events[0].line)
	}
	if recorder.events[0].level != logLevelInfo {
		t.Fatalf("expected event level %d, got %d", logLevelInfo, recorder.events[0].level)
	}
}

// ============================================================================
// Property 12: Field Access Line Schema
// ============================================================================

// jniFieldAccessNames lists all valid JNI field access function names.
var jniFieldAccessNames = []string{
	"GetObjectField", "GetBooleanField", "GetByteField", "GetCharField",
	"GetShortField", "GetIntField", "GetLongField", "GetFloatField", "GetDoubleField",
	"SetObjectField", "SetBooleanField", "SetByteField", "SetCharField",
	"SetShortField", "SetIntField", "SetLongField", "SetFloatField", "SetDoubleField",
	"GetStaticObjectField", "GetStaticBooleanField", "GetStaticByteField", "GetStaticCharField",
	"GetStaticShortField", "GetStaticIntField", "GetStaticLongField", "GetStaticFloatField", "GetStaticDoubleField",
	"SetStaticObjectField", "SetStaticBooleanField", "SetStaticByteField", "SetStaticCharField",
	"SetStaticShortField", "SetStaticIntField", "SetStaticLongField", "SetStaticFloatField", "SetStaticDoubleField",
}

// TestFieldAccessLineSchema verifies Property 12: Field Access Line Schema.
// For any field access event with a JNI function name, receiver JNIValue, field name,
// and value JNIValue:
// (a) if the JNI function name contains "Static", the output SHALL NOT contain `this=`
// (b) if the JNI function name starts with "Set", the output SHALL contain the `←` arrow
// (c) if the JNI function name starts with "Get", the output SHALL contain the `→` arrow
// (d) if the field name is not "?", the output SHALL contain a `: ` type annotation
//
// **Validates: Requirements 24.1–24.5**
func TestFieldAccessLineSchema(t *testing.T) {
	// Set up a recording sink to capture emitFieldAccess output
	sink := &recordingSink{}
	jniLogger.AddSink(sink)

	rapid.Check(t, func(t *rapid.T) {
		// Clear previous events
		sink.events = sink.events[:0]

		// Generate field access parameters
		nameIdx := rapid.IntRange(0, len(jniFieldAccessNames)-1).Draw(t, "fieldNameIdx")
		jniName := jniFieldAccessNames[nameIdx]

		// Generate receiver: for static fields, use a class-like value (Str = class name);
		// for instance fields, use an object value.
		isStatic := strings.Contains(jniName, "Static")
		var receiver JNIValue
		if isStatic {
			// Static: receiver.Str is the class name (used for display)
			className := genDottedClassName(t)
			receiver = JNIValue{Kind: KindClass, Str: className}
		} else {
			// Instance: receiver is an object
			receiver = genJNIValueOfKind(t, KindObject)
		}

		// Generate field name: either a valid identifier or "?" (unresolved)
		useUnresolved := rapid.IntRange(0, 9).Draw(t, "unresolvedChance")
		var fieldName string
		if useUnresolved == 0 {
			fieldName = "?"
		} else {
			fieldName = genJavaIdentifier(t)
		}

		// Generate field value (any non-void, non-null kind to ensure type annotation is meaningful)
		valueKinds := []JNIKind{
			KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong,
			KindFloat, KindDouble, KindString, KindClass, KindObject, KindArray, KindPointer,
		}
		valueKindIdx := rapid.IntRange(0, len(valueKinds)-1).Draw(t, "valueKindIdx")
		value := genJNIValueOfKind(t, valueKinds[valueKindIdx])

		// Generate caller string
		caller := ""
		hasCaller := rapid.Bool().Draw(t, "hasCaller")
		if hasCaller {
			lib := rapid.StringMatching(`[a-z]{3,8}\.so`).Draw(t, "lib")
			sym := genJavaIdentifier(t)
			offset := rapid.IntRange(0, 0xFFFF).Draw(t, "offset")
			caller = fmt.Sprintf("%s!%s+0x%x", lib, sym, offset)
		}

		// Call emitFieldAccess
		emitFieldAccess(0, jniName, receiver, fieldName, value, caller)

		// Get the captured output
		if len(sink.events) == 0 {
			t.Fatalf("emitFieldAccess produced no output for jniName=%q, fieldName=%q", jniName, fieldName)
		}
		output := sink.events[len(sink.events)-1].line

		// Strip ANSI for structural checks
		plain := stripANSI(output)

		// (a) If JNI name contains "Static", output does NOT contain "this="
		if isStatic {
			if strings.Contains(plain, "this=") {
				t.Fatalf("(a) Static field access %q should NOT contain 'this=' in output.\nPlain: %q",
					jniName, plain)
			}
		}

		// (b) If JNI name starts with "Set", output contains "←"
		if strings.HasPrefix(jniName, "Set") {
			if !strings.Contains(output, "←") {
				t.Fatalf("(b) Set field access %q should contain '←' arrow.\nOutput: %q\nPlain: %q",
					jniName, output, plain)
			}
		}

		// (c) If JNI name starts with "Get", output contains "→"
		if strings.HasPrefix(jniName, "Get") {
			if !strings.Contains(output, "→") {
				t.Fatalf("(c) Get field access %q should contain '→' arrow.\nOutput: %q\nPlain: %q",
					jniName, output, plain)
			}
		}

		// (d) If field name is not "?", output contains ": " (type annotation)
		if fieldName != "?" {
			if !strings.Contains(plain, ": ") {
				t.Fatalf("(d) Field access with known field name %q should contain ': ' type annotation.\nPlain: %q",
					fieldName, plain)
			}
		}
	})
}

// ============================================================================
// Property 4: Single-Line Log Output Invariant
// ============================================================================

// TestSingleLineLogOutput verifies Property 4: Single-Line Log Output Invariant.
// For any valid callFrame and JNIValue result, emitCallFull() output contains zero
// embedded \n characters.
//
// **Validates: Requirements 4.1**
func TestSingleLineLogOutput(t *testing.T) {
	// Set up a recording sink to capture emitCallFull output
	sink := &recordingSink{}
	jniLogger.AddSink(sink)

	rapid.Check(t, func(t *rapid.T) {
		// Clear previous events
		sink.events = sink.events[:0]

		// Generate a random call frame
		frame := genCallFrame(t)

		// Generate a random result value
		result := genJNIValue(t)

		// Call emitCallFull with the generated frame and result
		emitCallFull(0, frame, result, 0)

		// emitCallFull may suppress output if configSignatureBlacklisted matches,
		// but with default config (empty regex list) it should always emit.
		if len(sink.events) == 0 {
			// If no output was produced, the call was filtered — this is acceptable
			// but we can't verify the property. Skip this iteration.
			return
		}

		// Get the captured output line
		line := sink.events[len(sink.events)-1].line

		// Assert: the output line contains zero embedded newline characters
		if strings.Contains(line, "\n") {
			t.Fatalf("emitCallFull produced output with embedded newline.\nFrame: jniName=%q, className=%q, methodName=%q\nResult Kind: %d\nOutput: %q",
				frame.jniName, frame.className, frame.methodName, result.Kind, line)
		}
	})
}

// ============================================================================
// Unit Tests: Generic Object Formatting Edge Cases (Task 10.4)
// ============================================================================

// TestGenericObjectFormatting verifies the edge cases of formatObject for
// generic (non-special-cased) Java objects.
//
// **Validates: Requirements 7.6, 7.7, 7.8, 7.9**
func TestGenericObjectFormatting(t *testing.T) {
	formatter := lineFormatter{colorEnabled: true}
	formatterNoColor := lineFormatter{colorEnabled: false}

	t.Run("empty_toString_renders_class_name_only", func(t *testing.T) {
		// Requirement 7.9: If toString is empty, render only the formatted class name
		// without parenthesized content.
		v := JNIValue{Kind: KindObject, Str: "com.example.Foo", Extra: ""}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// Should contain the class name
		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name 'com.example.Foo' in output, got: %q", plain)
		}
		// Should NOT contain parenthesized content like ("...")
		if strings.Contains(plain, `("`) || strings.Contains(plain, `")`) {
			t.Fatalf("empty toString should not produce parenthesized content, got: %q", plain)
		}
		// Class name should be in Cyan
		if !strings.Contains(output, ansiCyan) {
			t.Fatalf("expected Cyan ANSI code for class name, got: %q", output)
		}
		// No-color mode should produce no ANSI
		noColorOutput := formatterNoColor.formatJNIValue(v)
		if strings.Contains(noColorOutput, "\x1b[") {
			t.Fatalf("no-color output contains ANSI escapes: %q", noColorOutput)
		}
	})

	t.Run("toString_equals_class_name_renders_class_only", func(t *testing.T) {
		// Requirement 7.9: If toString equals the class name, render only the
		// formatted class name without parenthesized content.
		v := JNIValue{Kind: KindObject, Str: "com.example.Foo", Extra: "com.example.Foo"}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// Should contain the class name
		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name 'com.example.Foo' in output, got: %q", plain)
		}
		// Should NOT contain parenthesized content
		if strings.Contains(plain, `("`) || strings.Contains(plain, `")`) {
			t.Fatalf("toString==className should not produce parenthesized content, got: %q", plain)
		}
		// Should render exactly the same as empty toString case
		emptyExtra := formatter.formatJNIValue(JNIValue{Kind: KindObject, Str: "com.example.Foo", Extra: ""})
		if output != emptyExtra {
			t.Fatalf("toString==className should render same as empty toString.\n  got:  %q\n  want: %q", output, emptyExtra)
		}
	})

	t.Run("toString_starts_with_className_at_hex", func(t *testing.T) {
		// Requirement 7.6: When toString starts with className@hex, render
		// class name in Cyan and @hexhash suffix in Gray.
		v := JNIValue{Kind: KindObject, Str: "com.example.Foo", Extra: "com.example.Foo@1a2b3c"}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// Plain text should contain the full identity string
		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}
		if !strings.Contains(plain, "@1a2b3c") {
			t.Fatalf("expected @hex suffix in output, got: %q", plain)
		}

		// The class name portion should be in Cyan
		if !strings.Contains(output, ansiCyan) {
			t.Fatalf("expected Cyan ANSI code for class name, got: %q", output)
		}
		// The @hex suffix should be in Gray
		if !strings.Contains(output, ansiGray+"@1a2b3c"+ansiReset) {
			t.Fatalf("expected '@1a2b3c' in Gray, got: %q", output)
		}
		// Should NOT contain parenthesized content (this is the identity format, not generic)
		if strings.Contains(plain, `("`) {
			t.Fatalf("className@hex format should not produce parenthesized content, got: %q", plain)
		}
	})

	t.Run("toString_contains_simple_name_case_insensitive", func(t *testing.T) {
		// Requirement 7.7: When toString contains the simple class name
		// (case-insensitive), highlight the class name portion in Cyan.
		v := JNIValue{Kind: KindObject, Str: "com.example.MyClass", Extra: "MyClass[data=123]"}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// Plain text should contain the toString content
		if !strings.Contains(plain, "MyClass[data=123]") {
			t.Fatalf("expected toString content in output, got: %q", plain)
		}

		// The simple name "MyClass" should be highlighted in Cyan
		if !strings.Contains(output, ansiCyan+"MyClass"+ansiReset) {
			t.Fatalf("expected 'MyClass' highlighted in Cyan, got: %q", output)
		}

		// The remaining text should be in Subtle/Gray
		if !strings.Contains(output, ansiSubtle) {
			t.Fatalf("expected Subtle ANSI code for surrounding text, got: %q", output)
		}
	})

	t.Run("toString_contains_simple_name_different_case", func(t *testing.T) {
		// Requirement 7.7: Case-insensitive match — toString has different casing
		// than the simple class name.
		v := JNIValue{Kind: KindObject, Str: "com.example.MyWidget", Extra: "mywidget is active"}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// Plain text should contain the toString content
		if !strings.Contains(plain, "mywidget is active") {
			t.Fatalf("expected toString content in output, got: %q", plain)
		}

		// The matched portion "mywidget" (from toString) should be highlighted in Cyan
		// Note: highlightClassInString uses the original case from toString
		if !strings.Contains(output, ansiCyan+"mywidget"+ansiReset) {
			t.Fatalf("expected 'mywidget' (case-insensitive match) highlighted in Cyan, got: %q", output)
		}
	})

	t.Run("toString_exceeds_120_chars_is_truncated", func(t *testing.T) {
		// Requirement 7.8: When no custom parser matches, render as
		// ClassName("toString content") with toString truncated to 120 chars.
		longStr := strings.Repeat("x", 200)
		v := JNIValue{Kind: KindObject, Str: "com.example.Foo", Extra: longStr}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// The output should contain the class name
		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}

		// The toString content should be truncated — should NOT contain the full 200-char string
		if strings.Contains(plain, longStr) {
			t.Fatalf("expected toString to be truncated, but full 200-char string found in output")
		}

		// Should contain exactly 120 'x' characters (the truncated portion)
		truncated := strings.Repeat("x", 120)
		if !strings.Contains(plain, truncated) {
			t.Fatalf("expected truncated content (120 chars) in output, got: %q", plain)
		}

		// Should contain the ellipsis indicating truncation
		if !strings.Contains(plain, "…") {
			t.Fatalf("expected '…' truncation indicator in output, got: %q", plain)
		}

		// Should be rendered in the generic ClassName("content") format
		if !strings.Contains(plain, `("`) || !strings.Contains(plain, `")`) {
			t.Fatalf("expected generic ClassName(\"...\") format, got: %q", plain)
		}

		// Content should be in Yellow
		if !strings.Contains(output, ansiYellow) {
			t.Fatalf("expected Yellow ANSI code for toString content, got: %q", output)
		}
	})

	t.Run("toString_exactly_120_chars_not_truncated", func(t *testing.T) {
		// Requirement 7.8: toString at exactly 120 chars should NOT be truncated.
		exactStr := strings.Repeat("y", 120)
		v := JNIValue{Kind: KindObject, Str: "com.example.Bar", Extra: exactStr}
		output := formatter.formatJNIValue(v)
		plain := stripANSI(output)

		// Should contain the full 120-char string without ellipsis
		if !strings.Contains(plain, exactStr) {
			t.Fatalf("expected full 120-char string in output, got: %q", plain)
		}
		if strings.Contains(plain, "…") {
			t.Fatalf("120-char toString should NOT be truncated, got: %q", plain)
		}
	})
}

// ============================================================================
// Property 5: Method Call Line Schema
// Feature: jni-call-logger, Property 5: Method Call Line Schema
// ============================================================================

// TestMethodCallLineSchema verifies Property 5: Method Call Line Schema.
// For any valid callFrame and result JNIValue:
// (a) the formatted output SHALL begin with a dim-styled [jniName] tag
// (b) if the receiver is non-null/non-void, the output SHALL contain "this="
// (c) if the result Kind is not KindVoid, the output SHALL contain the "→" arrow
// (d) if the result Kind is KindVoid, the output SHALL NOT contain the "→" arrow
//
// **Validates: Requirements 4.2, 4.6, 4.7**
func TestMethodCallLineSchema(t *testing.T) {
	// Set up a recording sink to capture emitCallFull output
	sink := &recordingSink{}
	jniLogger.AddSink(sink)

	rapid.Check(t, func(t *rapid.T) {
		// Clear previous events
		sink.events = sink.events[:0]

		// Generate a call frame and a result value
		frame := genCallFrame(t)
		result := genJNIValue(t)

		// Call emitCallFull to produce the log line
		emitCallFull(0, frame, result, 0)

		// Get the captured output
		if len(sink.events) == 0 {
			t.Fatalf("emitCallFull produced no output for frame.jniName=%q, result.Kind=%d",
				frame.jniName, result.Kind)
		}
		// emitCallFull writes a CALL line and (for non-void results) a separate
		// RETURN line, linked by the #id badge. Check the call line for the tag +
		// receiver and the combined output for the return arrow.
		callLine := sink.events[0].line
		var combined string
		for _, e := range sink.events {
			combined += e.line + "\n"
		}

		// (a) Call line starts with dim [jniName] tag
		// dim = \x1b[2m, so the tag should be: \x1b[2m[jniName]\x1b[0m
		expectedDimTag := "\x1b[2m[" + frame.jniName + "]\x1b[0m"
		if !strings.HasPrefix(callLine, expectedDimTag) {
			t.Fatalf("(a) call line should start with dim [%s] tag.\nExpected prefix: %q\nActual: %q",
				frame.jniName, expectedDimTag, callLine[:min(len(callLine), len(expectedDimTag)+20)])
		}

		// Strip ANSI for structural checks on this= (call line) and arrow (combined)
		plainCall := stripANSI(callLine)

		// (b) If receiver is non-null and non-void, the call line contains "this="
		if frame.receiver.Kind != KindNull && frame.receiver.Kind != KindVoid {
			if !strings.Contains(plainCall, "this=") {
				t.Fatalf("(b) non-null/non-void receiver (Kind=%d) should produce 'this=' on the call line.\nPlain: %q",
					frame.receiver.Kind, plainCall)
			}
		}

		// (c) If result Kind is not KindVoid, the output contains "→" (return line)
		if result.Kind != KindVoid {
			if !strings.Contains(combined, "→") {
				t.Fatalf("(c) non-void result (Kind=%d) should produce '→' in output.\nCombined: %q",
					result.Kind, combined)
			}
		}

		// (d) If result Kind is KindVoid, output does NOT contain "→"
		if result.Kind == KindVoid {
			if strings.Contains(combined, "→") {
				t.Fatalf("(d) void result should NOT produce '→' in output.\nCombined: %q", combined)
			}
		}
	})
}

// ============================================================================
// Property 8: Three-Tier Gate Filtering Correctness
// Feature: jni-call-logger, Property 8: Three-Tier Gate Filtering Correctness
// ============================================================================

// TestThreeTierGateFiltering verifies Property 8: Three-Tier Gate Filtering Correctness.
// For any configuration (with arbitrary functions/categories whitelist and
// exclude.functions/categories blacklist) and any JNI function name:
// (a) if the name appears in the expanded blacklist, configFunctionBlacklisted
//
//	SHALL return true regardless of whitelist membership,
//
// (b) if the name does not appear in the expanded blacklist and the whitelist is
//
//	non-empty and the name does not appear in the expanded whitelist,
//	configFunctionEnabled SHALL return false,
//
// (c) if both whitelist arrays are empty, configFunctionEnabled SHALL return true
//
//	for all names.
//
// **Validates: Requirements 9.4–9.7, 9.12, 10.1–10.5**
func TestThreeTierGateFiltering(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		// Generate a random configuration
		c := genConfig(t)

		// Install the config globally via the atomic.Pointer swap.
		// (No mutex needed — readers do a single atomic load.)
		cfg.Store(&c)
		defer cfg.Store(emptyConfig)

		// Generate a JNI function name — either from the known set or arbitrary
		useKnown := rapid.Bool().Draw(t, "useKnownName")
		var name string
		if useKnown {
			idx := rapid.IntRange(0, len(jniMethodNames)-1).Draw(t, "nameIdx")
			name = jniMethodNames[idx]
		} else {
			name = rapid.StringMatching(`[A-Z][a-zA-Z]{2,20}`).Draw(t, "arbitraryName")
		}

		// Derive the EXPECTED gate behaviour INDEPENDENTLY from the raw config
		// fields (Functions/Categories/Exclude) + the category-expansion map —
		// NOT from c.enabledSet/c.blacklistSet, which are the very maps the gate
		// reads. The old test consulted those derived sets, so it was tautological
		// (it could not catch a bug in buildEnabledSet/buildBlacklistSet). This
		// independent expansion also exercises set construction. (F5 test nit)
		expandedWhitelist := map[string]bool{}
		for _, fn := range c.Functions {
			expandedWhitelist[fn] = true
		}
		for _, cat := range c.Categories {
			for _, m := range categories[cat] {
				expandedWhitelist[m] = true
			}
		}
		whitelistEmpty := len(c.Functions) == 0 && len(c.Categories) == 0
		isWhitelisted := expandedWhitelist[name]

		expandedBlacklist := map[string]bool{}
		for _, fn := range c.Exclude.Functions {
			expandedBlacklist[fn] = true
		}
		for _, cat := range c.Exclude.Categories {
			for _, m := range categories[cat] {
				expandedBlacklist[m] = true
			}
		}
		isBlacklisted := expandedBlacklist[name]

		// The gate functions under test are the SAME impls the cgo exports call
		// (gate_shared.go) — not a test-only reimplementation (F5).

		// (a) blacklist membership ⇒ configFunctionBlacklistedImpl true, else false.
		if got := configFunctionBlacklistedImpl(name); got != isBlacklisted {
			t.Fatalf("(a) name %q: configFunctionBlacklistedImpl=%v, expected %v.\n"+
				"Exclude.Functions=%v Exclude.Categories=%v", name, got, isBlacklisted,
				c.Exclude.Functions, c.Exclude.Categories)
		}

		// (b) non-empty whitelist that doesn't include the name ⇒ not enabled.
		if !whitelistEmpty && !isWhitelisted {
			if configFunctionEnabledImpl(name) {
				t.Fatalf("(b) name %q not in non-empty whitelist but configFunctionEnabledImpl returned true.\n"+
					"Functions=%v Categories=%v", name, c.Functions, c.Categories)
			}
		}

		// (c) empty whitelist ⇒ everything enabled.
		if whitelistEmpty {
			if !configFunctionEnabledImpl(name) {
				t.Fatalf("(c) whitelist empty but configFunctionEnabledImpl returned false for %q.", name)
			}
		}

		// (d) name in the expanded whitelist ⇒ enabled.
		if !whitelistEmpty && isWhitelisted {
			if !configFunctionEnabledImpl(name) {
				t.Fatalf("(d) name %q is in the expanded whitelist but configFunctionEnabledImpl returned false.\n"+
					"Functions=%v Categories=%v", name, c.Functions, c.Categories)
			}
		}
	})
}

// ============================================================================
// Unit Tests for Custom Object Parsers (Task 11.4)
// Consolidated test covering Intent, Bundle, Uri, File, ComponentName parsers.
// **Validates: Requirements 7.1–7.9**
// ============================================================================

// TestCustomObjectParsers is a consolidated test that verifies all custom object
// parsers: Intent, Bundle, Uri, File, and ComponentName, plus generic object
// formatting edge cases.
func TestCustomObjectParsers(t *testing.T) {
	formatter := lineFormatter{colorEnabled: true}
	formatterNoColor := lineFormatter{colorEnabled: false}

	// --- Intent Parser (Requirement 7.1) ---
	t.Run("Intent/full_fields", func(t *testing.T) {
		toString := "Intent { act=android.intent.action.VIEW dat=content://contacts/people/1 cmp=com.example/.MainActivity (has extras) }"
		output := formatterNoColor.formatIntent(toString)

		if !strings.Contains(output, "act=") {
			t.Fatalf("missing 'act=' field in output: %q", output)
		}
		if !strings.Contains(output, "android.intent.action.VIEW") {
			t.Fatalf("missing act value in output: %q", output)
		}
		if !strings.Contains(output, "dat=") {
			t.Fatalf("missing 'dat=' field in output: %q", output)
		}
		if !strings.Contains(output, "content://contacts/people/1") {
			t.Fatalf("missing dat value in output: %q", output)
		}
		if !strings.Contains(output, "cmp=") {
			t.Fatalf("missing 'cmp=' field in output: %q", output)
		}
		if !strings.Contains(output, "com.example/.MainActivity") {
			t.Fatalf("missing cmp value in output: %q", output)
		}
		if !strings.Contains(output, "has extras") {
			t.Fatalf("missing 'has extras' indicator in output: %q", output)
		}
		if !strings.Contains(output, "{") || !strings.Contains(output, "}") {
			t.Fatalf("missing braces in output: %q", output)
		}
	})

	t.Run("Intent/only_act", func(t *testing.T) {
		toString := "Intent { act=android.intent.action.MAIN }"
		output := formatterNoColor.formatIntent(toString)

		if !strings.Contains(output, "act=") {
			t.Fatalf("missing 'act=' field: %q", output)
		}
		if strings.Contains(output, "dat=") || strings.Contains(output, "cmp=") {
			t.Fatalf("unexpected dat=/cmp= in single-field intent: %q", output)
		}
		if strings.Contains(output, "has extras") {
			t.Fatalf("unexpected 'has extras' in intent without extras: %q", output)
		}
	})

	t.Run("Intent/empty", func(t *testing.T) {
		toString := "Intent { }"
		output := formatterNoColor.formatIntent(toString)

		if strings.Contains(output, "act=") || strings.Contains(output, "dat=") || strings.Contains(output, "cmp=") {
			t.Fatalf("unexpected fields in empty intent: %q", output)
		}
		if !strings.Contains(output, "{") || !strings.Contains(output, "}") {
			t.Fatalf("missing braces in empty intent: %q", output)
		}
	})

	// --- Bundle Parser (Requirements 7.2, 7.3) ---
	t.Run("Bundle/empty", func(t *testing.T) {
		output := formatter.formatBundle("Bundle[{}]")
		plain := stripANSI(output)

		if !strings.Contains(plain, "empty") {
			t.Fatalf("expected 'empty' for empty bundle, got: %q", plain)
		}
		if !strings.Contains(plain, "android.os.Bundle") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}
	})

	t.Run("Bundle/non_empty", func(t *testing.T) {
		output := formatter.formatBundle("Bundle[mParcelledData.dataSize=128]")
		plain := stripANSI(output)

		if !strings.Contains(plain, "…") {
			t.Fatalf("expected '…' for non-empty bundle, got: %q", plain)
		}
		if strings.Contains(plain, "empty") {
			t.Fatalf("non-empty bundle should not contain 'empty', got: %q", plain)
		}
	})

	// --- Uri Parser (Requirement 7.4) ---
	t.Run("Uri/content_uri", func(t *testing.T) {
		output := formatter.formatObject("android.net.Uri", "content://com.example/path")
		plain := stripANSI(output)

		expectedPlain := `android.net.Uri("content://com.example/path")`
		if plain != expectedPlain {
			t.Fatalf("Uri plain mismatch:\n  got:  %q\n  want: %q", plain, expectedPlain)
		}
		if !strings.Contains(output, ansiYellow+"content://com.example/path"+ansiReset) {
			t.Fatalf("Uri content not in Yellow: %q", output)
		}
	})

	// --- File Parser (Requirement 7.4) ---
	t.Run("File/path", func(t *testing.T) {
		output := formatter.formatObject("java.io.File", "/data/local/tmp/file.txt")
		plain := stripANSI(output)

		expectedPlain := `java.io.File("/data/local/tmp/file.txt")`
		if plain != expectedPlain {
			t.Fatalf("File plain mismatch:\n  got:  %q\n  want: %q", plain, expectedPlain)
		}
		if !strings.Contains(output, ansiYellow+"/data/local/tmp/file.txt"+ansiReset) {
			t.Fatalf("File content not in Yellow: %q", output)
		}
	})

	// --- ComponentName Parser (Requirement 7.5) ---
	t.Run("ComponentName/basic", func(t *testing.T) {
		output := formatter.formatObject("android.content.ComponentName", "com.example/.MainActivity")
		plain := stripANSI(output)

		expectedPlain := `android.content.ComponentName{"com.example/.MainActivity"}`
		if plain != expectedPlain {
			t.Fatalf("ComponentName plain mismatch:\n  got:  %q\n  want: %q", plain, expectedPlain)
		}
		if !strings.Contains(output, ansiYellow+"com.example/.MainActivity"+ansiReset) {
			t.Fatalf("ComponentName content not in Yellow: %q", output)
		}
	})

	// --- Generic Object Formatting (Requirements 7.6–7.9) ---
	t.Run("Generic/empty_toString", func(t *testing.T) {
		output := formatter.formatObject("com.example.Foo", "")
		plain := stripANSI(output)

		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}
		if strings.Contains(plain, `("`) || strings.Contains(plain, `")`) {
			t.Fatalf("empty toString should not produce parenthesized content, got: %q", plain)
		}
	})

	t.Run("Generic/toString_equals_className", func(t *testing.T) {
		output := formatter.formatObject("com.example.Foo", "com.example.Foo")
		plain := stripANSI(output)

		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}
		if strings.Contains(plain, `("`) || strings.Contains(plain, `")`) {
			t.Fatalf("toString==className should not produce parenthesized content, got: %q", plain)
		}
	})

	t.Run("Generic/className_at_hex", func(t *testing.T) {
		output := formatter.formatObject("com.example.Foo", "com.example.Foo@1a2b3c")
		plain := stripANSI(output)

		if !strings.Contains(plain, "com.example.Foo") {
			t.Fatalf("expected class name in output, got: %q", plain)
		}
		if !strings.Contains(plain, "@1a2b3c") {
			t.Fatalf("expected @hex suffix in output, got: %q", plain)
		}
		if !strings.Contains(output, ansiGray+"@1a2b3c"+ansiReset) {
			t.Fatalf("expected '@1a2b3c' in Gray, got: %q", output)
		}
	})

	t.Run("Generic/toString_truncation", func(t *testing.T) {
		longStr := strings.Repeat("x", 200)
		output := formatter.formatObject("com.example.Foo", longStr)
		plain := stripANSI(output)

		if strings.Contains(plain, longStr) {
			t.Fatalf("expected toString to be truncated, but full 200-char string found")
		}
		truncated := strings.Repeat("x", 120)
		if !strings.Contains(plain, truncated) {
			t.Fatalf("expected truncated content (120 chars) in output, got: %q", plain)
		}
		if !strings.Contains(plain, "…") {
			t.Fatalf("expected '…' truncation indicator in output, got: %q", plain)
		}
	})

	// --- No-color mode verification ---
	t.Run("NoColor/all_parsers", func(t *testing.T) {
		cases := []struct {
			name    string
			class   string
			content string
		}{
			{"Uri", "android.net.Uri", "content://example"},
			{"File", "java.io.File", "/tmp/test.txt"},
			{"ComponentName", "android.content.ComponentName", "com.example/.Main"},
			{"Generic", "com.example.Foo", "some content"},
		}
		for _, tc := range cases {
			output := formatterNoColor.formatObject(tc.class, tc.content)
			if strings.Contains(output, "\x1b[") {
				t.Fatalf("%s: no-color output contains ANSI escapes: %q", tc.name, output)
			}
		}
	})
}

// ============================================================================
// Property 9: Caller Address Formatting Structure
// Feature: jni-call-logger, Property 9: Caller Address Formatting Structure
// **Validates: Requirements 11.3–11.8**
// ============================================================================

// TestCallerAddressFormatting verifies Property 9: Caller Address Formatting Structure.
// For any caller string in the format `library!symbol+0xNN`, `library!0xNN`,
// `library!symbol`, or raw `0xNN`:
// (a) if the string contains `!`, the output SHALL contain the Lavender ANSI code
//
//	for the library portion and the Pink ANSI code for the `!` separator
//
// (b) if the string contains `!` and `+`, the output SHALL contain the Blue ANSI
//
//	code for the symbol and Orange ANSI code for the offset
//
// (c) if the string starts with `0x` and contains no `!`, the output SHALL contain
//
//	the Orange ANSI code
//
// **Validates: Requirements 11.3–11.8**
func TestCallerAddressFormatting(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		// Generate one of the four caller string format variants
		variant := rapid.IntRange(0, 3).Draw(t, "variant")

		var caller string
		switch variant {
		case 0:
			// library!symbol+0xNN
			lib := rapid.StringMatching(`[a-z]{3,12}\.so`).Draw(t, "lib")
			sym := rapid.StringMatching(`[a-zA-Z_][a-zA-Z0-9_]{1,20}`).Draw(t, "sym")
			offset := rapid.IntRange(1, 0xFFFFFFF).Draw(t, "offset")
			caller = fmt.Sprintf("%s!%s+0x%x", lib, sym, offset)
		case 1:
			// library!0xNN (no symbol resolved)
			lib := rapid.StringMatching(`[a-z]{3,12}\.so`).Draw(t, "lib")
			offset := rapid.IntRange(1, 0xFFFFFFF).Draw(t, "offset")
			caller = fmt.Sprintf("%s!0x%x", lib, offset)
		case 2:
			// library!symbol (offset is zero, no suffix)
			lib := rapid.StringMatching(`[a-z]{3,12}\.so`).Draw(t, "lib")
			sym := rapid.StringMatching(`[a-zA-Z_][a-zA-Z0-9_]{1,20}`).Draw(t, "sym")
			caller = fmt.Sprintf("%s!%s", lib, sym)
		case 3:
			// raw 0xNN (no library resolved)
			addr := rapid.IntRange(1, 0xFFFFFFFFFFF).Draw(t, "addr")
			caller = fmt.Sprintf("0x%x", addr)
		}

		// Format the caller address with color enabled
		formatter := lineFormatter{colorEnabled: true}
		output := formatter.formatAddress(caller)

		// Output must be non-empty for any non-empty caller
		if output == "" {
			t.Fatalf("formatAddress produced empty output for caller %q", caller)
		}

		// (a) If the string contains `!`, the output SHALL contain the Lavender ANSI
		// code for the library portion and the Pink ANSI code for the `!` separator.
		if strings.Contains(caller, "!") {
			if !strings.Contains(output, ansiLavender) {
				t.Fatalf("(a) caller %q contains '!' but output missing Lavender ANSI code (%q).\nOutput: %q",
					caller, ansiLavender, output)
			}
			if !strings.Contains(output, ansiPink) {
				t.Fatalf("(a) caller %q contains '!' but output missing Pink ANSI code (%q).\nOutput: %q",
					caller, ansiPink, output)
			}
		}

		// (b) If the string contains `!` and `+`, the output SHALL contain the Blue
		// ANSI code for the symbol and Orange ANSI code for the offset.
		if strings.Contains(caller, "!") && strings.Contains(caller, "+") {
			if !strings.Contains(output, ansiBlue) {
				t.Fatalf("(b) caller %q contains '!' and '+' but output missing Blue ANSI code (%q).\nOutput: %q",
					caller, ansiBlue, output)
			}
			if !strings.Contains(output, ansiOrange) {
				t.Fatalf("(b) caller %q contains '!' and '+' but output missing Orange ANSI code (%q).\nOutput: %q",
					caller, ansiOrange, output)
			}
		}

		// (c) If the string starts with `0x` and contains no `!`, the output SHALL
		// contain the Orange ANSI code.
		if (strings.HasPrefix(caller, "0x") || strings.HasPrefix(caller, "0X")) && !strings.Contains(caller, "!") {
			if !strings.Contains(output, ansiOrange) {
				t.Fatalf("(c) caller %q starts with '0x' and has no '!' but output missing Orange ANSI code (%q).\nOutput: %q",
					caller, ansiOrange, output)
			}
		}
	})
}

// ============================================================================
// Unit Tests for Exception Event Formatting (Task 15.1)
// **Validates: Requirements 23.1–23.6**
// ============================================================================

// recordingSink captures log events for test assertions.
type recordingSinkExc struct {
	events []logEvent
}

func (s *recordingSinkExc) Write(event logEvent) {
	s.events = append(s.events, event)
}

// TestExceptionEventFormatting verifies that emitExceptionEvent correctly formats
// each of the 6 exception-related JNI functions per Requirement 23.
func TestExceptionEventFormatting(t *testing.T) {
	// Register a recording sink to capture output
	sink := &recordingSinkExc{}
	jniLogger.AddSink(sink)
	defer func() {
		// Remove the sink by replacing the logger
		jniLogger.mu.Lock()
		newSinks := make([]logSink, 0)
		for _, s := range jniLogger.sinks {
			if s != sink {
				newSinks = append(newSinks, s)
			}
		}
		jniLogger.sinks = newSinks
		jniLogger.mu.Unlock()
	}()

	// --- Test 1: ExceptionCheck returns true (Requirement 23.1) ---
	t.Run("ExceptionCheck/true", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "ExceptionCheck",
			caller:  "libapp.so!checkEx+0x1a",
		}
		result := JNIValue{Kind: KindBoolean, Int: 1}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("ExceptionCheck/true produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		// Must contain the function tag
		if !strings.Contains(plain, "[ExceptionCheck]") {
			t.Fatalf("missing [ExceptionCheck] tag in output: %q", plain)
		}
		// Must contain "true" (boolean result)
		if !strings.Contains(plain, "true") {
			t.Fatalf("missing 'true' in ExceptionCheck output: %q", plain)
		}
		// Must contain the arrow
		if !strings.Contains(output, "→") {
			t.Fatalf("missing arrow in ExceptionCheck output: %q", output)
		}
		// "true" must be in Magenta
		if !strings.Contains(output, ansiMagenta+"true"+ansiReset) {
			t.Fatalf("'true' not in Magenta in ExceptionCheck output: %q", output)
		}
	})

	// --- Test 2: ExceptionCheck returns false ---
	// NOTE: the C hook (hooked_ExceptionCheck) now SUPPRESSES the res==false
	// case at the producer (it was ~35% noise), so a false ExceptionCheck no
	// longer reaches the renderer in production. This test still validates the
	// renderer's boolean-false rendering, which other false-returning boolean
	// JNI calls (IsSameObject, IsInstanceOf, …) continue to exercise.
	t.Run("ExceptionCheck/false", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "ExceptionCheck",
			caller:  "libapp.so!checkEx+0x1a",
		}
		result := JNIValue{Kind: KindBoolean, Int: 0}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("ExceptionCheck/false produced no output")
		}
		output := sink.events[len(sink.events)-1].line

		if !strings.Contains(output, ansiMagenta+"false"+ansiReset) {
			t.Fatalf("'false' not in Magenta in ExceptionCheck output: %q", output)
		}
	})

	// --- Test 3: ExceptionOccurred returns non-null (Requirement 23.2) ---
	t.Run("ExceptionOccurred/non_null", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "ExceptionOccurred",
			caller:  "libapp.so!0x42",
		}
		result := JNIValue{
			Kind:  KindObject,
			Str:   "java.lang.NullPointerException",
			Extra: "Attempt to invoke virtual method on a null object reference",
		}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("ExceptionOccurred/non_null produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		// Must contain the function tag
		if !strings.Contains(plain, "[ExceptionOccurred]") {
			t.Fatalf("missing [ExceptionOccurred] tag: %q", plain)
		}
		// Exception class name must be present
		if !strings.Contains(plain, "NullPointerException") {
			t.Fatalf("missing exception class name: %q", plain)
		}
		// Exception class must be in Cyan
		if !strings.Contains(output, ansiCyan) {
			t.Fatalf("exception class not in Cyan: %q", output)
		}
		// Exception message must be in Yellow
		if !strings.Contains(output, ansiYellow) {
			t.Fatalf("exception message not in Yellow: %q", output)
		}
		if !strings.Contains(plain, "Attempt to invoke virtual method") {
			t.Fatalf("missing exception message: %q", plain)
		}
	})

	// --- Test 4: ExceptionOccurred returns null (Requirement 23.5) ---
	t.Run("ExceptionOccurred/null", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "ExceptionOccurred",
			caller:  "libapp.so!0x42",
		}
		result := JNIValue{Kind: KindNull}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("ExceptionOccurred/null produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		// Must contain "null"
		if !strings.Contains(plain, "null") {
			t.Fatalf("missing 'null' in ExceptionOccurred/null output: %q", plain)
		}
		// "null" must be in Magenta
		if !strings.Contains(output, ansiMagenta+"null"+ansiReset) {
			t.Fatalf("'null' not in Magenta: %q", output)
		}
	})

	// --- Test 5: Throw (Requirement 23.3) ---
	t.Run("Throw", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "Throw",
			args: []JNIValue{
				{Kind: KindObject, Str: "java.lang.IllegalStateException", Extra: "Invalid state"},
			},
			caller: "libapp.so!throwIt+0x10",
		}
		result := JNIValue{Kind: KindVoid}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("Throw produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		if !strings.Contains(plain, "[Throw]") {
			t.Fatalf("missing [Throw] tag: %q", plain)
		}
		// Exception class in Cyan
		if !strings.Contains(output, ansiCyan) {
			t.Fatalf("exception class not in Cyan: %q", output)
		}
		if !strings.Contains(plain, "IllegalStateException") {
			t.Fatalf("missing exception class: %q", plain)
		}
		// Message in Yellow
		if !strings.Contains(output, ansiYellow) {
			t.Fatalf("exception message not in Yellow: %q", output)
		}
		if !strings.Contains(plain, "Invalid state") {
			t.Fatalf("missing exception message: %q", plain)
		}
	})

	// --- Test 6: ThrowNew (Requirement 23.3) ---
	t.Run("ThrowNew", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "ThrowNew",
			args: []JNIValue{
				{Kind: KindClass, Str: "java.lang.RuntimeException"},
				{Kind: KindString, Str: "Something went wrong"},
			},
			caller: "libapp.so!throwNew+0x20",
		}
		result := JNIValue{Kind: KindVoid}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("ThrowNew produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		if !strings.Contains(plain, "[ThrowNew]") {
			t.Fatalf("missing [ThrowNew] tag: %q", plain)
		}
		// Exception class in Cyan
		if !strings.Contains(output, ansiCyan) {
			t.Fatalf("exception class not in Cyan: %q", output)
		}
		if !strings.Contains(plain, "RuntimeException") {
			t.Fatalf("missing exception class: %q", plain)
		}
		// Message in Yellow
		if !strings.Contains(output, ansiYellow) {
			t.Fatalf("exception message not in Yellow: %q", output)
		}
		if !strings.Contains(plain, "Something went wrong") {
			t.Fatalf("missing exception message: %q", plain)
		}
	})

	// --- Test 7: ExceptionClear (Requirement 23.4) ---
	t.Run("ExceptionClear", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "ExceptionClear",
			caller:  "libapp.so!clearEx+0x8",
		}
		result := JNIValue{Kind: KindVoid}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("ExceptionClear produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		if !strings.Contains(plain, "[ExceptionClear]") {
			t.Fatalf("missing [ExceptionClear] tag: %q", plain)
		}
		// Void suppression: should NOT contain "→" or "void"
		if strings.Contains(plain, "→") {
			t.Fatalf("ExceptionClear should suppress arrow (void suppression): %q", plain)
		}
		if strings.Contains(plain, "void") {
			t.Fatalf("ExceptionClear should suppress 'void' (void suppression): %q", plain)
		}
	})

	// --- Test 8: FatalError (Requirement 23.6) ---
	t.Run("FatalError", func(t *testing.T) {
		startIdx := len(sink.events)
		frame := &callFrame{
			jniName: "FatalError",
			args: []JNIValue{
				{Kind: KindString, Str: "JNI DETECTED ERROR IN APPLICATION"},
			},
			caller: "libapp.so!fatal+0x4",
		}
		result := JNIValue{Kind: KindVoid}
		emitCallFull(0, frame, result, 0)

		if len(sink.events) <= startIdx {
			t.Fatal("FatalError produced no output")
		}
		output := sink.events[len(sink.events)-1].line
		plain := stripANSI(output)

		if !strings.Contains(plain, "[FatalError]") {
			t.Fatalf("missing [FatalError] tag: %q", plain)
		}
		// Message must be in Red
		if !strings.Contains(output, ansiRed+"JNI DETECTED ERROR IN APPLICATION"+ansiReset) {
			t.Fatalf("FatalError message not in Red: %q", output)
		}
		// FatalError should be logged at error level
		lastEvent := sink.events[len(sink.events)-1]
		if lastEvent.level != logLevelError {
			t.Fatalf("FatalError should be logged at error level, got: %d", lastEvent.level)
		}
	})
}
