//go:build !android

package main

import (
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
)

// ============================================================================
// Types mirrored from android-tagged files for host-side testing.
//
// WARNING — DRIFT HAZARD:
//
// This file duplicates significant chunks of logger.go / value.go / config.go /
// signature.go under the `!android` build tag so the test suite can compile
// and run on the host without cgo. Every bug fix or behaviour change made to
// the android-tagged formatters MUST be mirrored here, otherwise the tests
// will pass against stale code and silently fail to catch regressions.
//
// Currently-mirrored behaviours (must stay in sync with logger.go):
//   - rune-aware `truncate` (no UTF-8 mid-codepoint splits)
//   - `escapeControlChars` applied at KindString rendering, formatObject's
//     toString, and exception/throw `Extra` rendering paths
//
// Longer-term, the platform-independent formatter logic should be moved to
// a non-tagged file and only the cgo bridge functions kept behind the
// android build tag — but that's a focused refactor for another day.
// ============================================================================

// callFrame holds everything captured at the JNI call site.
type callFrame struct {
	jniName    string
	mid        uintptr
	className  string
	methodName string
	receiver   JNIValue
	args       []JNIValue
	caller     string
}

// ExcludeRule mirrors the top-level include fields for blacklisting.
type ExcludeRule struct {
	Functions  []string `json:"functions"`
	Categories []string `json:"categories"`
	Regex      []string `json:"regex"`
}

// Config is the top-level configuration structure.
type Config struct {
	Functions  []string    `json:"functions"`
	Categories []string    `json:"categories"`
	Exclude    ExcludeRule `json:"exclude"`
	ArrayItems int         `json:"array_items"`

	enabledSet   map[string]bool
	blacklistSet map[string]bool
	regexList    []*regexp.Regexp
}

// ============================================================================
// ANSI color constants (mirrored from logger.go)
// ============================================================================

const (
	ansiReset   = "\x1b[0m"
	ansiDim     = "\x1b[2m"
	ansiRed     = "\x1b[31m"
	ansiGreen   = "\x1b[32m"
	ansiYellow  = "\x1b[33m"
	ansiBlue    = "\x1b[34m"
	ansiMagenta = "\x1b[35m"
	ansiCyan    = "\x1b[36m"
	ansiGray    = "\x1b[90m"
	// Extended palette
	ansiDarkGray = "\x1b[30;2m"
	ansiOrange   = "\x1b[38;2;250;179;135m"
	ansiLavender = "\x1b[38;2;180;190;254m"
	ansiPink     = "\x1b[38;2;245;194;231m"
	ansiSubtle   = "\x1b[38;2;108;112;134m"
	ansiMaroon   = "\x1b[38;2;235;160;172m"
)

// ============================================================================
// Category definitions (mirrored from config.go)
// ============================================================================

