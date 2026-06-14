// jnilogcfg — a TUI/CLI configurator for libjnilog's jnilog.json.
//
//	jnilogcfg [-f file] [-s serial] [-print]
//
// Default: opens the TUI on `file` (./jnilog.json), loading it if present.
// -print dumps the loaded/normalized config and exits (non-interactive).
package main

import (
	"flag"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// ── navigable rows ─────────────────────────────────────────────────────────

type rowKind int

const (
	rowCategory rowKind = iota
	rowArrayItems
	rowIncludeFns
	rowExcludeFns
	rowExcludeRegex
)

type row struct {
	kind rowKind
	cat  string // for rowCategory
}

func buildRows() []row {
	rows := make([]row, 0, len(categoryOrder)+4)
	for _, c := range categoryOrder {
		rows = append(rows, row{kind: rowCategory, cat: c})
	}
	rows = append(rows,
		row{kind: rowArrayItems},
		row{kind: rowIncludeFns},
		row{kind: rowExcludeFns},
		row{kind: rowExcludeRegex},
	)
	return rows
}

// ── styles ─────────────────────────────────────────────────────────────────

// Minimal, straight-line styling (no rounded corners): thin horizontal rules
// and a single left rule bar, muted palette.
var (
	stTitle   = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("#cdd6f4"))
	stCursor  = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("#cba6f7"))
	stInclude = lipgloss.NewStyle().Foreground(lipgloss.Color("#a6e3a1"))
	stExclude = lipgloss.NewStyle().Foreground(lipgloss.Color("#f38ba8"))
	stOff     = lipgloss.NewStyle().Foreground(lipgloss.Color("#6c7086"))
	stDesc    = lipgloss.NewStyle().Foreground(lipgloss.Color("#7f849c"))
	stKey     = lipgloss.NewStyle().Foreground(lipgloss.Color("#cba6f7"))
	stOK      = lipgloss.NewStyle().Foreground(lipgloss.Color("#a6e3a1"))
	stErr     = lipgloss.NewStyle().Foreground(lipgloss.Color("#f38ba8"))
	stPreview = lipgloss.NewStyle().Foreground(lipgloss.Color("#94e2d5"))
	stRule    = lipgloss.NewStyle().Foreground(lipgloss.Color("#313244"))
	stHint    = lipgloss.NewStyle().Foreground(lipgloss.Color("#f5c2e7"))
	stBar     = lipgloss.NewStyle().
			Border(lipgloss.NormalBorder(), false, false, false, true).
			BorderForeground(lipgloss.Color("#45475a")).
			PaddingLeft(1)
	stCarousel = lipgloss.NewStyle().
			Border(lipgloss.NormalBorder(), true, true, true, true).
			BorderForeground(lipgloss.Color("#585b70")).
			Padding(0, 2)
	stCarHead = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("#fab387"))
	stCarPat  = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("#f5e0dc"))
	stCarYes  = lipgloss.NewStyle().Foreground(lipgloss.Color("#a6e3a1"))
	stCarNo   = lipgloss.NewStyle().Foreground(lipgloss.Color("#f38ba8"))
	stCarPage = lipgloss.NewStyle().Italic(true).Foreground(lipgloss.Color("#585b70"))
)

// ── call-key format hints ─────────────────────────────────────────────────

// callKeyHint returns a short string describing what parts of a call key the
// given regex pattern targets.  The call key format is:
//
//	JniFunctionName|ClassName::methodName(type1, type2, ...)
//
// For field access:
//
//	JniFunctionName|ClassName.fieldName: type
func callKeyHint(pat string) string {
	if pat == "" {
		return "empty — matches nothing"
	}

	// Try to compile; if it fails we just show (invalid) instead of crashing.
	_, err := regexp.Compile(pat)
	if err != nil {
		return "invalid regex"
	}

	// Decompose the pattern into which parts of the call-key it targets.
	var tags []string

	// Does it target a specific JNI function name?  Check the full pattern
	// (bare identifier or anchored glob), or extract it from before a pipe.
	if fn := extractJniFnRef(pat); fn != "" {
		tags = append(tags, "fn:"+shortenFnRef(fn))
	}

	// Pipe = targets class-or-method context (the "|ClassName::methodName" part).
	if containsPipe(pat) {
		tags = append(tags, "class/method context")
		// "::" means it also targets a specific method name.
		if strings.Contains(pat, "::") {
			tags = append(tags, "method")
		}
		// Parentheses mean argument types are also matched (escape-aware).
		if strings.Contains(pat, `\(`) {
			tags = append(tags, "args")
		}
	}

	// Matches class names even without a pipe (e.g. "\bMyClass\b").
	if hasPackageLike(pat) {
		tags = append(tags, "class name")
	}

	switch {
	case len(tags) == 0:
		return "generic — tested against full call key"
	case len(tags) == 1 && strings.HasPrefix(tags[0], "fn:"):
		return tags[0]
	default:
		return strings.Join(tags, " · ")
	}
}

