package main

import (
	"sort"
	"strings"
)

// The JNI function-name catalog used by the include/exclude function picker for
// autocomplete/hinting. Mirrors the category → function expansion in the lib
// (src/go/config.go) so suggestions match exactly what libjnilog understands.
// (JNI names are mixed-case by definition — they are real symbol names.)

var jniRefTypes = []string{
	"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double",
}
var jniPrimArrTypes = []string{
	"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double",
}

var categoryFns = buildCategoryFns()
var allFunctions = buildAllFunctions()

func buildCategoryFns() map[string][]string {
	m := map[string][]string{}

	var methods []string
	for _, t := range jniRefTypes {
		for _, suf := range []string{"Method", "MethodV", "MethodA"} {
			methods = append(methods, "Call"+t+suf, "CallStatic"+t+suf, "CallNonvirtual"+t+suf)
		}
	}
	for _, suf := range []string{"Method", "MethodV", "MethodA"} {
		methods = append(methods, "CallVoid"+suf, "CallStaticVoid"+suf, "CallNonvirtualVoid"+suf)
	}
	methods = append(methods, "NewObject", "NewObjectV", "NewObjectA")
	m["methods"] = methods

	var fields []string
	for _, t := range jniRefTypes {
		fields = append(fields, "Get"+t+"Field", "Set"+t+"Field",
			"GetStatic"+t+"Field", "SetStatic"+t+"Field")
	}
	m["fields"] = fields

	m["lookups"] = []string{
		"FindClass", "GetMethodID", "GetStaticMethodID", "GetFieldID", "GetStaticFieldID",
	}
	m["exceptions"] = []string{
		"Throw", "ThrowNew", "ExceptionCheck", "ExceptionOccurred",
		"ExceptionDescribe", "ExceptionClear", "FatalError",
	}
	m["strings"] = []string{
		"NewString", "NewStringUTF", "GetStringLength", "GetStringUTFLength",
		"GetStringChars", "GetStringUTFChars", "ReleaseStringChars", "ReleaseStringUTFChars",
		"GetStringRegion", "GetStringUTFRegion", "GetStringCritical", "ReleaseStringCritical",
	}
	m["refs"] = []string{
		"NewGlobalRef", "DeleteGlobalRef", "NewLocalRef", "DeleteLocalRef",
		"NewWeakGlobalRef", "DeleteWeakGlobalRef", "IsSameObject",
		"PushLocalFrame", "PopLocalFrame", "EnsureLocalCapacity",
	}

	var arrays []string
	for _, t := range jniPrimArrTypes {
		arrays = append(arrays, "New"+t+"Array", "Get"+t+"ArrayElements",
			"Release"+t+"ArrayElements", "Get"+t+"ArrayRegion", "Set"+t+"ArrayRegion")
	}
	arrays = append(arrays, "NewObjectArray", "GetObjectArrayElement",
		"SetObjectArrayElement", "GetArrayLength")
	m["arrays"] = arrays

	return m
}

var catalogLookup = func() map[string]bool {
	s := map[string]bool{}
	for _, f := range allFunctions {
		s[f] = true
	}
	return s
}()

// catalogHas reports whether name is a known JNI function in the catalog.
func catalogHas(name string) bool { return catalogLookup[name] }

func buildAllFunctions() []string {
	seen := map[string]bool{}
	var out []string
	for _, c := range categoryOrder {
		for _, f := range categoryFns[c] {
			if !seen[f] {
				seen[f] = true
				out = append(out, f)
			}
		}
	}
	sort.Strings(out)
	return out
}

// matchFunctions returns catalog names matching `q` (case-insensitive
// substring), prefix matches first, then the rest — each alphabetical.
func matchFunctions(q string) []string {
	q = strings.ToLower(strings.TrimSpace(q))
	if q == "" {
		return append([]string(nil), allFunctions...)
	}
	var pre, sub []string
	for _, f := range allFunctions {
		lf := strings.ToLower(f)
		if strings.HasPrefix(lf, q) {
			pre = append(pre, f)
		} else if strings.Contains(lf, q) {
			sub = append(sub, f)
		}
	}
	return append(pre, sub...)
}
