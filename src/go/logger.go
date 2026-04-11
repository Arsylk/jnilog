package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
)

const (
	ansiReset   = "\x1b[0m"
	ansiBold    = "\x1b[1m"
	ansiDim     = "\x1b[2m"
	ansiItalic  = "\x1b[3m"
	ansiRed     = "\x1b[31m"
	ansiGreen   = "\x1b[32m"
	ansiYellow  = "\x1b[33m"
	ansiBlue    = "\x1b[34m"
	ansiMagenta = "\x1b[35m"
	ansiCyan    = "\x1b[36m"
	ansiGray    = "\x1b[90m"
	ansiHidden  = "\x1b[8m"
	// Extended palette
	ansiDarkGray = "\x1b[30;2m"
	ansiOrange   = "\x1b[38;2;250;179;135m"
	ansiLavender = "\x1b[38;2;180;190;254m"
	ansiPink     = "\x1b[38;2;245;194;231m"
	ansiSubtle   = "\x1b[38;2;108;112;134m"
	ansiMaroon   = "\x1b[38;2;235;160;172m"
	// Modifier resets
	ansiNoDim    = "\x1b[22m"
	ansiNoBold   = "\x1b[22m"
	ansiNoItalic = "\x1b[23m"
)

// ============================================================================
// Log infrastructure
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
		sink.Write(event)
	}
}

type stdoutSink struct{}

func (stdoutSink) Write(event logEvent) {
	if event.line != "" {
		fmt.Println(event.line)
	}
}

type logcatSink struct{}

func (logcatSink) Write(event logEvent) {
	if event.line != "" {
		writeLogcat(toAndroidLogPriority(event.level), event.line)
	}
}

func toAndroidLogPriority(level logLevel) int {
	switch level {
	case logLevelError:
		return 6
	case logLevelWarn:
		return 5
	default:
		return 4
	}
}

// ============================================================================
// Formatter
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

func (f lineFormatter) bold(s string) string { return f.colorize(ansiBold, s) }
func (f lineFormatter) dim(s string) string  { return f.colorize(ansiDim, s) }

func (f lineFormatter) formatPrefix(name string, color string) string {
	return "[" + f.colorize(color, name) + "]"
}

func (f lineFormatter) formatLogcatPrefix(name string, color string) string {
	return "[" + color + name + ansiReset + "]"
}

func (f lineFormatter) formatOffset(_ int) string { return "" }

func (f lineFormatter) formatArrow() string    { return f.colorize(ansiBlue, "→") }
func (f lineFormatter) formatSetArrow() string { return f.colorize(ansiBlue, "←") }

// ============================================================================
// Type-directed value formatting — the core of the redesign.
//
// formatJNIValue replaces the old formatValue(string, kind) heuristic.
// Each JNIKind maps to exactly one color/representation rule, decided
// statically at the call site — no string pattern matching at all.
// ============================================================================

