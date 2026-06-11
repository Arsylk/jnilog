package main

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// ── jnilog.json schema (mirrors src/go/config.go) ─────────────────────────

// Ordered for display; matches the categories the lib understands.
var categoryOrder = []string{
	"methods", "fields", "lookups", "strings", "arrays", "refs", "exceptions",
}

// descriptions are lower-case prose; only real JNI function/type names keep
// their mixed case (Call*Method, Get*/Set*Field, FindClass, IsSameObject, …).
var categoryDesc = map[string]string{
	"methods":    "Call*Method / NewObject — args + return value",
	"fields":     "Get*/Set*Field — the field value (read/written)",
	"lookups":    "FindClass, Get*MethodID, Get*FieldID",
	"strings":    "New/Get/Release String + UTF, regions",
	"arrays":     "New/Get/Release array elements + regions",
	"refs":       "global/local/weak ref create+delete, IsSameObject",
	"exceptions": "Throw/ThrowNew, ExceptionOccurred/Check/Clear",
}

// Per-category tri-state.
const (
	stateOff     = iota // in neither list
	stateInclude        // whitelist (top-level categories)
	stateExclude        // blacklist (exclude.categories)
)

// excludeJSON / configJSON marshal to exactly the shape the lib parses.
type excludeJSON struct {
	Functions  []string `json:"functions,omitempty"`
	Categories []string `json:"categories,omitempty"`
	Regex      []string `json:"regex,omitempty"`
}

type configJSON struct {
	Functions     []string     `json:"functions,omitempty"`
	Categories    []string     `json:"categories,omitempty"`
	Exclude       *excludeJSON `json:"exclude,omitempty"`
	ArrayItems    int          `json:"array_items"`
	ClassNameOnly bool         `json:"class_name_only,omitempty"`
}

// editable, UI-friendly form of the config.
type configState struct {
	catState      map[string]int
	includeFns    []string
	excludeFns    []string
	excludeRegex  []string
	arrayItems    int
	classNameOnly bool
}

func newConfigState() configState {
	cs := configState{catState: map[string]int{}, arrayItems: 16}
	for _, c := range categoryOrder {
		cs.catState[c] = stateOff
	}
	return cs
}

// build assembles the on-disk JSON form from the editable state.
func (cs configState) build() configJSON {
	var inc, exc []string
	for _, c := range categoryOrder {
		switch cs.catState[c] {
		case stateInclude:
			inc = append(inc, c)
		case stateExclude:
			exc = append(exc, c)
		}
	}
	cj := configJSON{
		Categories:    inc,
		Functions:     nonEmpty(cs.includeFns),
		ArrayItems:    cs.arrayItems,
		ClassNameOnly: cs.classNameOnly,
	}
	ex := excludeJSON{
		Categories: exc,
		Functions:  nonEmpty(cs.excludeFns),
		Regex:      nonEmpty(cs.excludeRegex),
	}
	if len(ex.Categories)+len(ex.Functions)+len(ex.Regex) > 0 {
		cj.Exclude = &ex
	}
	return cj
}

func (cs configState) json() string {
	b, _ := json.MarshalIndent(cs.build(), "", "  ")
	return string(b)
}

// apply loads a parsed JSON config back into the editable state.
func (cs *configState) apply(cj configJSON) {
	for _, c := range categoryOrder {
		cs.catState[c] = stateOff
	}
	for _, c := range cj.Categories {
		if _, ok := cs.catState[c]; ok {
			cs.catState[c] = stateInclude
		}
	}
	if cj.Exclude != nil {
		for _, c := range cj.Exclude.Categories {
			if _, ok := cs.catState[c]; ok {
				cs.catState[c] = stateExclude
			}
		}
		cs.excludeFns = cj.Exclude.Functions
		cs.excludeRegex = cj.Exclude.Regex
	} else {
		cs.excludeFns = nil
		cs.excludeRegex = nil
	}
	cs.includeFns = cj.Functions
	cs.arrayItems = cj.ArrayItems
	if cs.arrayItems <= 0 {
		cs.arrayItems = 16
	}
	cs.classNameOnly = cj.ClassNameOnly
}

// effectiveMode describes how the lib will treat this config.
func (cs configState) effectiveMode() string {
	include := len(cs.includeFns) > 0
	for _, c := range categoryOrder {
		if cs.catState[c] == stateInclude {
			include = true
		}
	}
	if include {
		return "whitelist — only the included functions/categories are logged"
	}
	return "all functions logged, minus the excludes"
}

// ── file + device I/O ─────────────────────────────────────────────────────

func loadFile(path string) (configJSON, error) {
	var cj configJSON
	data, err := os.ReadFile(path)
	if err != nil {
		return cj, err
	}
	if err := json.Unmarshal(data, &cj); err != nil {
		return cj, err
	}
	return cj, nil
}

func saveFile(path string, cs configState) error {
	return os.WriteFile(path, []byte(cs.json()+"\n"), 0o644)
}

func adbArgs(serial string, rest ...string) []string {
	var a []string
	if serial != "" {
		a = append(a, "-s", serial)
	}
	return append(a, rest...)
}

const devicePath = "/data/local/tmp/jnilog.json"

func adbPush(serial, path string) (string, error) {
	out, err := exec.Command("adb", adbArgs(serial, "push", path, devicePath)...).CombinedOutput()
	if err != nil {
		return strings.TrimSpace(string(out)), err
	}
	// best-effort world-readable so the (uid-isolated) app can read it
	_ = exec.Command("adb", adbArgs(serial, "shell", "chmod", "644", devicePath)...).Run()
	return fmt.Sprintf("pushed -> %s on device", devicePath), nil
}

func adbPull(serial, path string) (string, error) {
	out, err := exec.Command("adb", adbArgs(serial, "pull", devicePath, path)...).CombinedOutput()
	if err != nil {
		return strings.TrimSpace(string(out)), err
	}
	return fmt.Sprintf("pulled %s -> %s", devicePath, path), nil
}

func nonEmpty(s []string) []string {
	if len(s) == 0 {
		return nil
	}
	return s
}

// parseCSV splits a comma/space separated list into trimmed, non-empty tokens.
func parseCSV(s string) []string {
	f := strings.FieldsFunc(s, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' })
	var out []string
	for _, t := range f {
		if t = strings.TrimSpace(t); t != "" {
			out = append(out, t)
		}
	}
	return out
}
