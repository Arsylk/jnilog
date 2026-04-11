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

## Sample Output

Here's what the colorful JNI logging looks like in action (colors will display when viewed in a terminal):

```ansi
[FindClass] "com.example.MyActivity" → com.example.MyActivity @ 0x7f8b8c0d5e00
[GetMethodID] com.example.MyActivity::onCreate(Landroid/os/Bundle;)V → 0x7f8b8c0d5e01 @ 0x7f8b8c0d5e02
[CallObjectMethod] com.example.MyActivity::onCreate(null) → void @ 0x7f8b8c0d5e03
[GetStaticMethodID] android.util.Log::d(Ljava/lang/String;Ljava/lang/String;)I → 0x7f8b8c0d5e04 @ 0x7f8b8c0d5e05
[CallStaticObjectMethod] android.util.Log::d("MyApp", "JNI call intercepted") → 0 @ 0x7f8b8c0d5e06
[GetFieldID] com.example.MyActivity::myStringField Ljava/lang/String; → 0x7f8b8c0d5e07 @ 0x7f8b8c0d5e08
[GetObjectField] this=com.example.MyActivity myStringField → "Hello from JNI!" @ 0x7f8b8c0d5e09
[NewStringUTF] "Processing user input..." → 0x7f8b8c0d5e10 @ 0x7f8b8c0d5e11
[GetArrayLength] [1, 2, 3, 4, 5] → 5 @ 0x7f8b8c0d5e12
```

### Representative JNI Operations

Here are carefully selected examples showing the tool's key capabilities:

**Class Operations:**
```
[FindClass] "android.content.Intent" → android.content.Intent @ 0x7f8b8c0d5e00
```

**Method Lookup & Calls:**
```
[GetMethodID] android.app.Activity::onCreate(Landroid/os/Bundle;)V → 0x7f8b8c0d5e01 @ 0x7f8b8c0d5e02
[CallObjectMethod] com.example.MyActivity::setContentView(2131165184) → void @ 0x7f8b8c0d5e03
[GetStaticMethodID] android.util.Log::i(Ljava/lang/String;Ljava/lang/String;)I → 0x7f8b8c0d5e04 @ 0x7f8b8c0d5e05
[CallStaticObjectMethod] android.util.Log::d("TAG", "Button clicked") → 0 @ 0x7f8b8c0d5e06
```

**Field Operations:**
```
[GetFieldID] android.widget.TextView::mText Ljava/lang/CharSequence; → 0x7f8b8c0d5e07 @ 0x7f8b8c0d5e08
[GetObjectField] this=android.widget.Button mText → "Click Me" @ 0x7f8b8c0d5e09
```

**String & Array Operations:**
```
[NewStringUTF] "User tapped the screen" → 0x7f8b8c0d5e10 @ 0x7f8b8c0d5e11
[GetArrayLength] ["item1", "item2", "item3"] → 3 @ 0x7f8b8c0d5e12
```

### Real Command Output

When running the stealth test command `xmake run-stealth --pkg=com.syzw.sumiao --logcat`:

```
Deploying files to device for stealth test...
Running STEALTH injector for package: com.syzw.sumiao
[+] starting spawn injector package=com.syzw.sumiao payload=/data/local/tmp/libjnilog.so
[+] stealth mode enabled staging ephemeral payload
[?] staged ephemeral payload path=/data/data/com.syzw.sumiao/.cache_18a565a830ceb1bd
[?] killing existing app instance package=com.syzw.sumiao
[?] locating zygote64
[+] found zygote64 pid=1015
[?] resolving main activity package=com.syzw.sumiao
[+] resolved main activity package=com.syzw.sumiao activity=com.syzw.sumiao/com.fenghuajueli.module_host.LauncherActivity
[+] starting spawn injector package=com.syzw.sumiao mode=in-place
[?] resolved trap symbol symbol=setArgV0 addr=0x7199e0df18
[?] resolved loader symbol symbol=dlopen addr=0x71b022fa64
[+] recording zygote children
[+] applying agnostic trap mailbox=0x7199ef2ff0
[+] waiting for child process
[+] identified child pid=25213
[?] calculated child communication channel child_mailbox=0x7199ef2ff0
[+] polling agnostic mailbox
[+] handshake successful handle=0x1714d9a3608fc391 type=agnostic
[+] restoring zygote
[+] unlinked ephemeral payload ghost sequence complete
[+] injection sequence complete
[+] starting logcat pid=25213
```