func (f lineFormatter) formatJNIValue(v JNIValue) string {
	switch v.Kind {

	// ── Null / Void ──────────────────────────────────────────────────────────
	case KindNull:
		return f.colorize(ansiMagenta, "null")
	case KindVoid:
		return f.colorize(ansiGray, "void")

	// ── Boolean ──────────────────────────────────────────────────────────────
	// Bold magenta like Kotlin/Rust literals; "true"/"false" not 0/1
	case KindBoolean:
		if v.Int != 0 {
			return f.colorize(ansiMagenta, "true")
		}
		return f.colorize(ansiMagenta, "false")

	// ── Byte ─────────────────────────────────────────────────────────────────
	// Always hex; two digits — matches how bytecode tools show bytes
	case KindByte:
		return f.colorize(ansiMagenta, fmt.Sprintf("0x%02x", uint8(v.Int)))

	// ── Char ─────────────────────────────────────────────────────────────────
	// Printable ASCII → 'x', otherwise \uXXXX — yellow like string content
	case KindChar:
		r := rune(uint16(v.Int))
		if r >= 32 && r <= 126 && r != '\'' && r != '\\' {
			return f.colorize(ansiYellow, fmt.Sprintf("'%c'", r))
		}
		return f.colorize(ansiYellow, fmt.Sprintf("'\\u%04X'", r))

	// ── Short ────────────────────────────────────────────────────────────────
	case KindShort:
		return f.colorize(ansiMagenta, strconv.FormatInt(v.Int, 10))

	// ── Int ──────────────────────────────────────────────────────────────────
	// Negative values in decimal; non-negative also decimal unless looks like
	// a bitmask (top bits set and value > 0xFFFF → show hex too)
	case KindInt:
		n := int32(v.Int)
		if n >= 0 && uint32(n) > 0xFFFF && uint32(n)&0xFFFF0000 != 0 {
			return f.colorize(ansiMagenta, fmt.Sprintf("%d", n)) +
				f.colorize(ansiGray, fmt.Sprintf(" /* 0x%08x */", uint32(n)))
		}
		return f.colorize(ansiMagenta, strconv.FormatInt(int64(n), 10))

	// ── Long ─────────────────────────────────────────────────────────────────
	// Suffix L to distinguish from int; large values get hex annotation
	case KindLong:
		s := strconv.FormatInt(v.Int, 10) + "L"
		return f.colorize(ansiMagenta, s)

	// ── Float ────────────────────────────────────────────────────────────────
	// Suffix f; use shortest decimal representation
	case KindFloat:
		s := strconv.FormatFloat(v.Float, 'g', -1, 32) + "f"
		return f.colorize(ansiMagenta, s)

	// ── Double ───────────────────────────────────────────────────────────────
	case KindDouble:
		s := strconv.FormatFloat(v.Float, 'g', -1, 64)
		return f.colorize(ansiMagenta, s)

	// ── Java String ──────────────────────────────────────────────────────────
	// Always quoted yellow — never confused with a class name or number
	case KindString:
		content := v.Str
		if len(content) > 200 {
			content = content[:200] + "…"
		}
		escaped := strings.NewReplacer(`"`, `\"`, "\n", `\n`, "\r", `\r`, "\t", `\t`).Replace(content)
		return f.colorize(ansiYellow, `"`+escaped+`"`)

	// ── jclass ───────────────────────────────────────────────────────────────
	// Each component of the dotted name cyan, dots plain
	case KindClass:
		return f.formatClassName(v.Str)

	// ── jobject ──────────────────────────────────────────────────────────────
	// Several sub-cases, all decided by class name (not toString heuristics):
	case KindObject:
		if v.Str == "" {
			return f.colorize(ansiMagenta, "null")
		}
		return f.formatObject(v.Str, v.Extra)

	// ── Array ────────────────────────────────────────────────────────────────
	// Render each element individually via formatJNIValue — no post-assembly regex matching.
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
			inner += f.colorize(ansiGray, fmt.Sprintf(" +%d more", v.Int))
		}
		return f.colorize(ansiBlue, "[") + inner + f.colorize(ansiBlue, "]")

	// ── Pointer (method/field ID, raw address) ────────────────────────────────
	// Lavender hex — visually distinct from numeric primitives
	case KindPointer:
		if v.Str == "" || v.Str == "0x0" || v.Str == "0" {
			return f.colorize(ansiMagenta, "null")
		}
		return f.colorize(ansiLavender, v.Str)
	}

	return f.colorize(ansiGray, v.Str)
}

// formatObject renders a jobject whose class name and toString are known.
// Keeps special-case handling centralized rather than spread across callers.
func (f lineFormatter) formatObject(className, toString string) string {
	// Well-known Android types with structured representations
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

	// If toString contains the simple name, show toString as the primary content
	simpleName := simpleNameOf(className)
	if toString == "" || toString == className {
		return f.formatClassName(className)
	}
	// Standard Java identity toString: ClassName@hexhash — render uniformly
	if strings.HasPrefix(toString, className) && len(toString) > len(className) && toString[len(className)] == '@' {
		return f.formatClassName(className) + f.colorize(ansiGray, toString[len(className):])
	}
	if strings.Contains(strings.ToLower(toString), strings.ToLower(simpleName)) {
		// toString carries class context — highlight the class portion within it
		return f.highlightClassInString(toString, simpleName)
	}

	// Generic: ClassName("toString content")
	return f.formatClassName(className) +
		f.colorize(ansiBlue, `("`) +
		f.colorize(ansiYellow, truncate(toString, 120)) +
		f.colorize(ansiBlue, `")`)
}

