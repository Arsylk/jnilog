package main

/*
#cgo CFLAGS: -I${SRCDIR}/../cbridge
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"strings"
	"sync"
	"unsafe"
)

// ============================================================================
// Config struct — parsed from JSON file discovered at startup.
// ============================================================================

// ExcludeRule mirrors the top-level include fields for blacklisting.
type ExcludeRule struct {
	Functions  []string `json:"functions"`
	Categories []string `json:"categories"`
	Regex      []string `json:"regex"`
}

// Config is the top-level configuration structure.
// Zero values mean "use default" — everything on, array depth 16, no stack.
type Config struct {
	Functions  []string    `json:"functions"`
	Categories []string    `json:"categories"`
	Exclude    ExcludeRule `json:"exclude"`
	ArrayItems int         `json:"array_items"`
	StackDepth int         `json:"stack_depth"`

	// Populated after parsing (not in JSON):
	enabledSet   map[string]bool
	blacklistSet map[string]bool
	regexList    []*regexp.Regexp
}

var (
	cfg   Config
	cfgMu sync.RWMutex
)

// ============================================================================
// Category definitions — maps a category name to a list of JNI function names.
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
// Config discovery and parsing — called once at startup.
// ============================================================================

func loadConfig() {
	path := os.Getenv("JNILOG_CONFIG")
	if path == "" {
		path = "/data/local/tmp/jnilog.json"
	}

	data, err := os.ReadFile(path)
	if err != nil {
		logNativeInfo(fmt.Sprintf("config: no config file at %s, using defaults", path))
		return
	}

	var c Config
	if err := json.Unmarshal(data, &c); err != nil {
		logNativeWarn(fmt.Sprintf("config: failed to parse %s: %v", path, err))
		return
	}

	if c.ArrayItems <= 0 {
		c.ArrayItems = 16
	}
	if c.StackDepth < 0 {
		c.StackDepth = 0
	}

	buildEnabledSet(&c)
	buildBlacklistSet(&c)
	buildRegexList(&c)

	cfgMu.Lock()
	cfg = c
	cfgMu.Unlock()

	logNativeInfo(fmt.Sprintf("config: loaded %s (enabled=%d, blacklisted=%d, regex=%d, array=%d, stack=%d)",
		path,
		len(c.enabledSet), len(c.blacklistSet), len(c.regexList),
		c.ArrayItems, c.StackDepth))
}

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
			logNativeWarn(fmt.Sprintf("config: bad exclude regex %q: %v", pat, err))
			continue
		}
		c.regexList = append(c.regexList, re)
	}
}

// ============================================================================
// Cgo exports — queried from C hook entry points via the config cache.
// ============================================================================

//export config_function_blacklisted
func config_function_blacklisted(cName *C.char) C.int {
	name := C.GoString(cName)
	cfgMu.RLock()
	defer cfgMu.RUnlock()
	if cfg.blacklistSet == nil {
		return 0
	}
	if cfg.blacklistSet[name] {
		return 1
	}
	return 0
}

//export config_function_enabled
func config_function_enabled(cName *C.char) C.int {
	name := C.GoString(cName)
	cfgMu.RLock()
	defer cfgMu.RUnlock()
	if cfg.enabledSet == nil {
		return 1
	}
	if cfg.enabledSet[name] {
		return 1
	}
	return 0
}

//export config_array_max_items
func config_array_max_items() C.int {
	cfgMu.RLock()
	defer cfgMu.RUnlock()
	if cfg.ArrayItems > 0 {
		return C.int(cfg.ArrayItems)
	}
	return 16
}

//export config_stack_depth
func config_stack_depth() C.int {
	cfgMu.RLock()
	defer cfgMu.RUnlock()
	return C.int(cfg.StackDepth)
}

// ============================================================================
// Call-key builder — builds a regexable plain-text key for Gate 3 blacklist.
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

// callKey builds the plain-text regexable key for a callFrame.
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

// callKeyForFieldTarget builds the key for field access events from plain-text components.
func callKeyForFieldTarget(jniName, className, fieldName string, value JNIValue) string {
	return jniName + "|" + className + "." + fieldName + ": " + methodTypeName(value)
}

// configSignatureBlacklisted checks whether the given call key matches any
// exclude regex pattern.
func configSignatureBlacklisted(key string) bool {
	cfgMu.RLock()
	defer cfgMu.RUnlock()
	if cfg.regexList == nil {
		return false
	}
	for _, re := range cfg.regexList {
		if re.MatchString(key) {
			return true
		}
	}
	return false
}

// logNativeInfo / logNativeWarn — duplicates from rangeset.go for config startup.
// These are defined here with local helpers to avoid circular import concerns.
func logNativeConf(priority int, msg string) {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	goLogNative(C.int(priority), cMsg)
}