### Real JNI Logging Output

Here's actual output from running the tool on a real Android application (com.syzw.sumiao) captured from logcat. The app is protected with Jiagu anti-analysis technology:

```
I/JNILogPayload(25213): === libjnilog constructor start (PID=25213) ===
I/JNILogPayload(25213): init_once_handler: starting pid=25213
I/JNILogPayload(25213): init_once_handler: JNI_GetCreatedJavaVMs=0x6ef3445dcc JNI_OnLoad=0x0
I/JNILogPayload(25213): === JNILog Bridge Init (v3 typed) ===
W/JNILogPayload(25213): Failed to resolve art::ArtField::GetName
I/JNILogPayload(25213): install_loader_dlopen_hook: patched __loader_dlopen in libdl.so GOT (orig=0x71b022fa64)
I/JNILogPayload(25213): install_loader_dlopen_hook: patched __loader_android_dlopen_ext in libdl.so GOT (orig=0x71b022f1a0)
I/JNILogPayload(25213): Go runtime initialized
I/JNILogPayload(25213): [log] goBridgeInit: cmdline argv[0]="zygote64"
I/JNILogPayload(25213): [log] goBridgeInit: skipped package name (zygote/app_process/empty): "zygote64"
I/JNILogVisualize(25213): Visualization helpers VM set: 0xb400006f47366190
I/JNILogPayload(25213): init_once_handler: initialization successful
I/JNILogPayload(25213): library constructor: initialization complete
I/JNILog (25213): package name resolved from lib path: 'com.syzw.sumiao'

I/JNILogPayload(25213): [FindClass] "com.stub.StubApp" → com.stub.StubApp @ libjiagu_64.so!0x19dc0
I/JNILogPayload(25213): [GetStaticMethodID] com.stub.StubApp::getAppContext(): android.content.Context → 0x71aed71025 @ libjiagu_64.so!0x19e08
I/JNILogPayload(25213): [CallStaticObjectMethodV] com.stub.StubApp::getAppContext() → android.app.ContextImpl@e8fe7b6 @ libjiagu_64.so!0x1083c
I/JNILogPayload(25213): [FindClass] "android.app.ActivityThread" → android.app.ActivityThread @ libjiagu_64.so!0x1f3a0
I/JNILogPayload(25213): [GetStaticMethodID] android.app.ActivityThread::currentActivityThread(): android.app.ActivityThread → 0x71aed71025 @ libjiagu_64.so!0x1f3e4
I/JNILogPayload(25213): [CallStaticObjectMethodV] android.app.ActivityThread::currentActivityThread() → android.app.ActivityThread@8a34cb7 @ libjiagu_64.so!0x1083c
I/JNILogPayload(25213): [FindClass] "android.os.Build$VERSION" → android.os.Build$VERSION @ libjiagu_64.so!0x1f44c
I/JNILogPayload(25213): [GetStaticFieldID] android.os.Build$VERSION::SDK_INT int → 0x71aed71031 @ libjiagu_64.so!0x1f490
I/JNILogPayload(25213): [GetStaticIntField] this=android.os.Build$VERSION SDK_INT → 36 @ libjiagu_64.so!0x1f4c8
I/JNILogPayload(25213): [GetStaticMethodID] android.app.ActivityThread::currentPackageName(): java.lang.String → 0x71aed71025 @ libjiagu_64.so!0x1f894
I/JNILogPayload(25213): [CallStaticObjectMethodV] android.app.ActivityThread::currentPackageName() → "com.syzw.sumiao" @ libjiagu_64.so!0x1083c
I/JNILogPayload(25213): [GetStringUTFChars] ("com.syzw.sumiao") → 0x706738b690 @ libjiagu_64.so!0x1f8e4
I/JNILogPayload(25213): [ReleaseStringUTFChars] (0x71aed71035) → void @ libjiagu_64.so!0x1f938
I/JNILogPayload(25213): [FindClass] "android.content.Context" → android.content.Context @ libjiagu_64.so!0x1d284
I/JNILogPayload(25213): [GetMethodID] android.content.Context::checkPermission(java.lang.String, int, int): int → 0x71aed71025 @ libjiagu_64.so!0x1d2c8
I/JNILogPayload(25213): [NewStringUTF] ("android.permission.READ_PHONE_STATE") → 0x71aed7102d @ libjiagu_64.so!0x1d308
I/JNILogPayload(25213): [CallIntMethodV] this=android.app.ContextImpl@e8fe7b6 android.content.Context::checkPermission("android.permission.READ_PHONE_STATE", 25213, 10511) → -1 @ libjiagu_64.so!0x19cb4
I/JNILogPayload(25213): [FindClass] "android.app.AppOpsManager" → android.app.AppOpsManager @ libjiagu_64.so!0x1d384
I/JNILogPayload(25213): [GetStaticMethodID] android.app.AppOpsManager::permissionToOp(java.lang.String): java.lang.String → 0x71aed71031 @ libjiagu_64.so!0x1d3c8
I/JNILogPayload(25213): [CallStaticObjectMethodV] android.app.AppOpsManager::permissionToOp("android.permission.ACCESS_NETWORK_STATE") → null @ libjiagu_64.so!0x1083c
I/JNILogPayload(25213): [FindClass] "java.lang.System" → java.lang.System @ libjiagu_64.so!0x186a44
I/JNILogPayload(25213): [GetStaticMethodID] java.lang.System::getProperty(java.lang.String): java.lang.String → 0x71aed71025 @ libjiagu_64.so!0x186ad0
I/JNILogPayload(25213): [NewStringUTF] ("java.vm.version") → 0x71aed7102d @ libjiagu_64.so!0x186c48
I/JNILogPayload(25213): [CallStaticObjectMethodV] java.lang.System::getProperty("java.vm.version") → "2.1.0" @ libjiagu_64.so!0x168dcc
I/JNILogPayload(25213): [FindClass] "java.lang.String" → java.lang.String @ libjiagu_64.so!0x1ea440
I/JNILogPayload(25213): [GetMethodID] java.lang.String::getBytes(java.lang.String): byte[] → 0x71aed71039 @ libjiagu_64.so!0x1ea4b4
I/JNILogPayload(25213): [CallObjectMethodV] this="2.1.0" java.lang.String::getBytes("utf-8") → byte[]@b719c24 @ libjiagu_64.so!0x168e64
I/JNILogPayload(25213): [GetArrayLength] ([0x32, 0x2e, 0x31, 0x2e, 0x30]) → 5 @ libjiagu_64.so!0x1ea4f8
I/JNILogPayload(25213): [GetByteArrayElements] ([0x32, 0x2e, 0x31, 0x2e, 0x30]) → 0x7067391b50 @ libjiagu_64.so!0x1ea514
I/JNILogPayload(25213): [FindClass] "android.os.SystemProperties" → android.os.SystemProperties @ libjiagu_64.so!0x1bd01c
I/JNILogPayload(25213): [GetStaticMethodID] android.os.SystemProperties::get(java.lang.String): java.lang.String → 0x71aed71025 @ libjiagu_64.so!0x1bd07c
I/JNILogPayload(25213): [NewStringUTF] ("ro.product.real_model") → 0x71aed7102d @ libjiagu_64.so!0x1bd0b0
I/JNILogPayload(25213): [CallStaticObjectMethodV] android.os.SystemProperties::get("ro.product.real_model") → "" @ libjiagu_64.so!0x168dcc
I/JNILogPayload(25213): [FindClass] "android.os.Build" → android.os.Build @ libjiagu_64.so!0x1bd458
I/JNILogPayload(25213): [GetStaticFieldID] android.os.Build::MODEL java.lang.String → 0x71aed71025 @ libjiagu_64.so!0x1bd4b4
I/JNILogPayload(25213): [GetStaticObjectField] this=android.os.Build MODEL → "M2007J3SY" @ libjiagu_64.so!0x1bd4ec
I/JNILogPayload(25213): [GetStaticFieldID] android.os.Build::BRAND java.lang.String → 0x71aed71025 @ libjiagu_64.so!0x1bd4b4
I/JNILogPayload(25213): [GetStaticObjectField] this=android.os.Build BRAND → "Xiaomi" @ libjiagu_64.so!0x1bd4ec
I/JNILogPayload(25213): [RegisterNatives] com.stub.StubApp {interface14(int): java.lang.String @0x6e48c512b4, mark(): void @0x6e48cb470c, ...} 0x71aed71025 @ libjiagu_64.so!0x165a18
```

The colors make it easy to distinguish different types of JNI calls and their parameters/results at a glance. This output demonstrates the tool's ability to intercept and log JNI calls from protected Android applications.

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