var categories = map[string][]string{
	"methods": {
		"CallObjectMethod", "CallObjectMethodV", "CallObjectMethodA",
		"CallBooleanMethod", "CallBooleanMethodV", "CallBooleanMethodA",
		"CallByteMethod", "CallByteMethodV", "CallByteMethodA",
		"CallCharMethod", "CallCharMethodV", "CallCharMethodA",
		"CallShortMethod", "CallShortMethodV", "CallShortMethodA",
		"CallIntMethod", "CallIntMethodV", "CallIntMethodA",
		"CallLongMethod", "CallLongMethodV", "CallLongMethodA",
		"CallFloatMethod", "CallFloatMethodV", "CallFloatMethodA",
		"CallDoubleMethod", "CallDoubleMethodV", "CallDoubleMethodA",
		"CallStaticObjectMethod", "CallStaticObjectMethodV", "CallStaticObjectMethodA",
		"CallStaticBooleanMethod", "CallStaticBooleanMethodV", "CallStaticBooleanMethodA",
		"CallStaticByteMethod", "CallStaticByteMethodV", "CallStaticByteMethodA",
		"CallStaticCharMethod", "CallStaticCharMethodV", "CallStaticCharMethodA",
		"CallStaticShortMethod", "CallStaticShortMethodV", "CallStaticShortMethodA",
		"CallStaticIntMethod", "CallStaticIntMethodV", "CallStaticIntMethodA",
		"CallStaticLongMethod", "CallStaticLongMethodV", "CallStaticLongMethodA",
		"CallStaticFloatMethod", "CallStaticFloatMethodV", "CallStaticFloatMethodA",
		"CallStaticDoubleMethod", "CallStaticDoubleMethodV", "CallStaticDoubleMethodA",
		"CallNonvirtualObjectMethod", "CallNonvirtualObjectMethodV", "CallNonvirtualObjectMethodA",
		"CallNonvirtualBooleanMethod", "CallNonvirtualBooleanMethodV", "CallNonvirtualBooleanMethodA",
		"CallNonvirtualByteMethod", "CallNonvirtualByteMethodV", "CallNonvirtualByteMethodA",
		"CallNonvirtualCharMethod", "CallNonvirtualCharMethodV", "CallNonvirtualCharMethodA",
		"CallNonvirtualShortMethod", "CallNonvirtualShortMethodV", "CallNonvirtualShortMethodA",
		"CallNonvirtualIntMethod", "CallNonvirtualIntMethodV", "CallNonvirtualIntMethodA",
		"CallNonvirtualLongMethod", "CallNonvirtualLongMethodV", "CallNonvirtualLongMethodA",
		"CallNonvirtualFloatMethod", "CallNonvirtualFloatMethodV", "CallNonvirtualFloatMethodA",
		"CallNonvirtualDoubleMethod", "CallNonvirtualDoubleMethodV", "CallNonvirtualDoubleMethodA",
		"CallVoidMethod", "CallVoidMethodV", "CallVoidMethodA",
		"CallStaticVoidMethod", "CallStaticVoidMethodV", "CallStaticVoidMethodA",
		"CallNonvirtualVoidMethod", "CallNonvirtualVoidMethodV", "CallNonvirtualVoidMethodA",
		"NewObject", "NewObjectV", "NewObjectA",
	},
	"fields": {
		"GetObjectField", "GetBooleanField", "GetByteField", "GetCharField",
		"GetShortField", "GetIntField", "GetLongField", "GetFloatField", "GetDoubleField",
		"SetObjectField", "SetBooleanField", "SetByteField", "SetCharField",
		"SetShortField", "SetIntField", "SetLongField", "SetFloatField", "SetDoubleField",
		"GetStaticObjectField", "GetStaticBooleanField", "GetStaticByteField", "GetStaticCharField",
		"GetStaticShortField", "GetStaticIntField", "GetStaticLongField", "GetStaticFloatField", "GetStaticDoubleField",
		"SetStaticObjectField", "SetStaticBooleanField", "SetStaticByteField", "SetStaticCharField",
		"SetStaticShortField", "SetStaticIntField", "SetStaticLongField", "SetStaticFloatField", "SetStaticDoubleField",
	},
	"exceptions": {
		"Throw", "ThrowNew",
		"ExceptionCheck", "ExceptionOccurred", "ExceptionDescribe",
		"ExceptionClear", "FatalError",
	},
	"arrays": {
		"NewBooleanArray", "NewByteArray", "NewCharArray", "NewShortArray",
		"NewIntArray", "NewLongArray", "NewFloatArray", "NewDoubleArray",
		"NewObjectArray",
		"GetBooleanArrayElements", "GetByteArrayElements", "GetCharArrayElements", "GetShortArrayElements",
		"GetIntArrayElements", "GetLongArrayElements", "GetFloatArrayElements", "GetDoubleArrayElements",
		"ReleaseBooleanArrayElements", "ReleaseByteArrayElements", "ReleaseCharArrayElements", "ReleaseShortArrayElements",
		"ReleaseIntArrayElements", "ReleaseLongArrayElements", "ReleaseFloatArrayElements", "ReleaseDoubleArrayElements",
		"GetBooleanArrayRegion", "GetByteArrayRegion", "GetCharArrayRegion", "GetShortArrayRegion",
		"GetIntArrayRegion", "GetLongArrayRegion", "GetFloatArrayRegion", "GetDoubleArrayRegion",
		"SetBooleanArrayRegion", "SetByteArrayRegion", "SetCharArrayRegion", "SetShortArrayRegion",
		"SetIntArrayRegion", "SetLongArrayRegion", "SetFloatArrayRegion", "SetDoubleArrayRegion",
		"GetObjectArrayElement", "SetObjectArrayElement",
		"GetArrayLength",
	},
	"strings": {
		"NewString", "NewStringUTF",
		"GetStringLength", "GetStringUTFLength",
		"GetStringChars", "GetStringUTFChars",
		"ReleaseStringChars", "ReleaseStringUTFChars",
		"GetStringRegion", "GetStringUTFRegion",
		"GetStringCritical", "ReleaseStringCritical",
	},
	"refs": {
		"NewGlobalRef", "DeleteGlobalRef",
		"NewLocalRef", "DeleteLocalRef",
		"NewWeakGlobalRef", "DeleteWeakGlobalRef",
		"IsSameObject",
		"PushLocalFrame", "PopLocalFrame", "EnsureLocalCapacity",
	},
	"lookups": {
		"FindClass",
		"GetMethodID", "GetStaticMethodID",
		"GetFieldID", "GetStaticFieldID",
	},
}

// ============================================================================
// Config functions (mirrored from config.go)
// ============================================================================

// MUST match config.go: a single atomic.Pointer holding an immutable *Config.
var cfg atomic.Pointer[Config]
var emptyConfig = &Config{}

func init() { cfg.Store(emptyConfig) }

func loadCfg() *Config { return cfg.Load() }

func buildEnabledSet(c *Config) {
	if len(c.Functions) == 0 && len(c.Categories) == 0 {
		return
	}
	c.enabledSet = make(map[string]bool, 256)
	for _, fn := range c.Functions {
		c.enabledSet[fn] = true
	}
	for _, cat := range c.Categories {
		for _, m := range categories[cat] {
			c.enabledSet[m] = true
		}
	}
}

func buildBlacklistSet(c *Config) {
	if len(c.Exclude.Functions) == 0 && len(c.Exclude.Categories) == 0 {
		return
	}
	c.blacklistSet = make(map[string]bool, 128)
	for _, fn := range c.Exclude.Functions {
		c.blacklistSet[fn] = true
	}
	for _, cat := range c.Exclude.Categories {
		for _, m := range categories[cat] {
			c.blacklistSet[m] = true
		}
	}
}

func buildRegexList(c *Config) {
	for _, pat := range c.Exclude.Regex {
		re, err := regexp.Compile(pat)
		if err != nil {
			continue
		}
		c.regexList = append(c.regexList, re)
	}
}

func configSignatureBlacklisted(key string) bool {
	c := loadCfg()
	if c.regexList == nil {
		return false
	}
	for _, re := range c.regexList {
		if re.MatchString(key) {
			return true
		}
	}
	return false
}

