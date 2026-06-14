# jnilogcfg — TUI/CLI configurator for `jnilog.json`

A small standalone Go tool to build, edit, and deploy the libjnilog runtime
config (`/data/local/tmp/jnilog.json`). It is a **separate module** — its TUI
dependencies (Bubble Tea) never touch the c-shared library build.

## Run

```sh
cd tools/jnilogcfg
go run . -f ../../docs/jnilog.json        # open the TUI on a file
# or build once:
go build -o jnilogcfg . && ./jnilogcfg -f myconfig.json
```

From the repo root you can also use the xmake tasks:

```sh
xmake cfgtui        # opens the TUI on docs/jnilog.json
xmake cfgbuild      # builds the host binary into dist/jnilogcfg
```

## TUI keys

| key      | action                                              |
|----------|-----------------------------------------------------|
| `↑`/`↓`  | move between rows                                   |
| `space`  | category: cycle **off → include → exclude → off**; `include fns`/`exclude fns`: open the function picker; `array_items`: edit; `excl. regex`: open regex list editor |
| `enter`  | same as space (also commits an edit)                |
| `←`/`→`  | adjust `array_items` (also `-`/`+`)                 |
| `s`      | save to the file                                    |
| `p`      | save **and `adb push`** the config to the device    |
| `P`      | **`adb pull`** the device config back into the file |
| `r`      | reset to defaults (log everything)                  |
| `q`      | quit                                                |

### push / pull

The lib reads its config from `/data/local/tmp/jnilog.json` on the device at
injection time. **push** (`p`, or `-push`) writes your edited file and
`adb push`es it there (then `chmod 644` so the uid-isolated app can read it).
**pull** (`P`, or `-pull`) does the reverse — `adb pull`s whatever config is
currently on the device into your local file, so you can inspect/edit it. Both
honor `-s SERIAL` when multiple devices are attached.

### function picker (include / exclude)

`include fns` and `exclude fns` open a fuzzy multi-select picker: type to filter
the catalog of known JNI function names (live autocomplete/hinting), `space`/`enter`
to toggle `[x]`, `esc` to commit. A name not in the catalog shows a
`+ add "…"` entry so you can add custom symbols. The selection is written back
as the `functions` (whitelist) or `exclude.functions` (blacklist) list.

A live JSON preview of exactly what will be written is shown at all times,
along with the effective mode (all-minus-excludes vs whitelist).

### regex list editor (excl. regex)

`excl. regex` opens a **multi-line list editor** where each line is one regex
pattern with a **live format hint** describing what parts of the call key it
targets. The call key format is:

    FunctionName|ClassName::methodName(type1, type2, ...)

| key      | action inside the regex editor                       |
|----------|------------------------------------------------------|
| `↑`/`↓`  | move between patterns                                |
| `enter`  | edit the selected pattern (opens inline text input)  |
| `a`      | append a new blank pattern                           |
| `d`      | delete the selected pattern                          |
| `esc`    | commit all changes and return to the main view       |

#### Format hints

Each pattern gets an automatic, color-coded hint:

| Pattern                         | Hint                                 |
|---------------------------------|--------------------------------------|
| `DeleteLocalRef`                | `fn:DeleteLocalRef`                  |
| `Call.*`                        | `fn:Call*`                           |
| `\|.*MyClass::`                 | `class/method context · method`      |
| `\|.*MyClass::doSomething\(`   | `class/method context · method · args`|
| `\.MyClass`                    | `class name`                         |
| `FindClass\|.*SomeClass`       | `fn:FindClass · class/method context` |
| *(empty)*                       | `empty — matches nothing`            |
| *(invalid regex)*               | `invalid regex` (shown in red)       |

Invalid patterns are highlighted in red so you catch syntax errors at a glance.

## CLI (non-interactive)

```sh
jnilogcfg -f f.json -print              # normalize + print the config
jnilogcfg -f f.json -s SERIAL -push     # save + adb push to the device
jnilogcfg -f f.json -s SERIAL -pull     # adb pull the device config into f.json
```

`-s SERIAL` (optional) selects the adb device for push/pull.

## Config model (mirrors `src/go/config.go`)

- **Categories**: `methods`, `fields`, `lookups`, `strings`, `arrays`, `refs`,
  `exceptions`. Each can be *included* (whitelist) or *excluded* (blacklist).
- **Include empty ⇒ everything is logged** (minus the excludes). Adding any
  include category/function switches the lib into whitelist mode.
- **Exclude** always applies (functions, categories, and regex on call keys).
- **array_items**: max array elements rendered before `+N more`.