// extractJniFnRef extracts a JNI function name reference from the pattern,
// or returns "" if nothing looks like one.  Handles bare identifiers,
// anchored globs like "^Call.*", and prefix-before-pipe like "CallIntMethod|".
func extractJniFnRef(pat string) string {
	// Try the part before any pipe first — patterns like "CallIntMethod|...".
	prefix := pat
	if idx := strings.Index(pat, "|"); idx >= 0 {
		prefix = pat[:idx]
	}
	p := strings.TrimPrefix(prefix, "^")
	p = strings.TrimSuffix(p, "$")
	if p == "" {
		return ""
	}
	// Must start with uppercase.
	if p[0] < 'A' || p[0] > 'Z' {
		return ""
	}
	// Plain uppercase-starting identifier (no metachars) is always a JNI ref.
	if isPlainAlpha(p) {
		return p
	}
	// Otherwise, test if it's an anchor glob like "^Call.*" or "Call[A-Z].*".
	if isJniAnchorGlob(p) {
		return p
	}
	return ""
}

// isPlainAlpha returns true if s is non-empty and contains only letters.
func isPlainAlpha(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if !((r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z')) {
			return false
		}
	}
	return true
}

// isJniAnchorGlob returns true for patterns like "^Call.*" or "Call[A-Z].*"
// that start with an uppercase letter followed by a regex-like pattern that
// still clearly references a JNI function name.
func isJniAnchorGlob(p string) bool {
	// Strip anchors — we already did above, but be safe.
	p = strings.TrimPrefix(p, "^")
	p = strings.TrimSuffix(p, "$")
	if p == "" || !(p[0] >= 'A' && p[0] <= 'Z') {
		return false
	}
	// Strip common JNI-function-pattern metacharacters.
	// "Call[A-Z].*" → after stripping .* and [A-Z] ranges → "Call".
	clean := stripJniGlobMetas(p)
	for _, r := range clean {
		if (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') {
			continue
		}
		return false
	}
	return true
}

// stripJniGlobMetas removes regex metacharacters commonly used in JNI-name
// patterns: . * [ ] - plus backslash-escapes.
func stripJniGlobMetas(s string) string {
	s = strings.ReplaceAll(s, ".*", "")
	s = strings.ReplaceAll(s, ".", "")
	s = strings.ReplaceAll(s, "*", "")
	s = strings.ReplaceAll(s, "[", "")
	s = strings.ReplaceAll(s, "]", "")
	s = strings.ReplaceAll(s, "-", "")
	s = strings.ReplaceAll(s, "\\", "")
	return s
}

// shortenFnRef extracts a short readable reference from a function-name pattern.
func shortenFnRef(pat string) string {
	p := strings.TrimPrefix(pat, "^")
	p = strings.TrimSuffix(p, "$")
	// Replace ".*" with "*" for display
	p = strings.ReplaceAll(p, ".*", "*")
	// Remove escape backslashes for display
	p = strings.ReplaceAll(p, "\\.", ".")
	return p
}

// containsPipe reports whether the pattern targets the class/method portion
// of a call key (anything after the pipe separator).
func containsPipe(pat string) bool {
	// Look for an unescaped pipe
	for i := 0; i < len(pat); i++ {
		if pat[i] == '\\' {
			i++ // skip next char
			continue
		}
		if pat[i] == '|' {
			return true
		}
	}
	// Also check for | class/method indicators via hex or indirect pipes
	if strings.Contains(pat, `\|`) {
		return true
	}
	return false
}

// hasPackageLike checks if the pattern contains dotted package-like segments
// separated by escaped dots — a strong class-name signal even without a pipe.
func hasPackageLike(pat string) bool {
	// Look for patterns like `com\.` or `java\.` — package prefixes.
	return strings.Contains(pat, `\.`)
}

// ── regex carousel examples ───────────────────────────────────────────────

// regexExample is a single carousel entry demonstrating a practical pattern,
// what call keys it matches, and what keys it intentionally skips.
type regexExample struct {
	pattern string
	hint    string
	matches []string
	skips   []string
}