// configFunctionEnabled / configFunctionBlacklisted moved to the untagged
// gate_shared.go (configFunctionEnabledImpl / configFunctionBlacklistedImpl) so
// the host gate test and the cgo gate exports share one implementation (F5).

// ============================================================================
// Call-key builder (mirrored from config.go)
// ============================================================================

func methodTypeName(v JNIValue) string {
	switch v.Kind {
	case KindNull:
		return "null"
	case KindVoid:
		return "void"
	case KindBoolean:
		return "boolean"
	case KindByte:
		return "byte"
	case KindChar:
		return "char"
	case KindShort:
		return "short"
	case KindInt:
		return "int"
	case KindLong:
		return "long"
	case KindFloat:
		return "float"
	case KindDouble:
		return "double"
	case KindString:
		return "java.lang.String"
	case KindClass:
		return "java.lang.Class"
	case KindObject:
		if v.Str != "" {
			return v.Str
		}
		return "java.lang.Object"
	case KindArray:
		return methodArrayTypeName(v)
	case KindPointer:
		return "nativeptr"
	default:
		return "?"
	}
}

func methodArrayTypeName(v JNIValue) string {
	if len(v.Items) == 0 {
		return "[]"
	}
	return methodTypeName(v.Items[0]) + "[]"
}

func (frame *callFrame) callKey() string {
	if frame.className == "" {
		return frame.jniName
	}
	if frame.methodName == "" {
		return frame.jniName + "|" + frame.className
	}
	if frame.methodName == "<field>" {
		return frame.jniName + "|" + frame.className
	}
	var b strings.Builder
	b.WriteString(frame.jniName)
	b.WriteString("|")
	b.WriteString(frame.className)
	b.WriteString("::")
	b.WriteString(frame.methodName)
	b.WriteString("(")
	for i, arg := range frame.args {
		if i > 0 {
			b.WriteString(", ")
		}
		b.WriteString(methodTypeName(arg))
	}
	b.WriteString(")")
	return b.String()
}

func callKeyForFieldTarget(jniName, className, fieldName string, value JNIValue) string {
	return jniName + "|" + className + "." + fieldName + ": " + methodTypeName(value)
}

// ============================================================================
// Formatter (mirrored from logger.go)
// ============================================================================

type lineFormatter struct {
	colorEnabled bool
}

func (f lineFormatter) colorize(color string, s string) string {
	if !f.colorEnabled || s == "" {
		return s
	}
	return color + s + ansiReset
}

func (f lineFormatter) dim(s string) string { return f.colorize(ansiDim, s) }

func (f lineFormatter) formatOffset(_ int) string { return "" }
func (f lineFormatter) formatArrow() string       { return f.colorize(ansiBlue, "→") }
func (f lineFormatter) formatSetArrow() string    { return f.colorize(ansiBlue, "←") }

// formatJNIValue renders a typed JNI value with ANSI colorization.
func (f lineFormatter) formatJNIValue(v JNIValue) string {
	switch v.Kind {
	case KindNull:
		return f.colorize(ansiMagenta, "null")
	case KindVoid:
		return f.colorize(ansiGray, "void")
	case KindBoolean:
		if v.Int != 0 {
			return f.colorize(ansiMagenta, "true")
		}
		return f.colorize(ansiMagenta, "false")
	case KindByte:
		return f.colorize(ansiMagenta, byteHex(uint8(v.Int)))
	case KindChar:
		r := rune(uint16(v.Int))
		if r >= 32 && r <= 126 && r != '\'' && r != '\\' {
			return f.colorize(ansiYellow, "'"+string(r)+"'")
		}
		return f.colorize(ansiYellow, "'\\u"+u16Hex(uint16(v.Int))+"'")
	case KindShort:
		return f.colorize(ansiMagenta, strconv.FormatInt(v.Int, 10))
	case KindInt:
		n := int32(v.Int)
		if n >= 0 && uint32(n) > 0xFFFF && uint32(n)&0xFFFF0000 != 0 {
			return f.colorize(ansiMagenta, strconv.FormatInt(int64(n), 10)) +
				f.colorize(ansiGray, " /* 0x"+u32Hex(uint32(n))+" */")
		}
		return f.colorize(ansiMagenta, strconv.FormatInt(int64(n), 10))
	case KindLong:
		s := strconv.FormatInt(v.Int, 10) + "L"
		return f.colorize(ansiMagenta, s)
	case KindFloat:
		s := strconv.FormatFloat(v.Float, 'g', -1, 32) + "f"
		return f.colorize(ansiMagenta, s)
	case KindDouble:
		s := strconv.FormatFloat(v.Float, 'g', -1, 64)
		return f.colorize(ansiMagenta, s)
	case KindString:
		content := truncate(v.Str, 200)
		escaped := strings.ReplaceAll(escapeControlChars(content), `"`, `\"`)
		return f.colorize(ansiYellow, `"`+escaped+`"`)
	case KindClass:
		return f.formatClassName(v.Str)
	case KindObject:
		if v.Str == "" {
			return f.colorize(ansiMagenta, "null")
		}
		return f.formatObject(v.Str, v.Extra)
	case KindArray:
		if len(v.Items) == 0 {
			return f.colorize(ansiBlue, "[]")
		}
		parts := make([]string, len(v.Items))
		for i, item := range v.Items {
			parts[i] = f.formatJNIValue(item)
		}
		inner := strings.Join(parts, f.colorize(ansiGray, ", "))
		if v.Int > 0 {
			inner += f.colorize(ansiGray, " +"+strconv.FormatInt(v.Int, 10)+" more")
		}
		return f.colorize(ansiBlue, "[") + inner + f.colorize(ansiBlue, "]")
	case KindPointer:
		if v.Str == "" || v.Str == "0x0" || v.Str == "0" {
			return f.colorize(ansiMagenta, "null")
		}
		return f.colorize(ansiLavender, v.Str)
	}
	return f.colorize(ansiGray, v.Str)
}

