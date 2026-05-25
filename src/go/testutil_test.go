package main

import (
	"fmt"
	"testing"

	"pgregory.net/rapid"
)

// ============================================================================
// Rapid generators for JNI types used in property-based tests.
// ============================================================================

// genJNIKind generates a random JNIKind value (0–14).
func genJNIKind(t *rapid.T) JNIKind {
	return JNIKind(rapid.IntRange(0, 14).Draw(t, "kind"))
}

// genJavaIdentifier generates a valid Java-style identifier (e.g. method/field name).
func genJavaIdentifier(t *rapid.T) string {
	return rapid.StringMatching(`[a-zA-Z][a-zA-Z0-9_]{0,20}`).Draw(t, "ident")
}

// genDottedClassName generates a dotted Java class name like "com.example.Foo".
func genDottedClassName(t *rapid.T) string {
	nParts := rapid.IntRange(1, 4).Draw(t, "classParts")
	parts := make([]string, nParts)
	for i := 0; i < nParts; i++ {
		parts[i] = rapid.StringMatching(`[a-z][a-zA-Z0-9]{1,10}`).Draw(t, fmt.Sprintf("part%d", i))
	}
	return joinDots(parts)
}

func joinDots(parts []string) string {
	result := parts[0]
	for i := 1; i < len(parts); i++ {
		result += "." + parts[i]
	}
	return result
}

// genSafeString generates a string without control characters that could
// interfere with wire protocol separators (\x01, \x02, \x03, \x04).
func genSafeString(t *rapid.T) string {
	return rapid.StringMatching(`[a-zA-Z0-9 _./:@#$%^&*()+=\-]{0,50}`).Draw(t, "safeStr")
}

// genHexAddress generates a hex address string like "0x7f1234abcd".
func genHexAddress(t *rapid.T) string {
	addr := rapid.Uint64Range(1, 0xFFFFFFFFFFFF).Draw(t, "addr")
	return fmt.Sprintf("0x%x", addr)
}

// ============================================================================
// genJNIValue — generates a JNIValue covering all 15 kinds.
// ============================================================================

func genJNIValue(t *rapid.T) JNIValue {
	kind := genJNIKind(t)
	return genJNIValueOfKind(t, kind)
}

// genJNIValueOfKind generates a JNIValue for a specific kind.
func genJNIValueOfKind(t *rapid.T, kind JNIKind) JNIValue {
	switch kind {
	case KindNull:
		return JNIValue{Kind: KindNull}

	case KindVoid:
		return JNIValue{Kind: KindVoid}

	case KindBoolean:
		b := rapid.IntRange(0, 1).Draw(t, "bool")
		return JNIValue{Kind: KindBoolean, Int: int64(b)}

	case KindByte:
		b := rapid.IntRange(-128, 127).Draw(t, "byte")
		return JNIValue{Kind: KindByte, Int: int64(b)}

	case KindChar:
		// Java char is unsigned 16-bit
		c := rapid.IntRange(0, 65535).Draw(t, "char")
		return JNIValue{Kind: KindChar, Int: int64(c)}

	case KindShort:
		s := rapid.IntRange(-32768, 32767).Draw(t, "short")
		return JNIValue{Kind: KindShort, Int: int64(s)}

	case KindInt:
		n := rapid.Int32().Draw(t, "int")
		return JNIValue{Kind: KindInt, Int: int64(n)}

	case KindLong:
		n := rapid.Int64().Draw(t, "long")
		return JNIValue{Kind: KindLong, Int: n}

	case KindFloat:
		f := rapid.Float32().Draw(t, "float")
		return JNIValue{Kind: KindFloat, Float: float64(f)}

	case KindDouble:
		d := rapid.Float64().Draw(t, "double")
		return JNIValue{Kind: KindDouble, Float: d}

	case KindString:
		s := genSafeString(t)
		return JNIValue{Kind: KindString, Str: s}

	case KindClass:
		name := genDottedClassName(t)
		return JNIValue{Kind: KindClass, Str: name}

	case KindObject:
		className := genDottedClassName(t)
		extra := genSafeString(t)
		return JNIValue{Kind: KindObject, Str: className, Extra: extra}

	case KindArray:
		return genJNIArray(t)

	case KindPointer:
		addr := genHexAddress(t)
		return JNIValue{Kind: KindPointer, Str: addr}

	default:
		return JNIValue{Kind: KindNull}
	}
}