// regexExamples is a curated collection of patterns that span the full
// expressiveness of the call-key format — from narrow exact-match to broad
// wildcards, anchored JNI names, specific classes/methods, alternation, and
// package-name targeting.
var regexExamples = []regexExample{
	{
		pattern: `^CallIntMethod`,
		hint:    "exact JNI function, anchored at start",
		matches: []string{
			`CallIntMethod|com.Foo::bar(int)`,
			`CallIntMethod|com.Foo::baz(java.lang.String)`,
		},
		skips: []string{
			`CallVoidMethod|com.Foo::bar()`,
			`CallBooleanMethod|com.Foo::check()`,
		},
	},
	{
		pattern: `Set.*Field`,
		hint:    "any Set*Field call (field writes)",
		matches: []string{
			`SetIntField|com.Foo::x(int)`,
			`SetObjectField|com.Foo::ref(java.lang.Object)`,
		},
		skips: []string{
			`GetIntField|com.Foo::x(int)`,
			`CallIntMethod|com.Foo::setX()`,
		},
	},
	{
		pattern: `\|.*MyActivity`,
		hint:    "any call touching a specific class",
		matches: []string{
			`CallVoidMethod|com.app.MyActivity::onCreate(android.os.Bundle)`,
			`FindClass|com.app.MyActivity`,
		},
		skips: []string{
			`CallVoidMethod|com.app.OtherActivity::onCreate(android.os.Bundle)`,
			`FindClass|com.app.MyFragment`,
		},
	},
	{
		pattern: `::onClick`,
		hint:    "a specific method name on any class",
		matches: []string{
			`CallVoidMethod|android.view.View::onClick(android.view.View)`,
			`CallBooleanMethod|com.app.CustomBtn::onClick(android.view.View)`,
		},
		skips: []string{
			`CallVoidMethod|android.view.View::onCreate(android.os.Bundle)`,
			`CallVoidMethod|android.view.View::onLongClick(android.view.View)`,
		},
	},
	{
		pattern: `FindClass\|`,
		hint:    "class lookups by JNI name + pipe anchor",
		matches: []string{
			`FindClass|java.lang.String`,
			`FindClass|com.app.SecretService`,
		},
		skips: []string{
			`NewString|java.lang.String`,
			`CallVoidMethod|com.app.SecretService::run()`,
		},
	},
	{
		pattern: `(DeleteLocalRef|DeleteGlobalRef)`,
		hint:    "multiple related JNI functions (alternation)",
		matches: []string{
			`DeleteLocalRef|`,
			`DeleteGlobalRef|`,
		},
		skips: []string{
			`NewGlobalRef|`,
			`NewLocalRef|`,
		},
	},
	{
		pattern: `Call.*Method\|.*::getInstance`,
		hint:    "JNI fn group + specific method name",
		matches: []string{
			`CallObjectMethod|com.app.Singleton::getInstance()`,
			`CallStaticObjectMethod|com.app.Factory::getInstance(android.content.Context)`,
		},
		skips: []string{
			`CallObjectMethod|com.app.Singleton::getInstance(int)`,
			`CallObjectMethod|com.app.Singleton::getSystemService(java.lang.String)`,
		},
	},
	{
		pattern: `NewString`,
		hint:    "all string-creation JNI calls",
		matches: []string{
			`NewString|java.lang.String(abc)`,
			`NewStringUTF|java.lang.String(def)`,
		},
		skips: []string{
			`GetStringChars|java.lang.String(abc)`,
			`NewObject|java.lang.StringBuilder`,
		},
	},
	{
		pattern: `Throw`,
		hint:    "exception-related calls",
		matches: []string{
			`Throw|`,
			`ThrowNew|java.lang.Exception(msg)`,
		},
		skips: []string{
			`ExceptionCheck|()`,
			`ExceptionClear|`,
		},
	},
	{
		pattern: `com\\.example\\.`,
		hint:    "all calls in a package namespace",
		matches: []string{
			`FindClass|com.example.Foo`,
			`CallIntMethod|com.example.Bar::run(int)`,
		},
		skips: []string{
			`FindClass|com.other.Foo`,
			`CallIntMethod|org.whatever.Bar::run(int)`,
		},
	},
}

// regexCarouselTickMsg fires every 5s to advance the carousel.
type regexCarouselTickMsg struct{}

// ── model ──────────────────────────────────────────────────────────────────

type statusMsg struct {
	text string
	err  bool
}

type model struct {
	path   string
	serial string
	cs     configState
	rows   []row
	cursor int

	editing    bool
	input      textinput.Model
	editTarget rowKind

	// fuzzy multi-select picker for include/exclude function lists
	picking    bool
	pickTarget rowKind
	pickInput  textinput.Model
	pickSel    map[string]bool
	pickCursor int

	// regex list editor — each line is one pattern, multi-select with hints
	regexEditing bool
	regexList    []string
	regexCursor  int
	regexInput   textinput.Model
	regexLineIdx int // -1 = not editing a specific line

	// carousel that cycles thru example patterns in the regex editor
	regexCarouselIdx int

	width     int
	status    string
	statusErr bool
}

// a candidate row in the function picker.
type pcand struct {
	name string
	add  bool // synthetic "add custom name" entry
}

func newModel(path, serial string) model {
	cs := newConfigState()
	status := "new config (file not found) — defaults: log everything"
	if cj, err := loadFile(path); err == nil {
		cs.apply(cj)
		status = "loaded " + path
	}
	ti := textinput.New()
	ti.Prompt = "› "
	ti.CharLimit = 4096
	return model{path: path, serial: serial, cs: cs, rows: buildRows(), input: ti, status: status}
}

func (m model) Init() tea.Cmd { return nil }

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		return m, nil

	case statusMsg:
		m.status, m.statusErr = msg.text, msg.err
		// reloading after a pull
		if cj, err := loadFile(m.path); err == nil && !msg.err {
			m.cs.apply(cj)
		}
		return m, nil

	case regexCarouselTickMsg:
		if m.regexEditing {
			return m.updateRegexEditor(msg)
		}
		return m, nil

	case tea.KeyMsg:
		if m.picking {
			return m.updatePicker(msg)
		}
		if m.regexEditing {
			return m.updateRegexEditor(msg)
		}
		if m.editing {
			return m.updateEditing(msg)
		}
		return m.updateNav(msg)
	}
	return m, nil
}

