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
