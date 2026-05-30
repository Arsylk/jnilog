# Phase 0 — Baseline (captured 2026-05-31)

## Toolchain / environment (validated this session)
- Device `b83607a5` Xiaomi M2007J3SY (apollo), Android 16 / API 36, arm64-v8a, rooted (Magisk + APatch), `vma_hide` present.
- Go 1.26.3, NDK r29 at `/opt/android-ndk`, xmake 3.0.8.
- Loader: `/opt/github/gozinject` (root injector, vma_hide, no ptrace).

## 0.1 Baseline build
- `xmake b jnilog` → `dist/libjnilog.so` ELF ARM64. See `build.txt` for sha256/size.
  - sha256 `673818b4d4bd6437292620ccf173d656cace9f1af67991d093a4d65b74341849`, size 4623464.

## 0.2 Baseline host tests
- `go vet ./...` clean; `go test ./... -count=1` → ok (62 test funcs). Full log: `hosttest.txt`.

## 0.3 Baseline live capture
- Fixed config: `jnilog.json` = `{"categories":["methods","fields","lookups"],"array_items":16}` (pushed to `/data/local/tmp/jnilog.json`).
- Inject: from `/opt/github/gozinject`, `xmake run --lib=/opt/github/jnilog/dist/libjnilog.so --pkg=com.openai.chatgpt` (no `--logcat`; it injects and returns).
- Capture: `adb -s b83607a5 logcat -v brief -s JNILogPayload` → `live_before.raw.log` (1233 rendered lines).
- Rendering verified clean: FindClass / NewGlobalRef / GetMethodID / CallObjectMethodV / field access all correct; library-relative offsets (`@ libpairipcore.so!0xNNNN`) are stable across runs → diffable.

## IMPORTANT: com.openai.chatgpt crash is pairip anti-tamper, NOT a jnilog defect
- Injected child SIGSEGV'd (`SEGV_ACCERR`, fault `0x7f9219f000`, `libpairipcore.so` offset `0x1ad0000`, pc `0x57a98`) ~2s after launch, right after emitting `play.integrity.autoprotect.LOG_TELEMETRY` broadcasts.
- **Reproduced identically with NO injection** (`am start` only): same signal, same fault addr, same offset. So the crash is pairip detecting the rooted/tampered environment and self-terminating. It is independent of jnilog.
- Consequence for the regression gate: for `com.openai.chatgpt` the success metric is **"jnilog still injects and logs the same ~1233 consistent lines of pairip JNI activity, and adds NO crash of its own"** (no crash frame in `libjnilog.so`, no `JNI ERROR` from our hooks, no ART abort from our refs) — NOT "app does not crash." The remediation doc's §9.3 wording ("confirm it still doesn't crash") is imprecise on this point.
- For the long-run (>=10 min) object-heavy gref-stability tests (F1/F6/F24) a *different* target that stays alive is required; com.openai.chatgpt dies in ~2s.

## 0.5 Recordings
- `rec.ascii` (43 MB) / `rec2.ascii` (14 MB) are asciinema recordings of prior com.openai.chatgpt capture sessions. Moved to `docs/baseline/` and `*.ascii` gitignored (F15).