func (m model) updateNav(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "q", "ctrl+c":
		return m, tea.Quit
	case "up", "k":
		if m.cursor > 0 {
			m.cursor--
		}
	case "down", "j":
		if m.cursor < len(m.rows)-1 {
			m.cursor++
		}
	case " ", "enter":
		return m.activate()
	case "left", "-":
		if m.rows[m.cursor].kind == rowArrayItems && m.cs.arrayItems > 0 {
			m.cs.arrayItems--
		}
	case "right", "+", "=":
		if m.rows[m.cursor].kind == rowArrayItems {
			m.cs.arrayItems++
		}
	case "s":
		if err := saveFile(m.path, m.cs); err != nil {
			m.status, m.statusErr = "save failed: "+err.Error(), true
		} else {
			m.status, m.statusErr = "saved "+m.path, false
		}
	case "p":
		return m, m.pushCmd()
	case "P":
		return m, m.pullCmd()
	case "r":
		m.cs = newConfigState()
		m.status, m.statusErr = "reset to defaults (log everything)", false
	}
	return m, nil
}

// activate toggles a category, opens the function picker, or opens the inline
// editor (array_items) / regex list editor.
func (m model) activate() (tea.Model, tea.Cmd) {
	r := m.rows[m.cursor]
	switch r.kind {
	case rowCategory:
		m.cs.catState[r.cat] = (m.cs.catState[r.cat] + 1) % 3 // off->include->exclude->off
		return m, nil
	case rowIncludeFns, rowExcludeFns:
		return m.openPicker(r.kind)
	case rowArrayItems:
		m.editing = true
		m.editTarget = r.kind
		m.input.SetValue(strconv.Itoa(m.cs.arrayItems))
		m.input.CursorEnd()
		m.input.Focus()
		return m, textinput.Blink
	case rowExcludeRegex:
		return m.openRegexEditor()
	}
	return m, nil
}

func (m model) updateEditing(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc":
		m.editing = false
		m.input.Blur()
		return m, nil
	case "enter":
		v := m.input.Value()
		switch m.editTarget {
		case rowArrayItems:
			if n, err := strconv.Atoi(trim(v)); err == nil && n >= 0 {
				m.cs.arrayItems = n
			}
		}
		m.editing = false
		m.input.Blur()
		return m, nil
	}
	var cmd tea.Cmd
	m.input, cmd = m.input.Update(msg)
	return m, cmd
}

// ── function picker (fuzzy multi-select w/ autocomplete) ─────────────────────

func (m model) openPicker(target rowKind) (tea.Model, tea.Cmd) {
	m.picking = true
	m.pickTarget = target
	m.pickSel = map[string]bool{}
	cur := m.cs.includeFns
	if target == rowExcludeFns {
		cur = m.cs.excludeFns
	}
	for _, f := range cur {
		m.pickSel[f] = true
	}
	ti := textinput.New()
	ti.Prompt = ""
	ti.Placeholder = "type to filter…"
	ti.CharLimit = 128
	ti.Focus()
	m.pickInput = ti
	m.pickCursor = 0
	return m, textinput.Blink
}

// pickerCandidates: synthetic "add" entry (when the filter is a new name),
// then selected custom names matching the filter, then catalog matches.
func (m model) pickerCandidates() []pcand {
	q := strings.TrimSpace(m.pickInput.Value())
	var out []pcand
	if q != "" && !catalogHas(q) && !m.pickSel[q] {
		out = append(out, pcand{name: q, add: true})
	}
	ql := strings.ToLower(q)
	var customs []string
	for f := range m.pickSel {
		if !catalogHas(f) && (q == "" || strings.Contains(strings.ToLower(f), ql)) {
			customs = append(customs, f)
		}
	}
	sort.Strings(customs)
	for _, f := range customs {
		out = append(out, pcand{name: f})
	}
	for _, f := range matchFunctions(q) {
		out = append(out, pcand{name: f})
	}
	return out
}

func (m model) updatePicker(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc", "ctrl+c":
		m.commitPicker()
		m.picking = false
		m.pickInput.Blur()
		return m, nil
	case "up":
		if m.pickCursor > 0 {
			m.pickCursor--
		}
		return m, nil
	case "down":
		if m.pickCursor < len(m.pickerCandidates())-1 {
			m.pickCursor++
		}
		return m, nil
	case " ", "enter":
		cands := m.pickerCandidates()
		if m.pickCursor >= 0 && m.pickCursor < len(cands) {
			c := cands[m.pickCursor]
			if c.add {
				m.pickSel[c.name] = true
				m.pickInput.SetValue("")
				m.pickCursor = 0
			} else {
				m.pickSel[c.name] = !m.pickSel[c.name]
			}
		}
		return m, nil
	}
	var cmd tea.Cmd
	m.pickInput, cmd = m.pickInput.Update(msg)
	if n := len(m.pickerCandidates()); m.pickCursor >= n {
		m.pickCursor = n - 1
	}
	if m.pickCursor < 0 {
		m.pickCursor = 0
	}
	return m, cmd
}

// commitPicker writes the selection back to the config: catalog names in
// catalog order, then any custom names alphabetically.
func (m *model) commitPicker() {
	var sel []string
	for _, f := range allFunctions {
		if m.pickSel[f] {
			sel = append(sel, f)
		}
	}
	var customs []string
	for f := range m.pickSel {
		if m.pickSel[f] && !catalogHas(f) {
			customs = append(customs, f)
		}
	}
	sort.Strings(customs)
	sel = append(sel, customs...)
	if m.pickTarget == rowIncludeFns {
		m.cs.includeFns = sel
	} else {
		m.cs.excludeFns = sel
	}
}

