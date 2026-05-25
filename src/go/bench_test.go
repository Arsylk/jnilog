package main

import (
	"strings"
	"testing"
)

// Baseline benchmarks for the hot-path formatter + wire-protocol decoder.
// These are intentionally minimal — they exist so future hot-path
// optimizations (e.g. Sprintf→strconv, strings.Builder pooling) have a
// stable measurement target. Run with: `go test -bench=. -benchmem`.

func BenchmarkFormatJNIValue_Byte(b *testing.B) {
	v := JNIValue{Kind: KindByte, Int: 0x7f}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Char_Printable(b *testing.B) {
	v := JNIValue{Kind: KindChar, Int: int64('A')}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Char_Unicode(b *testing.B) {
	v := JNIValue{Kind: KindChar, Int: 0x00ff} // non-ASCII → \uXXXX path
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Int(b *testing.B) {
	v := JNIValue{Kind: KindInt, Int: 0x4243}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Int_Bitmask(b *testing.B) {
	// Triggers the hex-annotation branch (high bits set).
	v := JNIValue{Kind: KindInt, Int: 0x12345678}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Long(b *testing.B) {
	v := JNIValue{Kind: KindLong, Int: 0x1234567890abcdef}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_String(b *testing.B) {
	v := JNIValue{Kind: KindString, Str: "Hello, world! Some longer ASCII content."}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_StringWithControl(b *testing.B) {
	// Exercises the slow path of escapeControlChars.
	v := JNIValue{Kind: KindString, Str: "line1\nline2\rline3\x1b[2J"}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Object(b *testing.B) {
	v := JNIValue{
		Kind:  KindObject,
		Str:   "java.util.HashMap$EntrySet",
		Extra: "java.util.HashMap$EntrySet@5cac94d",
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_Pointer(b *testing.B) {
	v := JNIValue{Kind: KindPointer, Str: "0x7406efa025"}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

func BenchmarkFormatJNIValue_PrimitiveArray(b *testing.B) {
	items := make([]JNIValue, 8)
	for i := range items {
		items[i] = JNIValue{Kind: KindInt, Int: int64(i)}
	}
	v := JNIValue{Kind: KindArray, Items: items}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = hostFormatter.formatJNIValue(v)
	}
}

// BenchmarkDecodeArgs exercises the wire-protocol decoder on a representative
// 4-argument record (string + int + object + array).
func BenchmarkDecodeArgs(b *testing.B) {
	enc := strings.Join([]string{
		"s\x01Hello, world!",
		"I\x0142",
		"L\x01java/util/HashMap\x03{a=1, b=2}",
		"[\x01I\x041\x042\x043",
	}, "\x02") + "\x02"
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = decodeArgs(enc)
	}
}

// BenchmarkBuildReturnValue covers the C→Go return-event entry point. Object
// returns are the hottest variant in practice.
func BenchmarkBuildReturnValue_Object(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = buildReturnValue(int(KindObject), 0xdeadbeef,
			"java.util.HashMap$EntrySet",
			"java.util.HashMap$EntrySet@5cac94d")
	}
}

func BenchmarkBuildReturnValue_Int(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = buildReturnValue(int(KindInt), 0x4243, "", "")
	}
}