// formatObject renders a jobject.
func (f lineFormatter) formatObject(className, toString string) string {
	switch className {
	case "android.content.Intent":
		return f.formatIntent(toString)
	case "android.os.Bundle":
		return f.formatBundle(toString)
	case "android.net.Uri", "java.io.File":
		return f.formatClassName(className) + f.colorize(ansiBlue, `("`) +
			f.colorize(ansiYellow, toString) + f.colorize(ansiBlue, `")`)
	case "android.content.ComponentName":
		return f.formatClassName(className) + f.colorize(ansiBlue, `{"`) +
			f.colorize(ansiYellow, toString) + f.colorize(ansiBlue, `"}`)
	}

	simpleName := simpleNameOf(className)
	if toString == "" || toString == className {
		return f.formatClassName(className)
	}
	safeToString := escapeControlChars(toString)
	if strings.HasPrefix(safeToString, className) && len(safeToString) > len(className) && safeToString[len(className)] == '@' {
		return f.formatClassName(className) + f.colorize(ansiGray, safeToString[len(className):])
	}
	if strings.Contains(strings.ToLower(safeToString), strings.ToLower(simpleName)) {
		return f.highlightClassInString(safeToString, simpleName)
	}
	return f.formatClassName(className) +
		f.colorize(ansiBlue, `("`) +
		f.colorize(ansiYellow, truncate(safeToString, 120)) +
		f.colorize(ansiBlue, `")`)
}

func (f lineFormatter) formatIntent(s string) string {
	res := f.formatClassName("android.content.Intent") + f.colorize(ansiBlue, "{")
	var items []string
	for _, key := range []string{"act=", "dat=", "cmp="} {
		if val := extractKV(s, key); val != "" {
			label := f.colorize(ansiSubtle, key)
			value := f.colorize(ansiYellow, `"`+val+`"`)
			items = append(items, label+value)
		}
	}
	if strings.Contains(s, "has extras") {
		items = append(items, f.colorize(ansiGray, "has extras"))
	}
	res += strings.Join(items, f.colorize(ansiGray, ", "))
	res += f.colorize(ansiBlue, "}")
	return res
}

func (f lineFormatter) formatBundle(s string) string {
	res := f.formatClassName("android.os.Bundle") + f.colorize(ansiBlue, "{")
	if strings.Contains(s, "{}") {
		res += f.colorize(ansiMagenta, "empty")
	} else if strings.Contains(s, "Bundle[") {
		res += f.colorize(ansiGray, "…")
	}
	res += f.colorize(ansiBlue, "}")
	return res
}

func (f lineFormatter) formatClassName(name string) string {
	if name == "" {
		return f.colorize(ansiRed, "<unknown>")
	}
	name = normalizeClassName(name)
	parts := strings.Split(name, ".")
	colored := make([]string, len(parts))
	for i, p := range parts {
		colored[i] = f.colorize(ansiCyan, p)
	}
	return strings.Join(colored, ".")
}

func normalizeClassName(name string) string {
	name = strings.ReplaceAll(name, "/", ".")
	if !strings.HasPrefix(name, "[") {
		return name
	}
	switch name {
	case "[Z":
		return "boolean[]"
	case "[B":
		return "byte[]"
	case "[C":
		return "char[]"
	case "[S":
		return "short[]"
	case "[I":
		return "int[]"
	case "[J":
		return "long[]"
	case "[F":
		return "float[]"
	case "[D":
		return "double[]"
	}
	if strings.HasPrefix(name, "[L") && strings.HasSuffix(name, ";") {
		return strings.TrimPrefix(strings.TrimSuffix(name, ";"), "[L") + "[]"
	}
	return name
}

func simpleNameOf(fullName string) string {
	idx := strings.LastIndexByte(fullName, '.')
	if idx < 0 {
		return fullName
	}
	return fullName[idx+1:]
}

func (f lineFormatter) highlightClassInString(s, simpleName string) string {
	lower := strings.ToLower(s)
	idx := strings.Index(lower, strings.ToLower(simpleName))
	if idx < 0 {
		return f.colorize(ansiSubtle, s)
	}
	return f.colorize(ansiSubtle, s[:idx]) +
		f.colorize(ansiCyan, s[idx:idx+len(simpleName)]) +
		f.colorize(ansiSubtle, s[idx+len(simpleName):])
}

func (f lineFormatter) formatPrettyCallTyped(className, methodName string, args []JNIValue) string {
	var sb strings.Builder
	if className != "" {
		sb.WriteString(f.formatClassName(className))
		sb.WriteString(f.colorize(ansiBlue, "::"))
		if methodName == "" {
			sb.WriteString(f.colorize(ansiRed, "<unknown>"))
		} else {
			sb.WriteString(f.colorize(ansiGreen, methodName))
		}
	}
	sb.WriteString(f.colorize(ansiBlue, "("))
	for i, arg := range args {
		if i > 0 {
			sb.WriteString(f.colorize(ansiGray, ", "))
		}
		sb.WriteString(f.formatJNIValue(arg))
	}
	sb.WriteString(f.colorize(ansiBlue, ")"))
	return sb.String()
}

