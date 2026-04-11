# JNILog Payload

A standalone native library for logging JNI calls with ANSI colors, inspired by frida-jnitrace but implemented primarily in Go with a minimal C bridge for things that cannot be done in Go.

## Overview

JNILog intercepts JNI function calls via LD_PRELOAD hooking and logs them with colorful output similar to frida-jnitrace. It's designed to work on Android ARM64 systems as a standalone shared library without requiring Frida.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Target Process                        │
│  ┌──────────────────────────────────────────────────┐  │
│  │           JNI Calls (FindClass, GetMethodID)     │  │
│  └──────────────┬───────────────────────────────────┘  │
│                 │                                      │
│                 ▼                                      │
│  ┌──────────────────────────────────────────────────┐  │
│  │           JNIEnv VTable (Hooked)                 │  │
│  │  • Function pointers replaced by our hooks       │  │
│  │  • Calls forwarded to original implementations   │  │
│  └──────────────┬───────────────────────────────────┘  │
│                 │                                      │
│                 ▼                                      │
│  ┌──────────────────────────────────────────────────┐  │
│  │           C Bridge (LD_PRELOAD)                  │  │
│  │  • Minimal C code for function interception      │  │
│  │  • Marshals arguments to Go                      │  │
│  │  • Calls original JNI functions                  │  │
│  └──────────────┬───────────────────────────────────┘  │
│                 │                                      │
│                 ▼                                      │
│  ┌──────────────────────────────────────────────────┐  │
│  │           Go Logging Engine                      │  │
│  │  • ANSI color formatting                         │  │
│  │  • JNI signature parsing                        │  │
│  │  • Call stack tracking                          │  │
│  │  • Output to stdout/logcat                      │  │
│  └──────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

## Current Status

- **Working build**: `xmake -P . -y` produces a single deployable `build/libjnilog.so`
- **Hook installation**: JNI table copy-and-swap is implemented from `JNI_GetCreatedJavaVMs` / `JNI_OnLoad`
- **Go logging pipeline**: ANSI logging and JNI signature parsing are implemented
- **Current hook coverage**:
  - Class/reference operations: `FindClass`, `NewGlobalRef`
  - Method lookup/call operations: `GetMethodID`, `GetStaticMethodID`, `CallObjectMethod`, `CallStaticObjectMethod`
  - Field lookup operations: `GetFieldID`, `GetStaticFieldID`
  - String operations: `NewStringUTF`, `GetStringUTFChars`
  - Array operations: `GetArrayLength`, `NewBooleanArray`
- **Not finished yet**:
  - Field value hooks such as `GetObjectField` / `SetObjectField`
  - Additional generic hooks such as `DefineClass`
  - Broader call families beyond the current object-returning calls
  - End-to-end validation against a sample Android JNI app

## Features

- **Colorful Output**: ANSI color coding matching frida-jnitrace:
  - Class names: cyan
  - Method names: green
  - Strings: yellow
  - Numbers: magenta
  - Keywords: specific colors

- **Minimal Dependencies**: Pure Go with minimal C bridge, no Frida required

## Build Requirements

- Android NDK (for cross-compilation to ARM64)
- Go 1.20+ with CGO enabled
- ARM64 Linux/Android target

## Building

```bash
xmake -P . -y
```

Build artifacts are written to `build/`.

## Usage

```bash
# Set LD_PRELOAD and run target app
LD_PRELOAD=/path/to/libjnilog.so ./target-app

# For Android apps
LD_PRELOAD=/data/local/tmp/libjnilog.so am start -n com.example.app/.MainActivity
```

## Project Structure

```text
jnilog_payload/
├── README.md
├── xmake.lua
├── src/
│   ├── cbridge/              # Minimal C — only things impossible in Go
│   │   ├── bridge.c          # Thin wrappers delegating to Go exports
│   │   ├── bridge.h          # C API header
│   │   ├── hooks.c           # JNI vtable hooks (va_list, __builtin_return_address)
│   │   └── main.c            # LD_PRELOAD entry, dlopen/mprotect interposition
│   ├── go/                   # All logic lives here
│   │   ├── cbridge_bridge.c  # #include stub for cgo
│   │   ├── cbridge_hooks.c   # #include stub for cgo
│   │   ├── cbridge_main.c    # #include stub for cgo
│   │   ├── rangeset_helper.c # dl_iterate_phdr callback (C, calls Go exports)
│   │   ├── init.go           # One-shot initialization orchestration
│   │   ├── rangeset.go       # Exec range registry + package name tracking
│   │   ├── logger.go         # ANSI color logging engine
│   │   ├── main.go           # Go exports + JNI callback dispatch
│   │   └── signature.go      # JNI signature parser
│   └── shared/
│       └── types.h
└── build/
```

## Implementation Notes

1. **LD_PRELOAD approach**: The library intercepts `JNI_GetCreatedJavaVMs` and `JNI_OnLoad`, acquires a live `JNIEnv*`, copies the original JNI function table, and swaps in a hooked table.

2. **Go-first architecture**: All data logic (exec range tracking, package name filtering, logging, init orchestration) lives in Go. The C layer is a thin shell for things Go cannot do: `__attribute__((constructor))`, `dlopen`/`mprotect` symbol interposition, `va_list` forwarding in JNI hooks, and `__builtin_return_address` for caller identification.

3. **Current dispatch model**: Hooks call `log_jni_call(...)`, invoke the original JNI function, then call `log_jni_return(...)`. These forward to Go exports for formatting and output.

4. **Verification status**: The Android-targeted build passes, but sample-app runtime validation is still pending.

## Remaining Work

- Add field value hooks
- Add broader JNI call families and generic hooks
- Test on a sample Android JNI application
- Refine output formatting from real runtime traces

## License

MIT

## References

- [frida-jnitrace](https://github.com/iddoeldor/frida-jnitrace)
- [Android JNI Documentation](https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/jniTOC.html)
- [LD_PRELOAD Trickery](https://rafalcieslak.wordpress.com/2013/04/02/dynamic-linker-tricks-using-ld_preload-to-cheat-inject-features-and-investigate-programs/)