// ── regex list editor ──────────────────────────────────────────────────────

// openRegexEditor enters the multi-line regex list editor view, where each
// line is one exclude regex pattern with a format hint.
func (m model) openRegexEditor() (tea.Model, tea.Cmd) {
	m.regexEditing = true
	// Copy the current list; ensure at least one empty slot.
	m.regexList = append([]string(nil), m.cs.excludeRegex...)
	if len(m.regexList) == 0 {
		m.regexList = []string{""}
	}
	m.regexCursor = 0
	m.regexLineIdx = -1
	m.regexCarouselIdx = 0
	ti := textinput.New()
	ti.Prompt = "› "
	ti.CharLimit = 4096
	m.regexInput = ti
	return m, m.regexCarouselTick()
}

// regexCarouselTick returns a tea.Cmd that fires regexCarouselTickMsg after 5s.
func (m model) regexCarouselTick() tea.Cmd {
	return tea.Tick(5*time.Second, func(t time.Time) tea.Msg {
		return regexCarouselTickMsg{}
	})
}

// commitRegexEditor writes the regex list back to the config state (filtering
// out empty lines).
func (m *model) commitRegexEditor() {
	var out []string
	for _, pat := range m.regexList {
		if strings.TrimSpace(pat) != "" {
			out = append(out, strings.TrimSpace(pat))
		}
	}
	m.cs.excludeRegex = out
}

func (m model) updateRegexEditor(msg tea.Msg) (tea.Model, tea.Cmd) {
	// Handle carousel tick first — advance and re-schedule.
	if _, ok := msg.(regexCarouselTickMsg); ok {
		m.regexCarouselIdx = (m.regexCarouselIdx + 1) % len(regexExamples)
		return m, m.regexCarouselTick()
	}

	keyMsg, ok := msg.(tea.KeyMsg)
	if !ok {
		return m, nil
	}

	// If we're editing a specific line, route keystrokes to the line editor.
	if m.regexLineIdx >= 0 {
		return m.updateRegexLineEdit(keyMsg)
	}

	switch keyMsg.String() {
	case "esc", "ctrl+c":
		m.commitRegexEditor()
		m.regexEditing = false
		return m, nil

	case "up", "k":
		if m.regexCursor > 0 {
			m.regexCursor--
		}
		return m, nil

	case "down", "j":
		if m.regexCursor < len(m.regexList) {
			m.regexCursor++
		}
		return m, nil

	case "[":
		// Previous carousel example.
		m.regexCarouselIdx = (m.regexCarouselIdx - 1 + len(regexExamples)) % len(regexExamples)
		return m, m.regexCarouselTick()

	case "]":
		// Next carousel example.
		m.regexCarouselIdx = (m.regexCarouselIdx + 1) % len(regexExamples)
		return m, m.regexCarouselTick()

	case "enter":
		// Enter on the "add new" slot → insert a blank line and edit it.
		if m.regexCursor >= len(m.regexList) {
			m.regexList = append(m.regexList, "")
		}
		m.regexLineIdx = m.regexCursor
		if m.regexCursor >= 0 && m.regexCursor < len(m.regexList) {
			m.regexInput.SetValue(m.regexList[m.regexCursor])
		}
		m.regexInput.CursorEnd()
		m.regexInput.Focus()
		return m, textinput.Blink

	case "d", "D", "ctrl+d", "x":
		// Delete the selected pattern (only if it's a real entry).
		if m.regexCursor >= 0 && m.regexCursor < len(m.regexList) {
			m.regexList = append(m.regexList[:m.regexCursor], m.regexList[m.regexCursor+1:]...)
			if m.regexCursor >= len(m.regexList) && m.regexCursor > 0 {
				m.regexCursor--
			}
			// Ensure at least one slot remains.
			if len(m.regexList) == 0 {
				m.regexList = []string{""}
			}
		}
		return m, nil

	case "a", "A", "ctrl+a":
		// Append a new blank pattern and start editing it.
		m.regexList = append(m.regexList, "")
		m.regexCursor = len(m.regexList) - 1
		m.regexLineIdx = m.regexCursor
		m.regexInput.SetValue("")
		m.regexInput.CursorEnd()
		m.regexInput.Focus()
		return m, textinput.Blink
	}

	return m, nil
}

// updateRegexLineEdit handles editing a single line (opened via enter).
func (m model) updateRegexLineEdit(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc":
		// Cancel edit of this line — restore original value.
		m.regexLineIdx = -1
		m.regexInput.Blur()
		return m, nil

	case "enter":
		// Commit edit of this line.
		if m.regexLineIdx >= 0 && m.regexLineIdx < len(m.regexList) {
			m.regexList[m.regexLineIdx] = m.regexInput.Value()
			// If this was the last line and it's non-empty, add a new empty slot.
			if m.regexLineIdx == len(m.regexList)-1 && strings.TrimSpace(m.regexInput.Value()) != "" {
				m.regexList = append(m.regexList, "")
				m.regexCursor = m.regexLineIdx + 1
			} else {
				m.regexCursor = m.regexLineIdx + 1
				if m.regexCursor >= len(m.regexList) {
					m.regexList = append(m.regexList, "")
					m.regexCursor = len(m.regexList) - 1
				}
			}
		}
		m.regexLineIdx = -1
		m.regexInput.Blur()
		return m, nil
	}
	var cmd tea.Cmd
	m.regexInput, cmd = m.regexInput.Update(msg)
	return m, cmd
}