func (f lineFormatter) formatFieldName(name string) string {
	if name == "" {
		return f.colorize(ansiRed, "<unknown>")
	}
	if name == "?" {
		return f.colorize(ansiRed, "<unresolved>")
	}
	return f.colorize(ansiMagenta, name)
}

func (f lineFormatter) formatID(id uintptr) string {
	if id == 0 {
		return f.colorize(ansiMagenta, "null")
	}
	return f.colorize(ansiOrange, fmt.Sprintf("0x%x", id))
}

func (f lineFormatter) formatMethodFieldID(id uintptr) string {
	if id == 0 {
		return f.colorize(ansiMagenta, "null")
	}
	return f.colorize(ansiMaroon, fmt.Sprintf("0x%x", id))
}

func (f lineFormatter) formatAddress(caller string) string {
	if caller == "" {
		return ""
	}
	res := f.colorize(ansiDarkGray, "@ ")
	if strings.Contains(caller, "!") {
		parts := strings.SplitN(caller, "!", 2)
		res += f.colorize(ansiLavender, parts[0]) + f.colorize(ansiPink, "!")
		after := parts[1]
		if plus := strings.SplitN(after, "+", 2); len(plus) == 2 {
			// library!symbol+0xNN
			res += f.colorize(ansiBlue, plus[0]) + f.colorize(ansiGray, "+") + f.colorize(ansiOrange, plus[1])
		} else if strings.HasPrefix(after, "0x") || strings.HasPrefix(after, "0X") {
			// library!0xNN (no symbol resolved)
			res += f.colorize(ansiOrange, strings.ToLower(after))
		} else {
			// library!symbol (offset is zero, no suffix)
			res += f.colorize(ansiBlue, after)
		}
	} else if plus := strings.SplitN(caller, "+0x", 2); len(plus) == 2 {
		// library+0xNN wire form → in-memory/decrypted code (packer .bss / JIT),
		// not on disk. Keep the project's "library!offset" structure (lavender
		// lib, orange offset); the "!" is YELLOW (vs the on-disk PINK "!") as the
		// at-a-glance tell that the offset is into the runtime image, not the file.
		res += f.colorize(ansiLavender, plus[0]) + f.colorize(ansiYellow, "!") + f.colorize(ansiOrange, "0x"+plus[1])
	} else if strings.HasPrefix(caller, "0x") || strings.HasPrefix(caller, "0X") {
		// Raw hex address (no library resolved)
		res += f.colorize(ansiOrange, strings.ToLower(caller))
	} else {
		res += f.colorize(ansiDarkGray, caller)
	}
	return f.colorize(ansiDim, res)
}

func (f lineFormatter) formatFieldType(v JNIValue) string {
	switch v.Kind {
	case KindBoolean:
		return f.colorize(ansiMagenta, "boolean")
	case KindByte:
		return f.colorize(ansiMagenta, "byte")
	case KindChar:
		return f.colorize(ansiMagenta, "char")
	case KindShort:
		return f.colorize(ansiMagenta, "short")
	case KindInt:
		return f.colorize(ansiMagenta, "int")
	case KindLong:
		return f.colorize(ansiMagenta, "long")
	case KindFloat:
		return f.colorize(ansiMagenta, "float")
	case KindDouble:
		return f.colorize(ansiMagenta, "double")
	case KindString:
		return f.formatClassName("java.lang.String")
	case KindClass:
		return f.formatClassName("java.lang.Class")
	case KindObject:
		if v.Str != "" {
			return f.formatClassName(v.Str)
		}
		return f.colorize(ansiGray, "?")
	case KindArray:
		return f.formatArrayType(v)
	case KindNull:
		return f.colorize(ansiGray, "?")
	default:
		return f.colorize(ansiGray, "?")
	}
}

func (f lineFormatter) formatArrayType(v JNIValue) string {
	if len(v.Items) == 0 {
		return f.colorize(ansiMagenta, "?")
	}
	elemValue := v.Items[0]
	elemName := f.formatFieldType(elemValue)
	return elemName + f.colorize(ansiMagenta, "[]")
}

func (f lineFormatter) formatType(name string) string {
	if name == "" {
		return ""
	}
	return f.colorize(ansiCyan, name)
}

// ============================================================================
// Emit functions (mirrored from logger.go)
// ============================================================================

type logLevel int

const (
	logLevelInfo logLevel = iota
	logLevelWarn
	logLevelError
)

type logEvent struct {
	level logLevel
	line  string
}

type logSink interface {
	Write(event logEvent)
}

type multiOutputLogger struct {
	mu    sync.RWMutex
	sinks []logSink
}

func newMultiOutputLogger(sinks ...logSink) *multiOutputLogger {
	l := &multiOutputLogger{}
	for _, s := range sinks {
		l.AddSink(s)
	}
	return l
}

func (l *multiOutputLogger) AddSink(sink logSink) {
	if sink == nil {
		return
	}
	l.mu.Lock()
	l.sinks = append(l.sinks, sink)
	l.mu.Unlock()
}

func (l *multiOutputLogger) Write(event logEvent) {
	l.mu.RLock()
	defer l.mu.RUnlock()
	for _, sink := range l.sinks {
		l.writeSink(sink, event)
	}
}

// writeSink delivers an event to a single sink, recovering from panics so that
// one misbehaving sink does not prevent delivery to the remaining sinks.
func (l *multiOutputLogger) writeSink(sink logSink, event logEvent) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "[jnilog] WARNING: sink panicked: %v\n", r)
		}
	}()
	sink.Write(event)
}

// Global formatter and logger for host-side testing.
var (
	hostFormatter = lineFormatter{colorEnabled: true}
	jniLogger     = newMultiOutputLogger()
)

