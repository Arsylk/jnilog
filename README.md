# JNILog — Android JNI Call Interception & Structured Logging

A native shared library that hooks the **entire JNI function table** (~228 function pointers) inside any Android process, logs every call with **type-directed ANSI coloring**, and supports **runtime JSON configuration** for whitelist/blacklist/regex-based filtering.

Inspired by [frida-jnitrace](https://github.com/iddoeldor/frida-jnitrace).  No Frida required — the library injects itself via PLT/GOT patching in zygote and operates as a standalone `.so`.

---

## Quick Start

Requires the Android NDK (set `ANDROID_NDK_HOME` or `ANDROID_NDK`) and Go ≥ 1.26.

```bash
# Build the payload (produces dist/libjnilog.so)
xmake b jnilog

# Push to a connected device
xmake push                                 # convenience: adb push dist/libjnilog.so /data/local/tmp/

# Filter with a config file (optional)
echo '{"exclude":{"categories":["refs","strings"]}}' | adb shell tee /data/local/tmp/jnilog.json
```

Injection into a target process is out of scope for this build — load the produced
`.so` with your preferred mechanism (e.g. `LD_PRELOAD`, ptrace-based injector, or
the Zygisk module pattern). The library detects its environment in
`library_constructor` and self-installs the JNI table hooks when a `JavaVM` is
available.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  main.c          Injection layer                             │
│                  • ELF PLT/GOT patching (__loader_dlopen)    │
│                  • mprotect interception (live map tracking) │
│                  • VM table hook (GetEnv → new threads)      │
├──────────────────────────────────────────────────────────────┤
│  hooks.c         Core hook table + ~90 utility hooks         │
│  hook_methods.c  All Call*Method variants via X-macros       │
│  hook_fields.c   All Get/Set*Field variants via X-macros     │
│  hook_internal.h Types, enums, 3 X-macro type lists          │
├──────────────────────────────────────────────────────────────┤
│  hook_logging.c  Typed wire-protocol encoding                │
│  hook_common.c   Method/field caches, reentrancy, config     │
├──────────────────────────────────────────────────────────────┤
│  visualize.c     JNI object introspection (class name,       │
│                  toString, array elements, type detection)   │
│                  + vis_encode_typed_args (method arg → wire) │
├──────────────────────────────────────────────────────────────┤
│  bridge.c        ART symbol resolution, Go↔C glue            │
│  rangeset.c      /proc/self/maps caller filtering            │
├──────────────────────────────────────────────────────────────┤
│  Go side                                                    │
│  logger.go       Colorized formatter, all emit_* functions   │
│  value.go        JNIValue, decodeArgs, buildReturnValue      │
│  config.go       JSON config, cgo exports, call-key builder  │
│  main.go         Cgo callbacks, callFrame stack              │
└──────────────────────────────────────────────────────────────┘
```

### Data flow

1. **Hook entry** → checks `should_log_from_caller` (is caller from a tracked library?)  
2. **Config gate** → checks whitelist/blacklist (cached O(1) lookup, cgo crossing only once per function name)  
3. **Arg extraction** → `extract_va_args` pulls typed args from `va_list`  
4. **Wire encoding** → `vis_encode_typed_args` produces `sigChar\x01value\x02` records  
5. **Go callback** → `goJNICallCallback` builds `callFrame`, pushes to per-thread stack  
6. **Call execution** → original JNI function runs  
7. **Return encoding** → `log_method_return_value` resolves the typed return value  
8. **Go render** → `emitCallFull` pops frame, checks Gate 3 (regex blacklist), formats and writes  

---

## Hook Coverage — Complete JNINativeInterface

| Category | Count | Key functions |
|---|---|---|
| **Method calls** | 93 | `Call*Method`, `CallStatic*Method`, `CallNonvirtual*Method`, `NewObject` — all 9 types × 3 variants |
| **Field access** | 36 | `Get/Set*Field`, `Get/SetStatic*Field` — all 9 types |
| **Lookups** | 5 | `FindClass`, `GetMethodID`, `GetStaticMethodID`, `GetFieldID`, `GetStaticFieldID` |
| **Register** | 2 | `RegisterNatives`, `UnregisterNatives` |
| **References** | 9 | `New/Delete{Global,Local,WeakGlobal}Ref`, `IsSameObject`, `Push/PopLocalFrame` |
| **Strings** | 10 | `{New,Get,Release}{String,StringUTF}{Chars,}`, `GetString{UTF,}{Length,Region}` |
| **Arrays — primitive** | 40 | `New*Array`, `Get/Release*ArrayElements`, `Get/Set*ArrayRegion` — all 8 types |
| **Arrays — object** | 4 | `NewObjectArray`, `Get/SetObjectArrayElement`, `GetArrayLength` |
| **Critical sections** | 4 | `Get/ReleasePrimitiveArrayCritical`, `Get/ReleaseStringCritical` |
| **Exceptions** | 7 | `Throw`, `ThrowNew`, `Exception{Occurred,Describe,Clear,Check}`, `FatalError` |
| **Class/Object** | 7 | `AllocObject`, `IsInstanceOf`, `DefineClass`, `GetSuperclass`, `IsAssignableFrom`, `GetObjectClass` |
| **Direct buffers** | 3 | `NewDirectByteBuffer`, `GetDirectBuffer{Address,Capacity}` |
| **Monitors** | 2 | `MonitorEnter`, `MonitorExit` |
| **Misc** | 6 | `GetVersion`, `GetJavaVM`, `ToReflected{Method,Field}`, `FromReflected{Method,Field}` |
| **Total** | **228** | |

---

## Output Format

Every JNI event is rendered as a single colored log line.  Colors follow a consistent palette:

| Element | Color | Example |
|---|---|---|
| Class name | Cyan | `com.miniclip.framework.ThreadingContext` |
| Method name | Green | `queueEvent` |
| Field name | Magenta | `Main` |
| String content | Yellow | `"en-US"` |
| Numeric literals | Magenta | `42`, `0x2a`, `3.14f` |
| Boolean literals | Magenta | `true`, `false` |
| Null | Magenta | `null` |
| Method/field ID | Maroon | `0x4346` |
| Caller address | Lavender/Pink/Orange | `libfoo.so!0x1234` |
| Separators | Blue/Gray | `→`, `.`, `: `, `::` |

### Method calls

```
[CallStaticVoidMethodV]  com.x.Miniclip::queueEvent(com.x.ThreadingContext("Main"), com.x.NativeRunnable@f653cdb) @ libfoo.so!0x3d992e4
```

Instance methods show `this=`:
```
[CallObjectMethodV] this=java.util.HashMap$EntrySet@5cac94d java.util.Set::iterator() → java.util.HashMap$EntryIterator @ libfoo.so!0x3d98d5c
```

### Field access

Static fields — no `this=`, class name from receiver:
```
[GetStaticObjectField] com.x.ThreadingContext.Main: com.x.ThreadingContext → com.x.ThreadingContext("Main") @ libfoo.so!0x3d9c23c
```

Instance fields:
```
[GetObjectField] this=ApplicationInfo{5f6d975 io.smscash} nativeLibraryDir: java.lang.String → "/data/app/.../lib/arm64" @ libfoo.so!0x66920
```

### Lookups

```
[FindClass] "android/content/pm/ApplicationInfo" → android.content.pm.ApplicationInfo @ libfoo.so!0x668e4
[GetFieldID] android.content.pm.ApplicationInfo.nativeLibraryDir: java.lang.String → 0x7406efa025 @ libfoo.so!0x66908
[GetStaticMethodID] android.util.Log::d(String, String): int → 0x7406efa021 @ libfoo.so!0x1d3c8
```

### Exception events

```
[ExceptionCheck] () → false @ libfoo.so!0x3d9c318
[ExceptionOccurred] () → java.lang.NullPointerException: Attempt to invoke virtual method... @ libfoo.so!0x112868
```

### Array operations

```
[NewByteArray] (8) → [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00] @ libfoo.so!0x3d9b554
[GetArrayLength] ([0xde, 0x02, 0x64, 0x26, 0xfe, 0x7e, 0x01, 0x63, 0xed, 0xbc, 0xe0, 0x3a, 0x15, 0x39, 0xec, 0xec]) → 16 @ libfoo.so!0x3d9b5fc
[SetObjectArrayElement] (array=[com.x.weeklypremium, null +2 more], index=8, value=null) @ libfoo.so!0x3d9af68
```

### Void-returning calls

`→ void` is suppressed — the side effect is the point:
```
[DeleteLocalRef] (com.x.ThreadingContext("Main")) @ libfoo.so!0x3d988dc
[ReleaseByteArrayElements] (2, "free") @ libfoo.so!0x1234
```

---

## Configuration

Place a JSON file at `/data/local/tmp/jnilog.json` (or set `JNILOG_CONFIG` env var to a custom path).  Without a config file, **everything is logged** — identical to pre-config behavior.

### Schema

```json
{
  "functions":   [],       // Whitelist: exact JNI function names. Empty = all.
  "categories":  [],       // Category expansion merged into whitelist.
  "exclude": {
    "functions":  [],      // Blacklist: exact JNI function names to suppress.
    "categories": [],      // Blacklist category expansion.
    "regex":      []       // Regex patterns matched against call-key strings.
  },
  "array_items": 16        // Max array elements before "+N more" truncation.
}
```

### Available categories

| Category | Member count | What it includes |
|---|---|---|
| `methods` | 93 | All `Call*Method`, `CallStatic*Method`, `CallNonvirtual*Method`, `NewObject` |
| `fields` | 36 | All `Get/Set*Field`, `Get/SetStatic*Field` |
| `exceptions` | 7 | `Throw`, `ThrowNew`, `Exception*`, `FatalError` |
| `arrays` | 44 | All `New*Array`, `Get/Set/Release*Array*`, `GetArrayLength`, `SetObjectArrayElement` |
| `strings` | 10 | All `*String*` operations |
| `refs` | 10 | All `*GlobalRef`, `*LocalRef`, `IsSameObject`, `Push/PopLocalFrame`, `EnsureLocalCapacity` |
| `lookups` | 5 | `FindClass`, `GetMethodID`, `GetStaticMethodID`, `GetFieldID`, `GetStaticFieldID` |

### Three-tier gate system

```
JNI call enters hook
        │
        ▼
  Gate 1:  is function name in exclude.functions/categories?
  (C side, O(1) map lookup via cached result)
        │
  Yes ───┴──→  skip (cheapest bail)
        │
  No
        │
        ▼
  Gate 2:  is function name in functions/categories?
  (C side, O(1) map lookup; nil set = everything passes)
        │
  No ───┴──→  skip
        │
  Yes
        │
        ▼
  (Build wire format, execute original JNI, push to Go)
        │
        ▼
  Gate 3:  does the call-key match any exclude.regex pattern?
  (Go side, compiled regex match)
        │
  Yes ───┴──→  suppress line
        │
  No
        │
        ▼
  Write formatted log line
```

### Example configs

**Suppress noise categories:**
```json
{
  "exclude": {
    "categories": ["refs", "strings"]
  }
}
```

**Focus on methods and fields only:**
```json
{
  "categories": ["methods", "fields", "lookups"]
}
```

**Ignore all `iterator()` calls:**
```json
{
  "exclude": {
    "regex": ["::iterator\\("]
  }
}
```

**Everything except one package:**
```json
{
  "categories": ["methods", "fields", "lookups", "exceptions"],
  "exclude": {
    "regex": ["^(?!.*\\|?com\\.myapp\\.).*$"]
  }
}
```

**Deep array inspection:**
```json
{
  "array_items": 64
}
```

### Call-key format for regex filtering

`exclude.regex` patterns are matched against plain-text keys (no colors, no truncation).  Keys use `|` to separate the JNI function name from context:

| Event | Key format |
|---|---|
| Instance method | `CallObjectMethod\|com.x.Foo::bar(int, java.lang.String): void` |
| Static method | `CallStaticVoidMethod\|com.x.Bar::baz(): java.lang.String` |
| Field get (static) | `GetStaticObjectField\|com.x.Singleton.instance: com.x.Singleton` |
| Field set (instance) | `SetIntField\|com.x.Counter.count: int` |
| FindClass | `FindClass\|android.content.pm.ApplicationInfo` |
| GetMethodID | `GetMethodID\|java.util.Set::iterator(): java.util.Iterator` |
| RegisterNatives | `RegisterNatives\|com.x.nativeJNI.CocoJNI` |
| NewObject | `NewObject\|com.x.Runnable::<init>(long): void` |
| Utility hooks | `DeleteLocalRef`, `NewByteArray`, `ReleaseBooleanArrayElements` |

**Regex examples:**

| Goal | Pattern |
|---|---|
| One function | `^CallObjectMethod\|` |
| Function family | `^Release.*ArrayElements$` |
| Everything on a class | `\|com\.miniclip\.` |
| A specific method | `::iterator\(` |
| All static field sets | `^SetStatic.*Field\|` |
| Null exception checks | `\|\(none\)$` |

---

## Building

**Requirements:**
- Android NDK (for `aarch64-linux-android21-clang`) — set `ANDROID_NDK_HOME` or `ANDROID_NDK`
- Go ≥ 1.26 with `CGO_ENABLED=1` (set automatically by the xmake target)
- xmake (build orchestration)

```bash
# Build the payload library
xmake b jnilog
# → dist/libjnilog.so (ARM64)

# Push to a connected device
xmake push
```

The payload is a **cgo shared library** — `go build -buildmode=c-shared` produces a
single `.so` containing the Go runtime plus the C hook layer, linking
`encoding/json`, `regexp`, and the Go runtime statically.

---

## Loading the payload into a target process

This repository ships only the payload `.so` — loading it into a target process
is intentionally out of scope. Pick the injection vector that fits your context:

- **`LD_PRELOAD`** — works for a fresh fork of an executable you control. The
  payload constructor detects this case via the `LD_PRELOAD` env var and
  initializes synchronously instead of using a background thread.
- **Zygote-time injection** — patch `__loader_dlopen` in zygote's `libdl.so` GOT
  so every spawned app inherits the loader hook. The shipped payload includes
  this self-installing logic in `library_constructor`, but it requires that the
  payload itself already be loaded into zygote (separate boot-time tool).
- **Process-attached injection** — `ptrace`-based injectors (e.g. parasite-style
  shellcode) that force-load the payload into a running PID.
- **Zygisk module** — wrap the payload as a Zygisk module so Magisk handles the
  zygote injection lifecycle.

On load, the constructor runs through `init_once_handler` (`main.c`):
1. Resolve `JNI_GetCreatedJavaVMs` / `JNI_OnLoad` in `libart.so`.
2. Run `bridge_init` (Go bridge + ART symbol resolution).
3. Patch `__loader_dlopen` / `__loader_android_dlopen_ext` in `libdl.so`.
4. If a `JavaVM` already exists, install the JNI-table hooks immediately;
   otherwise wait for `JNI_OnLoad` / `GetEnv` to provide one.

Per-process config is read from `/data/local/tmp/jnilog.json` (or
`JNILOG_CONFIG=<path>`); see [`docs/CONFIG_SCHEMA.json`](docs/CONFIG_SCHEMA.json).

---

## Project Structure

```
jnilog/
├── src/
│   ├── cbridge/                   # C layer (hooks, injection, visualization)
│   │   ├── main.c                 # Constructor, PLT/GOT patching, VM hooks
│   │   ├── bridge.c/.h            # ART symbol resolution, Go↔C funnel functions
│   │   ├── hooks.c                # JNI table slot assignments + utility hooks
│   │   ├── hook_methods.c         # Call*Method variants (X-macro generated)
│   │   ├── hook_fields.c          # Get/Set*Field variants (X-macro generated)
│   │   ├── hook_logging.c         # Method/field log contexts, wire encoding dispatch
│   │   ├── hook_common.c          # Caches, reentrancy guards, config query cache
│   │   ├── hook_internal.h        # Shared types, enums, X-macro type lists
│   │   ├── visualize.c/.h         # JNI object introspection + typed-arg wire encoding
│   │   └── rangeset.c             # dl_iterate_phdr-based caller-range filter
│   └── go/                        # Go layer (rendering, config, types)
│       ├── main.go                # Cgo callbacks, callFrame stack, bridge init
│       ├── init.go                # Process init, package-name resolution
│       ├── logger.go              # Colorized formatter, all emit_* functions
│       ├── value.go               # JNIValue type system, decodeArgs, buildReturnValue
│       ├── config.go              # JSON config parsing, cgo exports, call-key builder
│       ├── signature.go           # JNI signature parser
│       ├── rangeset.go            # Thin Go wrappers for C rangeset
│       ├── visualize.go           # Thin Go wrappers for C visualize functions
│       ├── cbridge_all.c          # cgo aggregator that compiles src/cbridge/*.c
│       ├── ndk_compat.h           # GCC-only shim so host gopls can parse NDK headers
│       └── types_host.go          # !android mirror of formatter/decoder for host tests
├── docs/
│   └── CONFIG_SCHEMA.json         # JSON Schema for config file
├── xmake.lua                      # Build orchestration
└── README.md
```

---

## Key Design Decisions

### C ↔ Go separation via typed wire protocol
The C hook layer captures raw JNI data and encodes it into a byte-delimited wire format.  Go receives fully-typed **JNIValue** structs with no string parsing — the wire protocol is the single contract between C and Go:

```
C:  sigChar \x01 value [ \x03 extra ] \x02
Go: decodeArgs() → []JNIValue{Kind, Int, Float, Str, Extra, Items}
```

All colorization, formatting, and output logic lives exclusively in Go.

### X-macro code generation
Three X-macro type lists (`JNI_PRIMITIVE_ARRAY_TYPES`, `JNI_FIELD_TYPES`, `JNI_INSTANCE_NONVOID_TYPES`) generate ~200 hook functions from single definitions.  Adding support for a new JNI primitive type requires changing exactly one macro.

### Reentrancy via TLS flag
Hooks call JNI to resolve object types for display (e.g. `toString()` on return values).  Without protection, these JNI calls would re-enter our own hooks → infinite recursion.  A thread-local `g_in_hook` flag short-circuits reentrant calls directly to the original JNI table.

### Per-thread callFrame stack
Nested JNI calls (e.g. `toString()` called by `vis_object_tostring` inside a `CallObjectMethod` hook) are tracked via a per-thread FIFO stack in Go.  Each `log_jni_call` pushes a frame, each `log_jni_return` pops it.  Return values are matched to the correct call site.

### Multi-sink logging
Lines are written to both `logcat` (`ANDROID_LOG_INFO`) and `stdout`.  `NO_COLOR=1` environment variable disables ANSI escapes for plain-text output.

### Fork-safe initialization
The library constructor must initialize in every forked child process.  A mutex-guarded `g_initialized` flag (process-local data, resets to 0 after fork) replaces `pthread_once` which survives fork and would skip init in children.

---

## Troubleshooting

**Injection times out ("timeout identifying child process"):**  
The most common cause is a previous injection that left zygote in a bad state.  Reboot the device.  The `pthread_once` → mutex-guarded flag fix prevents this from recurring.

**No JNI logs appear:**  
Check that the app actually loads native libraries and makes JNI calls.  Some apps are pure Java/Kotlin and never touch JNI.  The config file may be blacklisting everything — verify with an empty `{}` config.

**App crashes on injection:**  
Some apps (especially games with anti-tamper like AppSealing, Jiagu, DexProtector) will detect the injected library and kill the process.  This is expected behavior for protected apps.

**"Failed to resolve art::ArtField::GetName" warning:**  
The library tries three mangled symbol names for `art::ArtField::GetName` across ART versions.  If none resolve, field names fall back to the method/field ID cache (populated when `GetFieldID`/`GetStaticFieldID` hooks fire).  Unresolved fields show `<unresolved>` in red.

**Build fails with "conflicting types for config_function_*":**  
You have a stale `_cgo_export.h` or `bridge.h` declaring cgo exports.  Delete the build directory and rebuild from scratch: `rm -rf .xmake build dist && xmake b jnilog`.

---

## License

MIT