// genJNIArray generates a KindArray JNIValue with homogeneous element types.
func genJNIArray(t *rapid.T) JNIValue {
	// Element kind: any non-array, non-void kind to avoid deep recursion
	elemKinds := []JNIKind{
		KindNull, KindBoolean, KindByte, KindChar, KindShort,
		KindInt, KindLong, KindFloat, KindDouble, KindString,
		KindClass, KindObject, KindPointer,
	}
	elemKindIdx := rapid.IntRange(0, len(elemKinds)-1).Draw(t, "elemKindIdx")
	elemKind := elemKinds[elemKindIdx]

	nItems := rapid.IntRange(0, 10).Draw(t, "arrayLen")
	items := make([]JNIValue, nItems)
	for i := 0; i < nItems; i++ {
		items[i] = genJNIValueOfKind(t, elemKind)
	}

	// Overflow count ("+N more" suffix)
	overflow := int64(rapid.IntRange(0, 100).Draw(t, "overflow"))

	return JNIValue{
		Kind:  KindArray,
		Int:   overflow,
		Items: items,
	}
}

// genNonVoidNonNullJNIValue generates a JNIValue that is neither null nor void.
// Useful for receiver values in call frames.
func genNonVoidNonNullJNIValue(t *rapid.T) JNIValue {
	kinds := []JNIKind{
		KindBoolean, KindByte, KindChar, KindShort, KindInt, KindLong,
		KindFloat, KindDouble, KindString, KindClass, KindObject,
		KindArray, KindPointer,
	}
	idx := rapid.IntRange(0, len(kinds)-1).Draw(t, "nonNullKindIdx")
	return genJNIValueOfKind(t, kinds[idx])
}

// ============================================================================
// genCallFrame — generates a method call frame.
// ============================================================================

// jniMethodNames is a representative subset of JNI method call function names.
var jniMethodNames = []string{
	"CallObjectMethod", "CallObjectMethodV", "CallObjectMethodA",
	"CallBooleanMethod", "CallIntMethod", "CallLongMethod",
	"CallVoidMethod", "CallVoidMethodV", "CallVoidMethodA",
	"CallStaticObjectMethod", "CallStaticVoidMethod", "CallStaticIntMethod",
	"CallNonvirtualObjectMethod", "CallNonvirtualVoidMethod",
	"NewObject", "NewObjectV", "NewObjectA",
	"GetObjectField", "GetIntField", "GetBooleanField",
	"SetObjectField", "SetIntField", "SetBooleanField",
	"GetStaticObjectField", "SetStaticObjectField",
	"GetBooleanArrayRegion", "SetByteArrayRegion",
	"GetObjectArrayElement", "SetObjectArrayElement",
}

func genCallFrame(t *rapid.T) *callFrame {
	// Pick a JNI function name
	nameIdx := rapid.IntRange(0, len(jniMethodNames)-1).Draw(t, "jniNameIdx")
	jniName := jniMethodNames[nameIdx]

	// Class and method names
	className := genDottedClassName(t)
	methodName := genJavaIdentifier(t)

	// Receiver: either null (for static) or an object
	var receiver JNIValue
	isStatic := rapid.Bool().Draw(t, "isStatic")
	if isStatic {
		receiver = JNIValue{Kind: KindNull}
	} else {
		receiver = genJNIValueOfKind(t, KindObject)
	}

	// Arguments: 0–5 args of random kinds
	nArgs := rapid.IntRange(0, 5).Draw(t, "nArgs")
	args := make([]JNIValue, nArgs)
	for i := 0; i < nArgs; i++ {
		args[i] = genJNIValue(t)
	}

	// Caller address
	callerFormat := rapid.IntRange(0, 3).Draw(t, "callerFmt")
	var caller string
	switch callerFormat {
	case 0:
		// library!symbol+0xNN
		lib := rapid.StringMatching(`[a-z]{3,10}\.so`).Draw(t, "lib")
		sym := genJavaIdentifier(t)
		offset := rapid.IntRange(0, 0xFFFF).Draw(t, "offset")
		caller = fmt.Sprintf("%s!%s+0x%x", lib, sym, offset)
	case 1:
		// library!0xNN
		lib := rapid.StringMatching(`[a-z]{3,10}\.so`).Draw(t, "lib")
		offset := rapid.IntRange(0, 0xFFFF).Draw(t, "offset")
		caller = fmt.Sprintf("%s!0x%x", lib, offset)
	case 2:
		// raw 0xNN
		caller = genHexAddress(t)
	case 3:
		// empty
		caller = ""
	}

	return &callFrame{
		jniName:    jniName,
		mid:        uintptr(rapid.Uint64().Draw(t, "mid")),
		className:  className,
		methodName: methodName,
		receiver:   receiver,
		args:       args,
		caller:     caller,
	}
}

