package main

import (
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
)

// key builds a tea.KeyMsg whose String() matches the navigation switch.
func key(s string) tea.KeyMsg {
	switch s {
	case "down":
		return tea.KeyMsg{Type: tea.KeyDown}
	case "up":
		return tea.KeyMsg{Type: tea.KeyUp}
	case "right":
		return tea.KeyMsg{Type: tea.KeyRight}
	case "enter":
		return tea.KeyMsg{Type: tea.KeyEnter}
	case "esc":
		return tea.KeyMsg{Type: tea.KeyEsc}
	default:
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)}
	}
}

func send(m tea.Model, keys ...string) model {
	for _, k := range keys {
		mm, _ := m.Update(key(k))
		m = mm
	}
	return m.(model)
}

func TestCategoryToggleCycle(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	// rows[1] == "fields"; move down once, then cycle off->include->exclude->off.
	got := send(m, "down")
	if got.rows[got.cursor].cat != "fields" {
		t.Fatalf("cursor not on fields, got %q", got.rows[got.cursor].cat)
	}
	got = send(got, " ") // include
	if got.cs.catState["fields"] != stateInclude {
		t.Fatalf("expected include, got %d", got.cs.catState["fields"])
	}
	if !strings.Contains(got.cs.json(), `"fields"`) {
		t.Fatalf("json missing fields include:\n%s", got.cs.json())
	}
	got = send(got, " ") // exclude
	if got.cs.catState["fields"] != stateExclude {
		t.Fatalf("expected exclude, got %d", got.cs.catState["fields"])
	}
	if !strings.Contains(got.cs.json(), `"exclude"`) {
		t.Fatalf("json missing exclude block:\n%s", got.cs.json())
	}
	got = send(got, " ") // off
	if got.cs.catState["fields"] != stateOff {
		t.Fatalf("expected off, got %d", got.cs.catState["fields"])
	}
}

func TestArrayItemsAdjust(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	// navigate down to the array_items row
	for got.rows[got.cursor].kind != rowArrayItems {
		got = send(got, "down")
	}
	start := got.cs.arrayItems // 16
	got = send(got, "right", "right", "right")
	if got.cs.arrayItems != start+3 {
		t.Fatalf("array_items = %d, want %d", got.cs.arrayItems, start+3)
	}
}

func TestPickerSelectExcludeFns(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	for got.rows[got.cursor].kind != rowExcludeFns {
		got = send(got, "down")
	}
	got = send(got, "enter") // open the picker
	if !got.picking {
		t.Fatal("expected picking mode")
	}
	got = send(got, "DeleteLocalRef") // filter (exact catalog match)
	cands := got.pickerCandidates()
	if len(cands) == 0 || cands[0].name != "DeleteLocalRef" || cands[0].add {
		t.Fatalf("expected DeleteLocalRef as top candidate, got %+v", cands)
	}
	got = send(got, " ")   // toggle the top candidate
	got = send(got, "esc") // commit + close
	if got.picking {
		t.Fatal("expected picker to close")
	}
	if len(got.cs.excludeFns) != 1 || got.cs.excludeFns[0] != "DeleteLocalRef" {
		t.Fatalf("excludeFns = %v", got.cs.excludeFns)
	}
}

func TestPickerAddCustom(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	for got.rows[got.cursor].kind != rowIncludeFns {
		got = send(got, "down")
	}
	got = send(got, "enter")          // open picker
	got = send(got, "MyCustomThing")  // not in catalog -> synthetic add entry at top
	cands := got.pickerCandidates()
	if len(cands) == 0 || !cands[0].add || cands[0].name != "MyCustomThing" {
		t.Fatalf("expected synthetic add entry, got %+v", cands)
	}
	got = send(got, "enter") // accept the add entry
	got = send(got, "esc")   // commit
	if len(got.cs.includeFns) != 1 || got.cs.includeFns[0] != "MyCustomThing" {
		t.Fatalf("includeFns = %v", got.cs.includeFns)
	}
}

// ── regex editor tests ──────────────────────────────────────────────────────

func TestRegexEditorOpenClose(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	// Navigate to the excl. regex row (last row).
	for got.rows[got.cursor].kind != rowExcludeRegex {
		got = send(got, "down")
	}
	got = send(got, "enter") // open regex editor
	if !got.regexEditing {
		t.Fatal("expected regex editing mode")
	}
	if len(got.regexList) == 0 {
		t.Fatal("regexList should have at least one slot")
	}
	// Close without changes.
	got = send(got, "esc")
	if got.regexEditing {
		t.Fatal("expected regex editor to close")
	}
	if len(got.cs.excludeRegex) != 0 {
		t.Fatalf("expected empty excludeRegex, got %v", got.cs.excludeRegex)
	}
}