func (m model) pushCmd() tea.Cmd {
	path, serial, cs := m.path, m.serial, m.cs
	return func() tea.Msg {
		if err := saveFile(path, cs); err != nil {
			return statusMsg{"save failed: " + err.Error(), true}
		}
		out, err := adbPush(serial, path)
		return statusMsg{out, err != nil}
	}
}

func (m model) pullCmd() tea.Cmd {
	path, serial := m.path, m.serial
	return func() tea.Msg {
		out, err := adbPull(serial, path)
		return statusMsg{out, err != nil}
	}
}

// ── view ───────────────────────────────────────────────────────────────────

// dividerWidth spans the full terminal width (falls back to 80 before the
// first WindowSizeMsg).
func (m model) dividerWidth() int {
	if m.width > 1 {
		return m.width
	}
	return 80
}

func (m model) View() string {
	if m.picking {
		return m.viewPicker()
	}
	if m.regexEditing {
		return m.viewRegexEditor()
	}
	rule := stRule.Render(strings.Repeat("─", m.dividerWidth()))

	var b []string
	b = append(b, stTitle.Render("jnilog config")+stDesc.Render("   "+m.path))
	b = append(b, rule)
	b = append(b, stDesc.Render("mode  ")+m.cs.effectiveMode())
	b = append(b, rule)

	for i, r := range m.rows {
		// separator between the category block and the settings block
		if r.kind != rowCategory && i > 0 && m.rows[i-1].kind == rowCategory {
			b = append(b, rule)
		}
		cursor := "  "
		if i == m.cursor {
			cursor = stCursor.Render("▌ ")
		}
		b = append(b, cursor+m.renderRow(r, i == m.cursor))
	}

	b = append(b, rule)
	b = append(b, stDesc.Render("preview  "+m.path))
	b = append(b, stBar.Render(stPreview.Render(m.cs.json())))
	b = append(b, rule)
	b = append(b, m.help())

	st := stOK
	if m.statusErr {
		st = stErr
	}
	b = append(b, st.Render(m.status))
	return join(b)
}

func (m model) viewPicker() string {
	rule := stRule.Render(strings.Repeat("─", m.dividerWidth()))
	label := "include functions  (whitelist)"
	if m.pickTarget == rowExcludeFns {
		label = "exclude functions  (suppress)"
	}
	var b []string
	b = append(b, stTitle.Render("jnilog config")+stDesc.Render("   ·   "+label))
	b = append(b, rule)
	b = append(b, stKey.Render("filter  ")+m.pickInput.View())
	b = append(b, rule)

	cands := m.pickerCandidates()
	const maxRows = 14
	start := 0
	if m.pickCursor >= maxRows {
		start = m.pickCursor - maxRows + 1
	}
	if len(cands) == 0 {
		b = append(b, stOff.Render("  (no matches — type a name and press enter to add it)"))
	}
	for i := start; i < len(cands) && i < start+maxRows; i++ {
		c := cands[i]
		cur := "  "
		if i == m.pickCursor {
			cur = stCursor.Render("▌ ")
		}
		if c.add {
			b = append(b, cur+stInclude.Render(`+ add "`+c.name+`"  (custom)`))
			continue
		}
		box, name := stOff.Render("[ ]"), stDesc.Render(c.name)
		if m.pickSel[c.name] {
			box, name = stInclude.Render("[x]"), stInclude.Render(c.name)
		}
		b = append(b, cur+box+" "+name)
	}
	if n := len(cands); n > start+maxRows {
		b = append(b, stOff.Render(fmt.Sprintf("  … %d more", n-(start+maxRows))))
	}

	b = append(b, rule)
	b = append(b, stDesc.Render(fmt.Sprintf("%d selected", countSel(m.pickSel))))
	b = append(b, m.pickerHelp())
	return join(b)
}