// ============================================================================
// genConfig — generates a Config for filtering tests.
// ============================================================================

// allCategoryNames lists all valid category names.
var allCategoryNames = []string{"methods", "fields", "exceptions", "arrays", "strings", "refs", "lookups"}

func genConfig(t *rapid.T) Config {
	// Whitelist functions (0–5 random JNI function names)
	nFuncs := rapid.IntRange(0, 5).Draw(t, "nFuncs")
	funcs := make([]string, nFuncs)
	for i := 0; i < nFuncs; i++ {
		idx := rapid.IntRange(0, len(jniMethodNames)-1).Draw(t, fmt.Sprintf("funcIdx%d", i))
		funcs[i] = jniMethodNames[idx]
	}

	// Whitelist categories (0–3 random categories)
	nCats := rapid.IntRange(0, 3).Draw(t, "nCats")
	cats := make([]string, nCats)
	for i := 0; i < nCats; i++ {
		idx := rapid.IntRange(0, len(allCategoryNames)-1).Draw(t, fmt.Sprintf("catIdx%d", i))
		cats[i] = allCategoryNames[idx]
	}

	// Exclude functions (0–3)
	nExFuncs := rapid.IntRange(0, 3).Draw(t, "nExFuncs")
	exFuncs := make([]string, nExFuncs)
	for i := 0; i < nExFuncs; i++ {
		idx := rapid.IntRange(0, len(jniMethodNames)-1).Draw(t, fmt.Sprintf("exFuncIdx%d", i))
		exFuncs[i] = jniMethodNames[idx]
	}

	// Exclude categories (0–2)
	nExCats := rapid.IntRange(0, 2).Draw(t, "nExCats")
	exCats := make([]string, nExCats)
	for i := 0; i < nExCats; i++ {
		idx := rapid.IntRange(0, len(allCategoryNames)-1).Draw(t, fmt.Sprintf("exCatIdx%d", i))
		exCats[i] = allCategoryNames[idx]
	}

	// Exclude regex patterns (0–2 simple patterns)
	nRegex := rapid.IntRange(0, 2).Draw(t, "nRegex")
	regexPats := make([]string, nRegex)
	for i := 0; i < nRegex; i++ {
		// Generate simple regex patterns that are always valid
		pat := rapid.StringMatching(`[a-zA-Z.]{1,15}`).Draw(t, fmt.Sprintf("regex%d", i))
		regexPats[i] = pat
	}

	// Array items
	arrayItems := rapid.IntRange(1, 50).Draw(t, "arrayItems")

	c := Config{
		Functions:  funcs,
		Categories: cats,
		Exclude: ExcludeRule{
			Functions:  exFuncs,
			Categories: exCats,
			Regex:      regexPats,
		},
		ArrayItems: arrayItems,
	}

	// Build the derived sets
	buildEnabledSet(&c)
	buildBlacklistSet(&c)
	buildRegexList(&c)

	return c
}

// ============================================================================
// Smoke tests to verify generators produce valid values.
// ============================================================================

func TestGenJNIValue(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		v := genJNIValue(t)
		if v.Kind > KindPointer {
			t.Fatalf("generated JNIValue with invalid kind: %d", v.Kind)
		}
	})
}

func TestGenCallFrame(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		frame := genCallFrame(t)
		if frame == nil {
			t.Fatal("genCallFrame returned nil")
		}
		if frame.jniName == "" {
			t.Fatal("genCallFrame produced empty jniName")
		}
		if frame.className == "" {
			t.Fatal("genCallFrame produced empty className")
		}
		if frame.methodName == "" {
			t.Fatal("genCallFrame produced empty methodName")
		}
	})
}

func TestGenConfig(t *testing.T) {
	rapid.Check(t, func(t *rapid.T) {
		c := genConfig(t)
		if c.ArrayItems < 1 {
			t.Fatalf("genConfig produced ArrayItems < 1: %d", c.ArrayItems)
		}
	})
}