func TestRegexEditorAddAndEditPattern(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	for got.rows[got.cursor].kind != rowExcludeRegex {
		got = send(got, "down")
	}
	got = send(got, "enter") // open regex editor

	// Add a new pattern via 'a' key
	got = send(got, "a") // append blank line and enter edit mode
	if got.regexLineIdx < 0 {
		t.Fatal("expected to be editing a line after pressing 'a'")
	}

	// Type the pattern character by character.
	got = send(got, "D", "e", "l", "e", "t", "e", "L", "o", "c", "a", "l", "R", "e", "f")
	got = send(got, "enter") // commit the edit

	if got.regexLineIdx >= 0 {
		t.Fatal("expected edit mode to close after enter")
	}

	// Find the pattern in the list.
	found := false
	for _, pat := range got.regexList {
		if pat == "DeleteLocalRef" {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("DeleteLocalRef not found in regexList: %v", got.regexList)
	}
}

func TestRegexEditorDeletePattern(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	for got.rows[got.cursor].kind != rowExcludeRegex {
		got = send(got, "down")
	}
	got = send(got, "enter") // open regex editor

	// Add a pattern first.
	got = send(got, "a")
	got = send(got, "T", "e", "s", "t", "P", "a", "t", "t", "e", "r", "n")
	got = send(got, "enter")

	// Add a second pattern.
	got = send(got, "a")
	got = send(got, "A", "n", "o", "t", "h", "e", "r", "P", "a", "t")
	got = send(got, "enter")

	// Delete the first pattern (cursor should be on the last line after enter).
	// Move up to the first non-empty pattern, counting empty slots.
	// The list after adding two patterns: [TestPattern, AnotherPat, ""].
	// After the last enter, cursor goes to the end (the empty slot).
	// Move up twice to get to TestPattern.
	for i := 0; i < 3; i++ {
		got = send(got, "up")
	}
	got = send(got, "d") // delete

	// Verify TestPattern was removed.
	for _, pat := range got.regexList {
		if pat == "TestPattern" {
			t.Fatal("TestPattern should have been deleted")
		}
	}
	// AnotherPat should still be there.
	found := false
	for _, pat := range got.regexList {
		if pat == "AnotherPat" {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("AnotherPat should still exist after deletion")
	}
}

func TestRegexEditorCommitToConfig(t *testing.T) {
	m := newModel("/tmp/jnilogcfg_nonexistent.json", "")
	got := m
	for got.rows[got.cursor].kind != rowExcludeRegex {
		got = send(got, "down")
	}
	got = send(got, "enter") // open regex editor

	// Add a pattern.
	got = send(got, "a")
	got = send(got, "^", "C", "a", "l", "l", ".", "*")
	got = send(got, "enter")

	// Commit via esc.
	got = send(got, "esc")

	if len(got.cs.excludeRegex) != 1 || got.cs.excludeRegex[0] != "^Call.*" {
		t.Fatalf("excludeRegex = %v, want [^Call.*]", got.cs.excludeRegex)
	}
}

// ── callKeyHint tests ─────────────────────────────────────────────────────

func TestCallKeyHint(t *testing.T) {
	tests := []struct {
		pattern string
		want    string // substring the hint should contain
	}{
		{"", "empty"},
		{"DeleteLocalRef", "fn:DeleteLocalRef"},
		{"CallIntMethod", "fn:CallIntMethod"},
		{"^Call.*", "fn:Call*"},
		{"FindClass|.*MyClass", "class/method context"},
		{"FindClass|.*MyClass", "fn:FindClass"},
		{"|.*MyClass::doSomething", "method"},
		{"|.*MyClass::doSomething\\(", "args"},
		{"\\.MyClass", "class name"},
		{"[", "invalid"}, // deliberately bad regex
		{"^Call[A-Z].*", "fn:Call[A-Z]*"},
	}

	for _, tt := range tests {
		t.Run(tt.pattern, func(t *testing.T) {
			hint := callKeyHint(tt.pattern)
			if !strings.Contains(hint, tt.want) {
				t.Errorf("callKeyHint(%q) = %q, want substring %q",
					tt.pattern, hint, tt.want)
			}
		})
	}
}

func TestCallKeyHintEdgeCases(t *testing.T) {
	// A bare pipe pattern (just |something) should show class/method context.
	hint := callKeyHint(`|.*Foo::bar`)
	if !strings.Contains(hint, "class/method context") {
		t.Errorf("expected class/method context, got %q", hint)
	}

	// Escaped dot = package name signal.
	hint = callKeyHint(`com\.example\.MyClass`)
	if !strings.Contains(hint, "class name") {
		t.Errorf("expected class name hint, got %q", hint)
	}
}

func TestEffectiveMode(t *testing.T) {
	cs := newConfigState()
	if !strings.HasPrefix(cs.effectiveMode(), "all") {
		t.Fatalf("empty include should be ALL mode, got %q", cs.effectiveMode())
	}
	cs.catState["methods"] = stateInclude
	if !strings.HasPrefix(cs.effectiveMode(), "whitelist") {
		t.Fatalf("included category should be WHITELIST, got %q", cs.effectiveMode())
	}
}