// formatIntent parses the Android Intent toString format.
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

// formatBundle renders android.os.Bundle.
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

// formatArrayRepr wraps a pre-rendered array repr in colored brackets.
func (f lineFormatter) formatArrayRepr(repr string) string {
	if repr == "" || repr == "[]" {
		return f.colorize(ansiBlue, "[]")
	}
	// repr comes from C as "[elem, elem, ...]" — re-bracket in our color scheme
	inner := repr
	if strings.HasPrefix(repr, "[") && strings.HasSuffix(repr, "]") {
		inner = repr[1 : len(repr)-1]
	}
	return f.colorize(ansiBlue, "[") + f.colorize(ansiCyan, inner) + f.colorize(ansiBlue, "]")
}

// ============================================================================
// Class name formatting
// ============================================================================

// formatClassName colors each component of a dotted class name.
// All parts are cyan; dots are left uncolored for readability.
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

// normalizeClassName handles JVM array descriptors and slash-separated names.
func normalizeClassName(name string) string {
	name = strings.ReplaceAll(name, "/", ".")
	if !strings.HasPrefix(name, "[") {
		return name
	}
	// Array descriptor
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

// highlightClassInString colors the simple class name within a toString value.
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

// ============================================================================
// Structured call/field formatting — type-directed, no string re-parsing
// ============================================================================

// formatPrettyCallTyped renders a method call from its structured components.
// No string parsing; each piece is type-directed via formatJNIValue.
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

// formatFieldName renders a Java field name in magenta (distinct from methods).
func (f lineFormatter) formatFieldName(name string) string {
	if name == "" {
		return f.colorize(ansiRed, "<unknown>")
	}
	return f.colorize(ansiMagenta, name)
}

// formatID renders a jmethodID / jfieldID / jclass handle.
func (f lineFormatter) formatID(id uintptr) string {
	if id == 0 {
		return f.colorize(ansiMagenta, "null")
	}
	return f.colorize(ansiOrange, fmt.Sprintf("0x%x", id))
}

// formatMethodFieldID renders a jmethodID or jfieldID in the maroon palette.
func (f lineFormatter) formatMethodFieldID(id uintptr) string {
	if id == 0 {
		return f.colorize(ansiMagenta, "null")
	}
	return f.colorize(ansiMaroon, fmt.Sprintf("0x%x", id))
}

// formatAddress renders a caller return address in dim style.
func (f lineFormatter) formatAddress(caller string) string {
	if caller == "" {
		return ""
	}
	res := f.colorize(ansiDarkGray, "@ ")
	if strings.Contains(caller, "!") {
		parts := strings.SplitN(caller, "!", 2)
		res += f.colorize(ansiLavender, parts[0]) + f.colorize(ansiPink, "!")
		if plus := strings.SplitN(parts[1], "+", 2); len(plus) == 2 {
			res += f.colorize(ansiBlue, plus[0]) + f.colorize(ansiOrange, "+"+plus[1])
		} else {
			res += f.colorize(ansiOrange, parts[1])
		}
	} else if strings.HasPrefix(caller, "0x") {
		res += f.colorize(ansiOrange, caller)
	} else {
		res += f.colorize(ansiDarkGray, caller)
	}
	return f.colorize(ansiDim, res)
}

// ============================================================================
// Signature formatting (lookup events)
// ============================================================================

func (f lineFormatter) formatType(name string) string {
	if name == "" {
		return ""
	}
	return f.colorize(ansiCyan, name)
}

// ============================================================================
// Emit functions — one per event category, all use typed values
// ============================================================================

// emitCallFull renders a complete method call + return as one log line.
func emitCallFull(offset int, frame *callFrame, result JNIValue) {
	methodTag := f.dim("[" + frame.jniName + "]")

	var receiverStr string
	if frame.receiver.Kind != KindNull && frame.receiver.Kind != KindVoid {
		receiverStr = f.colorize(ansiGray, "this=") + f.formatJNIValue(frame.receiver)
	}

	var prettyCall string
	if strings.HasSuffix(frame.jniName, "ArrayRegion") && len(frame.args) >= 3 {
		// ArrayRegion calls: label each positional arg for readability.
		// args[0]=start (KindInt), args[1]=len (KindInt), args[2]=buf (KindArray).
		prettyCall = f.colorize(ansiBlue, "(") +
			f.colorize(ansiGray, "start=") + f.formatJNIValue(frame.args[0]) +
			f.colorize(ansiGray, ", len=") + f.formatJNIValue(frame.args[1]) +
			f.colorize(ansiGray, ", buf=") + f.formatJNIValue(frame.args[2]) +
			f.colorize(ansiBlue, ")")
	} else {
		prettyCall = f.formatPrettyCallTyped(frame.className, frame.methodName, frame.args)
	}

	resultStr := f.formatJNIValue(result)

	writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s %s %s",
		f.formatOffset(offset),
		methodTag,
		receiverStr,
		prettyCall,
		f.formatArrow(),
		resultStr,
		f.formatAddress(frame.caller),
	))
}

