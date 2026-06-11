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
	"sort"
	"strconv"
	"strings"

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
	stBar     = lipgloss.NewStyle().
			Border(lipgloss.NormalBorder(), false, false, false, true).
			BorderForeground(lipgloss.Color("#45475a")).
			PaddingLeft(1)
)

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

	case tea.KeyMsg:
		if m.picking {
			return m.updatePicker(msg)
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
// editor (array_items / regex).
func (m model) activate() (tea.Model, tea.Cmd) {
	r := m.rows[m.cursor]
	switch r.kind {
	case rowCategory:
		m.cs.catState[r.cat] = (m.cs.catState[r.cat] + 1) % 3 // off->include->exclude->off
		return m, nil
	case rowIncludeFns, rowExcludeFns:
		return m.openPicker(r.kind)
	default:
		m.editing = true
		m.editTarget = r.kind
		switch r.kind {
		case rowArrayItems:
			m.input.SetValue(strconv.Itoa(m.cs.arrayItems))
		case rowExcludeRegex:
			m.input.SetValue(joinCSV(m.cs.excludeRegex))
		}
		m.input.CursorEnd()
		m.input.Focus()
		return m, textinput.Blink
	}
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
		case rowExcludeRegex:
			m.cs.excludeRegex = parseCSV(v)
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
		return m.fnRow("excl. regex", m.cs.excludeRegex, rowExcludeRegex, "regex on call keys: fn|class::method(args): ret")
	}
	return ""
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