func writeLine(level logLevel, line string) {
	jniLogger.Write(logEvent{level: level, line: line})
}

// emitExceptionEvent renders exception-related JNI events with specialized formatting.
// Handles: ExceptionCheck, ExceptionOccurred, Throw, ThrowNew, ExceptionClear, FatalError.
// Returns true if the event was handled (caller should not emit via emitCallFull).
func emitExceptionEvent(offset int, frame *callFrame, result JNIValue) bool {
	f := hostFormatter
	switch frame.jniName {
	case "ExceptionCheck":
		// Render boolean result in Magenta
		methodTag := f.dim("[" + frame.jniName + "]")
		resultStr := f.formatJNIValue(result) // KindBoolean → "true"/"false" in Magenta
		writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s",
			f.formatOffset(offset),
			methodTag,
			f.formatArrow(),
			resultStr,
			f.formatAddress(frame.caller),
		))
		return true

	case "ExceptionOccurred":
		// Non-null: exception class in Cyan, message in Yellow
		// Null: "null" in Magenta
		methodTag := f.dim("[" + frame.jniName + "]")
		if result.Kind == KindNull || (result.Kind == KindObject && result.Str == "") {
			writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s",
				f.formatOffset(offset),
				methodTag,
				f.formatArrow(),
				f.colorize(ansiMagenta, "null"),
				f.formatAddress(frame.caller),
			))
		} else {
			// result is KindObject with Str=className, Extra=toString message
			classStr := f.formatClassName(result.Str)
			var msgStr string
			if result.Extra != "" {
				msgStr = " " + f.colorize(ansiYellow, `"`+escapeControlChars(result.Extra)+`"`)
			}
			writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s%s %s",
				f.formatOffset(offset),
				methodTag,
				f.formatArrow(),
				classStr,
				msgStr,
				f.formatAddress(frame.caller),
			))
		}
		return true

	case "Throw", "ThrowNew":
		// Exception class in Cyan, message in Yellow
		methodTag := f.dim("[" + frame.jniName + "]")
		// For Throw/ThrowNew, the exception info is in the args or receiver.
		// The first arg is typically the exception class/object.
		var classStr, msgStr string
		if len(frame.args) > 0 {
			arg0 := frame.args[0]
			if arg0.Kind == KindObject || arg0.Kind == KindClass {
				classStr = f.formatClassName(arg0.Str)
				if arg0.Extra != "" {
					msgStr = " " + f.colorize(ansiYellow, `"`+escapeControlChars(arg0.Extra)+`"`)
				}
			} else {
				classStr = f.formatJNIValue(arg0)
			}
		}
		// For ThrowNew, the second arg is the message string.
		// MUST match logger.go — escapeControlChars guards against ANSI
		// injection via attacker-controlled exception messages.
		if frame.jniName == "ThrowNew" && len(frame.args) > 1 {
			arg1 := frame.args[1]
			if arg1.Kind == KindString {
				msgStr = " " + f.colorize(ansiYellow, `"`+escapeControlChars(arg1.Str)+`"`)
			}
		}
		writeLine(logLevelInfo, fmt.Sprintf("%s%s %s%s %s",
			f.formatOffset(offset),
			methodTag,
			classStr,
			msgStr,
			f.formatAddress(frame.caller),
		))
		return true

	case "ExceptionClear":
		// Void suppression applies — just emit the tag and caller
		methodTag := f.dim("[" + frame.jniName + "]")
		writeLine(logLevelInfo, fmt.Sprintf("%s%s %s",
			f.formatOffset(offset),
			methodTag,
			f.formatAddress(frame.caller),
		))
		return true

	case "FatalError":
		// Message in Red
		methodTag := f.dim("[" + frame.jniName + "]")
		var msgStr string
		if len(frame.args) > 0 && frame.args[0].Kind == KindString {
			msgStr = f.colorize(ansiRed, frame.args[0].Str)
		} else if len(frame.args) > 0 {
			msgStr = f.colorize(ansiRed, f.formatJNIValue(frame.args[0]))
		}
		writeLine(logLevelError, fmt.Sprintf("%s%s %s %s",
			f.formatOffset(offset),
			methodTag,
			msgStr,
			f.formatAddress(frame.caller),
		))
		return true
	}

	return false
}

// emitCallFull renders a complete method call + return as one log line.
func (f lineFormatter) callIDBadge(id uint64) string {
	return f.colorize(ansiSubtle, "#"+strconv.FormatUint(id, 16))
}