// emitJNILookup renders GetMethodID / FindClass / GetFieldID events.
func emitJNILookup(lookupType string, name string, sig string, classHandle uintptr, className string, caller string) {
	methodTag := f.dim("[" + lookupType + "]")

	var prettyTarget string
	var returnVal string
	if lookupType == "FindClass" {
		dotName := strings.ReplaceAll(name, "/", ".")
		prettyTarget = f.colorize(ansiLavender, `"`+dotName+`"`)
		if classHandle == 0 {
			returnVal = f.colorize(ansiMagenta, "null")
		} else {
			returnVal = f.formatClassName(dotName)
		}
	} else {
		isField := strings.Contains(lookupType, "Field")
		prettyClass := f.formatClassName(strings.ReplaceAll(className, "/", "."))

		var memberName string
		if isField {
			memberName = f.formatFieldName(name)
		} else {
			memberName = f.colorize(ansiGreen, name)
		}

		prettyTarget = prettyClass + f.colorize(ansiBlue, "::") + memberName

		if strings.Contains(lookupType, "Method") || isField {
			parsedSig := parseJNISignature(sig)
			if isField && parsedSig != "" {
				prettyTarget += " " + parsedSig
			} else {
				prettyTarget += parsedSig
			}
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

// emitRegisterNatives renders JNI RegisterNatives events.
func emitRegisterNatives(clazz uintptr, className string, methods string, caller string) {
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

// emitFieldAccess renders GetField / SetField events.
// The value is a typed JNIValue rather than a pre-formatted string.
func emitFieldAccess(offset int, name string, receiver JNIValue, fieldName string, value JNIValue, caller string) {
	methodTag := f.dim("[" + name + "]")

	var receiverStr string
	if receiver.Kind != KindNull {
		receiverStr = f.colorize(ansiGray, "this=") + f.formatJNIValue(receiver)
	}

	arrow := f.formatArrow()
	if strings.HasPrefix(name, "Set") {
		arrow = f.formatSetArrow()
	}

	writeLine(logLevelInfo, fmt.Sprintf("%s%s %s %s %s %s %s",
		f.formatOffset(offset),
		methodTag,
		receiverStr,
		f.formatFieldName(fieldName),
		arrow,
		f.formatJNIValue(value),
		f.formatAddress(caller),
	))
}

// emitInfo logs a plain informational message.
func emitInfo(msg string) {
	writeLine(logLevelInfo, msg)
}

func emitInfoStdout(msg string) {
	stdoutSink{}.Write(logEvent{
		level: logLevelInfo,
		line:  fmt.Sprintf("%s %s", "["+ansiGreen+"log"+ansiReset+"]", msg),
	})
}

// ============================================================================
// Utilities
// ============================================================================

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

// extractKV extracts the value following key in s (stopped by space or '}').
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

func truncate(s string, max int) string {
	if len(s) <= max {
		return s
	}
	return s[:max] + "…"
}

// ============================================================================
// Global singletons
// ============================================================================

var (
	f         = lineFormatter{colorEnabled: os.Getenv("NO_COLOR") == ""}
	jniLogger = newMultiOutputLogger(logcatSink{}, stdoutSink{})
)

func registerLogSink(sink logSink) { jniLogger.AddSink(sink) }

func writeLine(level logLevel, line string) {
	jniLogger.Write(logEvent{level: level, line: line})
}