func (m model) viewRegexEditor() string {
	rule := stRule.Render(strings.Repeat("─", m.dividerWidth()))

	var b []string
	b = append(b, stTitle.Render("jnilog config")+stDesc.Render("   ·   exclude regex patterns"))
	b = append(b, rule)
	b = append(b, stDesc.Render(
		"Each pattern is tested against the call key format:"))
	b = append(b, stDesc.Render(
		"  " + stHint.Render("FunctionName|ClassName::methodName(type1, type2, ...)")))

	// ── carousel ────────────────────────────────────────────────────
	cx := regexExamples[m.regexCarouselIdx]
	var carLines []string

	// Header row
	carLines = append(carLines, fmt.Sprintf("  %s   %s",
		stCarHead.Render("pattern carousel"),
		stCarPage.Render(fmt.Sprintf("%d / %d", m.regexCarouselIdx+1, len(regexExamples)))))

	// Blank line for breathing room
	carLines = append(carLines, "")

	// Pattern on its own line with a label
	carLines = append(carLines, fmt.Sprintf("  %s   %s",
		stKey.Render("pattern"), stCarPat.Render(cx.pattern)))

	// Hint on its own line with a label
	carLines = append(carLines, fmt.Sprintf("  %s      %s",
		stKey.Render("hint"), stDesc.Render(cx.hint)))

	// Blank line before matches
	carLines = append(carLines, "")

	// Matches section
	carLines = append(carLines, fmt.Sprintf("  %s", stKey.Render("matches")))
	for _, m := range cx.matches {
		disp := m
		const carMatchMax = 56
		if len(disp) > carMatchMax {
			disp = disp[:carMatchMax-1] + "…"
		}
		carLines = append(carLines, "    "+stCarYes.Render("✓  "+disp))
	}

	// Blank line before skips
	carLines = append(carLines, "")

	// Skips section
	carLines = append(carLines, fmt.Sprintf("  %s", stKey.Render("skips")))
	for _, s := range cx.skips {
		disp := s
		const carSkipMax = 56
		if len(disp) > carSkipMax {
			disp = disp[:carSkipMax-1] + "…"
		}
		carLines = append(carLines, "    "+stCarNo.Render("✗  "+disp))
	}

	carInner := join(carLines)
	carBox := stCarousel.Render(carInner)
	b = append(b, carBox)
	b = append(b, rule)

	const maxRows = 14

	// Find the cursor window.
	start := 0
	if m.regexCursor >= maxRows {
		start = m.regexCursor - maxRows + 1
	}
	if start > len(m.regexList) {
		start = 0
	}

	for i := start; i < len(m.regexList) && i < start+maxRows; i++ {
		cur := "  "
		if i == m.regexCursor {
			cur = stCursor.Render("▌ ")
		}

		pat := m.regexList[i]
		hint := callKeyHint(pat)

		// If we're editing this line, show the input widget.
		if m.regexEditing && m.regexLineIdx == i {
			line := fmt.Sprintf("%s%2d  %s", cur, i+1, m.regexInput.View())
			b = append(b, line)
			b = append(b, fmt.Sprintf("     %s", stHint.Render("→ "+hint)))
		} else if pat == "" {
			line := fmt.Sprintf("%s%2d  %s", cur, i+1, stOff.Render("(empty)"))
			b = append(b, line)
			b = append(b, fmt.Sprintf("     %s", stHint.Render("→ "+hint)))
		} else {
			// Highlight invalid patterns.
			style := stPreview
			if _, err := regexp.Compile(pat); err != nil {
				style = stErr
			}
			// Truncate long patterns.
			disp := pat
			const maxPatWidth = 64
			if len(disp) > maxPatWidth {
				disp = disp[:maxPatWidth-1] + "…"
			}
			line := fmt.Sprintf("%s%2d  %s", cur, i+1, style.Render(disp))
			b = append(b, line)
			b = append(b, fmt.Sprintf("     %s", stHint.Render("→ "+hint)))
		}
	}

	// "add new" row — only if we're not at the last visible line.
	if len(m.regexList) < start+maxRows || m.regexCursor >= len(m.regexList) {
		cur := "  "
		if m.regexCursor >= len(m.regexList) {
			cur = stCursor.Render("▌ ")
		}
		b = append(b, cur+stInclude.Render("+ add new pattern"))
	}

	if n := len(m.regexList); n > start+maxRows {
		b = append(b, stOff.Render(fmt.Sprintf("  … %d total", n)))
	}

	b = append(b, rule)
	count := 0
	for _, pat := range m.regexList {
		if strings.TrimSpace(pat) != "" {
			count++
		}
	}
	b = append(b, stDesc.Render(fmt.Sprintf("%d pattern(s)", count)))
	b = append(b, m.regexEditorHelp())
	return join(b)
}

func (m model) regexEditorHelp() string {
	if m.regexLineIdx >= 0 {
		return stDesc.Render("editing — ") + stKey.Render("enter") + stDesc.Render(" commit   ") +
			stKey.Render("esc") + stDesc.Render(" cancel")
	}
	k := func(key, what string) string { return stKey.Render(key) + stDesc.Render(" "+what+"   ") }
	return k("↑↓", "move") + k("enter", "edit") + k("a", "add") +
		k("d", "delete") + k("[", "prev ex") + k("]", "next ex") + k("esc", "done")
}

func (m model) pickerHelp() string {
	k := func(key, what string) string { return stKey.Render(key) + stDesc.Render(" "+what+"   ") }
	return k("type", "filter") + k("↑↓", "move") + k("space", "toggle") +
		k("enter", "add/toggle") + k("esc", "done")
}

func countSel(s map[string]bool) int {
	n := 0
	for _, v := range s {
		if v {
			n++
		}
	}
	return n
}