func emitCallFull(offset int, frame *callFrame, result JNIValue, id uint64) {
	f := hostFormatter

	if configSignatureBlacklisted(frame.callKey()) {
		return
	}

	// Exception events get specialized formatting
	if emitExceptionEvent(offset, frame, result) {
		return
	}

	methodTag := f.dim("[" + frame.jniName + "]") + " " + f.callIDBadge(id)

	var receiverStr string
	if frame.receiver.Kind != KindNull && frame.receiver.Kind != KindVoid {
		receiverStr = f.colorize(ansiGray, "this=") + f.formatJNIValue(frame.receiver)
	}

	var prettyCall string
	if strings.HasSuffix(frame.jniName, "ArrayRegion") && len(frame.args) >= 3 {
		prettyCall = f.colorize(ansiBlue, "(") +
			f.colorize(ansiGray, "start=") + f.formatJNIValue(frame.args[0]) +
			f.colorize(ansiGray, ", len=") + f.formatJNIValue(frame.args[1]) +
			f.colorize(ansiGray, ", buf=") + f.formatJNIValue(frame.args[2]) +
			f.colorize(ansiBlue, ")")
	} else if frame.jniName == "SetObjectArrayElement" && len(frame.args) >= 3 {
		prettyCall = f.colorize(ansiBlue, "(") +
			f.colorize(ansiGray, "array=") + f.formatJNIValue(frame.args[0]) +
			f.colorize(ansiGray, ", index=") + f.formatJNIValue(frame.args[1]) +
			f.colorize(ansiGray, ", value=") + f.formatJNIValue(frame.args[2]) +
			f.colorize(ansiBlue, ")")
	} else if frame.jniName == "GetObjectArrayElement" && len(frame.args) >= 2 {
		prettyCall = f.colorize(ansiBlue, "(") +
			f.colorize(ansiGray, "array=") + f.formatJNIValue(frame.args[0]) +
			f.colorize(ansiGray, ", index=") + f.formatJNIValue(frame.args[1]) +
			f.colorize(ansiBlue, ")")
	} else {
		prettyCall = f.formatPrettyCallTyped(frame.className, frame.methodName, frame.args)
	}

	// CALL line (receiver + args + caller); RETURN value goes on its own line
	// below, linked by the #id badge. Mirrors the android emitCallFull.
	writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s",
		f.formatOffset(offset),
		methodTag,
		receiverStr,
		prettyCall,
		f.formatAddress(frame.caller),
	))

	if result.Kind != KindVoid {
		writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s",
			f.formatOffset(offset),
			methodTag,
			f.formatArrow(),
			f.formatJNIValue(result),
		))
	}
}

// emitFieldAccess renders GetField / SetField events.
func emitFieldAccess(offset int, name string, receiver JNIValue, fieldName string, value JNIValue, caller string) {
	f := hostFormatter
	methodTag := f.dim("[" + name + "]")

	isStatic := strings.Contains(name, "Static")

	var targetStr string
	fieldKnown := fieldName != "?"

	if isStatic {
		className := receiver.Str
		if className == "" {
			className = "<unknown>"
		}
		targetStr = f.formatClassName(className) +
			f.colorize(ansiGray, ".") +
			f.formatFieldName(fieldName)
		if fieldKnown {
			targetStr += f.colorize(ansiGray, ": ") + f.formatFieldType(value)
		}
	} else {
		var receiverStr string
		if receiver.Kind != KindNull {
			receiverStr = f.colorize(ansiGray, "this=") + f.formatJNIValue(receiver)
		}
		targetStr = receiverStr + " " + f.formatFieldName(fieldName)
		if fieldKnown {
			targetStr += f.colorize(ansiGray, ": ") + f.formatFieldType(value)
		}
	}

	arrow := f.formatArrow()
	if strings.HasPrefix(name, "Set") {
		arrow = f.formatSetArrow()
	}

	if fieldKnown && isStatic {
		if configSignatureBlacklisted(callKeyForFieldTarget(name, receiver.Str, fieldName, value)) {
			return
		}
	}

	writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s %s",
		f.formatOffset(offset),
		methodTag,
		strings.TrimSpace(targetStr),
		arrow,
		f.formatJNIValue(value),
		f.formatAddress(caller),
	))
}

// ============================================================================
// Signature parsing (mirrored from signature.go)
// ============================================================================

func parseJNISignature(sig string) string {
	f := hostFormatter
	if sig == "" {
		return ""
	}

	if sig[0] != '(' {
		t, _, err := parseJNITypePlain(sig, 0)
		if err != nil {
			return sig
		}
		return f.formatType(t)
	}

	idx := 1
	params := make([]string, 0, 4)
	for idx < len(sig) && sig[idx] != ')' {
		param, next, err := parseJNITypePlain(sig, idx)
		if err != nil {
			return sig
		}
		params = append(params, f.formatType(param))
		idx = next
	}

	if idx >= len(sig) || sig[idx] != ')' {
		return sig
	}

	ret, _, err := parseJNITypePlain(sig, idx+1)
	if err != nil {
		return sig
	}

	return f.colorize(ansiBlue, "(") + strings.Join(params, f.colorize(ansiGray, ", ")) + f.colorize(ansiBlue, "): ") + f.formatType(ret)
}

func parseJNITypePlain(sig string, idx int) (string, int, error) {
	if idx >= len(sig) {
		return "", idx, fmt.Errorf("unexpected end of signature")
	}

	switch sig[idx] {
	case 'V':
		return "void", idx + 1, nil
	case 'Z':
		return "boolean", idx + 1, nil
	case 'B':
		return "byte", idx + 1, nil
	case 'C':
		return "char", idx + 1, nil
	case 'S':
		return "short", idx + 1, nil
	case 'I':
		return "int", idx + 1, nil
	case 'J':
		return "long", idx + 1, nil
	case 'F':
		return "float", idx + 1, nil
	case 'D':
		return "double", idx + 1, nil
	case '[':
		inner, next, err := parseJNITypePlain(sig, idx+1)
		if err != nil {
			return "", idx, err
		}
		return inner + "[]", next, nil
	case 'L':
		end := strings.IndexByte(sig[idx:], ';')
		if end < 0 {
			return "", idx, fmt.Errorf("unterminated object type")
		}
		name := sig[idx+1 : idx+end]
		name = strings.ReplaceAll(name, "/", ".")
		return name, idx + end + 1, nil
	default:
		return "", idx, fmt.Errorf("unknown type %q", sig[idx])
	}
}

// ============================================================================
// Utilities (mirrored from logger.go)
// ============================================================================

func extractKV(s, key string) string {
	idx := strings.Index(s, key)
	if idx < 0 {
		return ""
	}
	start := idx + len(key)
	end := strings.IndexAny(s[start:], " }")
	if end < 0 {
		return s[start:]
	}
	return s[start : start+end]
}

// truncate clips s to at most maxRunes Unicode code points, appending "…"
// when truncation occurs. Operates on runes (not bytes) so multi-byte UTF-8
// sequences never get split mid-rune. MUST match logger.go.
func truncate(s string, maxRunes int) string {
	if maxRunes <= 0 {
		return "…"
	}
	n := 0
	for i := range s {
		if n == maxRunes {
			return s[:i] + "…"
		}
		n++
	}
	return s
}

// escapeControlChars replaces C0/C1 control bytes with visible escapes so an
// untrusted Java string can't inject ANSI sequences. MUST match logger.go.
func escapeControlChars(s string) string {
	needs := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < 0x20 || c == 0x7f {
			needs = true
			break
		}
	}
	if !needs {
		return s
	}
	var b strings.Builder
	b.Grow(len(s) + 8)
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '\t':
			b.WriteString(`\t`)
		case c == '\n':
			b.WriteString(`\n`)
		case c == '\r':
			b.WriteString(`\r`)
		case c < 0x20 || c == 0x7f:
			fmt.Fprintf(&b, `\x%02x`, c)
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

// ============================================================================
// RegisterNatives emission (mirrored from logger.go)
// ============================================================================

// emitRegisterNatives renders JNI RegisterNatives events.
func emitRegisterNatives(clazz uintptr, className string, methods string, caller string) {
	f := hostFormatter
	methodTag := f.dim("[RegisterNatives]")
	prettyClass := f.formatClassName(strings.ReplaceAll(className, "/", "."))

	parts := splitDepth(methods, ", ")
	formatted := make([]string, len(parts))
	for i, part := range parts {
		atIdx := strings.LastIndex(part, " @")
		if atIdx < 0 {
			atIdx = strings.LastIndex(part, "@")
		}
		if atIdx >= 0 {
			nameAndSig := strings.TrimSpace(part[:atIdx])
			addr := strings.TrimSpace(part[atIdx:])
			spIdx := strings.IndexByte(nameAndSig, ' ')
			if spIdx >= 0 {
				mname := nameAndSig[:spIdx]
				sig := nameAndSig[spIdx+1:]
				formatted[i] = f.colorize(ansiGreen, mname) + parseJNISignature(sig) + " " + f.colorize(ansiMaroon, addr)
			} else {
				formatted[i] = f.colorize(ansiGreen, nameAndSig) + " " + f.colorize(ansiMaroon, addr)
			}
		} else {
			formatted[i] = f.colorize(ansiGreen, part)
		}
	}

	prettyMethods := f.colorize(ansiBlue, "{") +
		strings.Join(formatted, f.colorize(ansiGray, ", ")) +
		f.colorize(ansiBlue, "}")

	writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s %s",
		f.formatOffset(0),
		methodTag,
		prettyClass,
		prettyMethods,
		f.formatID(clazz),
		f.formatAddress(caller),
	))
}

// emitJNILookup renders GetMethodID / FindClass / GetFieldID events.
func emitJNILookup(lookupType string, name string, sig string, classHandle uintptr, className string, caller string) {
	f := hostFormatter

	// Gate 3: regex blacklist
	key := lookupType + "|" + className
	if configSignatureBlacklisted(key) {
		return
	}

	methodTag := f.dim("[" + lookupType + "]")

	var prettyTarget string
	var returnVal string
	if lookupType == "FindClass" {
		// Argument is the raw JNI descriptor with slashes ("android/content/pm/...")
		prettyTarget = f.colorize(ansiLavender, `"`+name+`"`)
		if classHandle == 0 {
			returnVal = f.colorize(ansiMagenta, "null")
		} else {
			// Return is the resolved jclass — display as dotted Java name
			dotName := strings.ReplaceAll(name, "/", ".")
			returnVal = f.formatClassName(dotName)
		}
	} else {
		isField := strings.Contains(lookupType, "Field")
		prettyClass := f.formatClassName(strings.ReplaceAll(className, "/", "."))

		parsedSig := parseJNISignature(sig)

		if isField {
			// Field: <class>.<field>: <type>  (dot separator, colon+space before type)
			memberName := f.formatFieldName(name)
			prettyTarget = prettyClass + f.colorize(ansiGray, ".") + memberName
			if parsedSig != "" {
				prettyTarget += f.colorize(ansiGray, ": ") + parsedSig
			}
		} else {
			// Method: <class>::<method>(<args>): <ret>
			memberName := f.colorize(ansiGreen, name)
			prettyTarget = prettyClass + f.colorize(ansiBlue, "::") + memberName + parsedSig
		}
		returnVal = f.formatMethodFieldID(classHandle)
	}

	writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s %s",
		f.formatOffset(0),
		methodTag,
		prettyTarget,
		f.formatArrow(),
		returnVal,
		f.formatAddress(caller),
	))
}

// splitDepth splits s by sep, respecting bracket nesting.
func splitDepth(s, sep string) []string {
	var parts []string
	depth := 0
	start := 0
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '[', '(', '{':
			depth++
		case ']', ')', '}':
			depth--
		}
		if depth == 0 && strings.HasPrefix(s[i:], sep) {
			parts = append(parts, strings.TrimSpace(s[start:i]))
			i += len(sep) - 1
			start = i + 1
		}
	}
	parts = append(parts, strings.TrimSpace(s[start:]))
	return parts
}