func (m model) renderRow(r row, sel bool) string {
	switch r.kind {
	case rowCategory:
		col := fmt.Sprintf("%-10s", r.cat) // pad plain, before styling, so descs align
		var mark, name string
		switch m.cs.catState[r.cat] {
		case stateInclude:
			mark, name = stInclude.Render("[+] include"), stInclude.Render(col)
		case stateExclude:
			mark, name = stExclude.Render("[-] exclude"), stExclude.Render(col)
		default:
			mark, name = stOff.Render("[ ] off    "), stOff.Render(col)
		}
		return fmt.Sprintf("%s  %s  %s", mark, name, stDesc.Render(categoryDesc[r.cat]))
	case rowArrayItems:
		v := strconv.Itoa(m.cs.arrayItems)
		if m.editing && m.editTarget == rowArrayItems {
			v = m.input.View()
		}
		return fmt.Sprintf("%s  %s  %s", stKey.Render("array_items"), v,
			stDesc.Render("max array elements before +N more"))
	case rowIncludeFns:
		return m.fnRow("include fns", m.cs.includeFns, rowIncludeFns, "extra function names to whitelist")
	case rowExcludeFns:
		return m.fnRow("exclude fns", m.cs.excludeFns, rowExcludeFns, "function names to suppress (e.g. DeleteLocalRef)")
	case rowExcludeRegex:
		return m.regexRow("excl. regex", m.cs.excludeRegex, "regex on call keys: JniName|Class::method(args)")
	}
	return ""
}

func (m model) regexRow(label string, vals []string, desc string) string {
	if len(vals) == 0 {
		return fmt.Sprintf("%s  %s  %s", stKey.Render(label), stOff.Render("(none)"), stDesc.Render(desc))
	}
	// Show count and a summary hint for the first pattern.
	first := vals[0]
	hint := callKeyHint(first)
	const maxw = 40
	if len(first) > maxw {
		first = first[:maxw-1] + "…"
	}
	return fmt.Sprintf("%s  %s  %s  [%s]", stKey.Render(label),
		stPreview.Render(first),
		stDesc.Render(fmt.Sprintf("(%d pattern(s))", len(vals))),
		stHint.Render(hint))
}

func (m model) fnRow(label string, vals []string, k rowKind, desc string) string {
	if m.editing && m.editTarget == k {
		return fmt.Sprintf("%s  %s", stKey.Render(label), m.input.View())
	}
	if len(vals) == 0 {
		return fmt.Sprintf("%s  %s  %s", stKey.Render(label), stOff.Render("(none)"), stDesc.Render(desc))
	}
	v := joinCSV(vals)
	const maxw = 52
	if len(v) > maxw {
		v = v[:maxw-1] + "…"
	}
	return fmt.Sprintf("%s  %s  %s", stKey.Render(label), stPreview.Render(v),
		stDesc.Render(fmt.Sprintf("(%d)", len(vals))))
}

func (m model) help() string {
	if m.editing {
		return stDesc.Render("editing — ") + stKey.Render("enter") + stDesc.Render(" commit   ") +
			stKey.Render("esc") + stDesc.Render(" cancel")
	}
	k := func(key, what string) string { return stKey.Render(key) + stDesc.Render(" "+what+"   ") }
	return k("↑↓", "move") + k("space", "toggle/edit") + k("←→", "count") +
		k("s", "save") + k("p", "push→dev") + k("P", "pull←dev") + k("r", "reset") + k("q", "quit")
}

// ── main ───────────────────────────────────────────────────────────────────

func main() {
	var path, serial string
	var doPrint, doPush, doPull bool
	flag.StringVar(&path, "f", "jnilog.json", "config file path (load + save target)")
	flag.StringVar(&serial, "s", "", "adb device serial for push/pull")
	flag.BoolVar(&doPrint, "print", false, "print the loaded/normalized config and exit")
	flag.BoolVar(&doPush, "push", false, "normalize + save the file, push it to the device, and exit")
	flag.BoolVar(&doPull, "pull", false, "pull "+devicePath+" from the device into the file and exit")
	flag.Parse()

	if doPull {
		if out, err := adbPull(serial, path); err != nil {
			fmt.Fprintln(os.Stderr, "pull failed:", out)
			os.Exit(1)
		} else {
			fmt.Println(out)
		}
		return
	}

	if doPrint || doPush {
		cs := newConfigState()
		if cj, err := loadFile(path); err == nil {
			cs.apply(cj)
		}
		if doPush {
			if err := saveFile(path, cs); err != nil {
				fmt.Fprintln(os.Stderr, "save failed:", err)
				os.Exit(1)
			}
			out, err := adbPush(serial, path)
			if err != nil {
				fmt.Fprintln(os.Stderr, "push failed:", out)
				os.Exit(1)
			}
			fmt.Println(out)
			return
		}
		fmt.Println(cs.json())
		return
	}

	p := tea.NewProgram(newModel(path, serial), tea.WithAltScreen())
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

// ── small helpers ──────────────────────────────────────────────────────────

func joinCSV(s []string) string {
	out := ""
	for i, t := range s {
		if i > 0 {
			out += ", "
		}
		out += t
	}
	return out
}

func join(lines []string) string {
	out := ""
	for i, l := range lines {
		if i > 0 {
			out += "\n"
		}
		out += l
	}
	return out
}

func trim(s string) string {
	for len(s) > 0 && (s[0] == ' ' || s[0] == '\t') {
		s = s[1:]
	}
	for len(s) > 0 && (s[len(s)-1] == ' ' || s[len(s)-1] == '\t') {
		s = s[:len(s)-1]
	}
	return s
}
