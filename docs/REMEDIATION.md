# JNILog — Remediation & Refactor Plan

> Source: deep multi-agent analysis session (5 validation agents over the C hook layer,
> injection/init layer, Go formatter/config layer, event_pipe + visualize, and
> tests/build/docs). Every CRITICAL/HIGH item below was cross-checked against source.
>
> Goal: drive the project to state-of-the-art with **no known correctness, memory-safety,
> reliability, fork-safety, test-honesty, or documentation defects**.
>
> Target: Android ARM64 (`arm64-v8a`), single cgo `c-shared` `.so`. The C layer compiles
> as **one translation unit** via `src/go/cbridge_all.c` — keep that invariant in mind for
> every C change (file-local `static` symbols are visible across the concatenated TU).
>
> Status legend: ☐ not started · ◑ in progress · ☑ done · ⊘ won't-fix (justify)

---

## 0. How to use this document

1. Work **phase by phase**. Phases are ordered by (risk reduction × blast radius).
2. Before any code change, capture a **baseline** (§9.1): build, host tests, and a live
   `gozinject` capture into `com.openai.chatgpt` / a test app on the connected device
   (`b83607a5`, Android 16, arm64-v8a).
3. After each phase: rebuild, re-run host tests, re-run the live capture, and **diff the
   rendered output** against the baseline to catch regressions (§9).
4. Check off items inline. Keep the "Verification" note for each item honest — if it was
   only host-tested and not device-tested, say so.

---

## 1. Severity summary (what "sub-prime" meant)

| # | Severity | Title | Files | Phase |
|---|----------|-------|-------|-------|
| F1 | 🔴 CRITICAL | Global-ref leak on dropped/oversized/non-attached datagram → JVM gref-table exhaustion → crash | `event_pipe.c`, `event_pipe.go` | 1 |
| F2 | 🔴 CRITICAL | Fork-safety premise false (`static g_initialized` does not reset across `fork()`) | `main.c`, `bridge.c`, `rangeset.c`, `init.go` | 2 |
| F3 | 🔴 HIGH | GOT page restored to hardcoded `PROT_READ` over a gratuitous 2-page span | `main.c` | 2 |
| F4 | 🔴 HIGH | `design.md`/`requirements.md`/`tasks.md`/README stale re: socket transport | docs | 6 |
| F5 | 🔴 HIGH | Tests validate drifted host mirror; production socket path has zero coverage | `types_host.go`, `event_pipe.go`, `*_test.go` | 5 |
| F6 | 🟠 MEDIUM | `pendingCalls` leaks unpaired CALL frames | `event_pipe.go`, `bridge.c` | 1 |
| F7 | 🟠 MEDIUM | 512-byte per-field truncation desyncs `\x1A<n>` placeholder ↔ sidecar | `event_pipe.c` | 1 |
| F8 | 🟠 MEDIUM | Placeholder slot ASCII `'0'+n` only safe while `EVENT_PIPE_MAX_REFS<=8` | `hooks.c`, `hook_fields.c`, `hook_logging.c`, `event_pipe.go` | 1 |
| F9 | 🟠 MEDIUM | Wire string values split on raw `\x01`–`\x04`; delimiter-in-string corrupts framing | `value.go`, `visualize.c` | 3 |
| F10 | 🟠 MEDIUM | Cache read path returns raw pointers under released rwlock; torn-string race | `hook_common.c`, `hook_logging.c` | 3 |
| F11 | 🟠 MEDIUM | Process-wide `mprotect` interposer returns `-1` w/o errno; does fopen/lock on hot path | `main.c` | 2 |
| F12 | 🟡 LOW | `/proc/self/maps` lines truncated at 512 B drop long-pathed libs | `main.c`, `rangeset.c` | 4 |
| F13 | 🟡 LOW | Anonymous executable (JIT/packed) regions never added to range set | `rangeset.c` | 4 |
| F14 | 🟡 LOW | `g_seed_attempted` is `volatile`, not atomic | `rangeset.c` | 4 |
| F15 | 🟡 LOW | 57 MB untracked `rec*.ascii` in repo root; `.gitignore` gap | repo | 6 |
| F16 | 🟡 LOW | `go build -a` forces full rebuild each invocation | `xmake.lua` | 6 |
| F17 | 🟡 LOW | README `strings`=10 (code 12); `design.md` `refs`=9 (code 10); "FIFO" should be LIFO | docs | 6 |
| F18 | 🟡 LOW | `cfg_cache` double Go round-trip on racing first lookup; never freed | `hook_common.c` | 3 |
| F19 | 🟡 LOW | `tls_last_call_id` reached via `extern` redecl of a `static` (works only via concat TU) | `hooks.c`, `bridge.c` | 3 |
| F20 | 🟡 LOW | `lookup_field_info` calls ART `GetName` even on cache hit; shared TLS buffer lifetime undocumented | `hook_common.c` | 3 |
| F21 | 🟡 LOW | `is_self_symbol` / `k_log_tag` dead code | `main.c`, `bridge.c` | 6 |
| F22 | 🟡 LOW | `%g` lossy/locale-sensitive; `'D'` memcpy assumes LP64 | `visualize.c` | 3 |
| F23 | 🟡 LOW | VM-table publish race: `memcpy` into the about-to-be-published `g_hooked_vm_table` | `main.c` | 2 |
| F24 | 🟡 LOW | No `SO_RCVBUF`/`SO_SNDBUF` readback; no drop counter; no reader shutdown | `event_pipe.c`, `event_pipe.go` | 1 |

---

## 2. Phase 0 — Safety net (do first, no behavior change)

**Objective:** make regressions detectable before changing anything.

- ☑ **0.1 Baseline build.** `xmake b jnilog` → `dist/libjnilog.so` ELF ARM64. sha256 + size recorded in `docs/baseline/build.txt`.
- ☑ **0.2 Baseline host tests.** `go vet ./...` clean, `go test ./... -count=1` ok (62 funcs). Full output in `docs/baseline/hosttest.txt`.
- ☑ **0.3 Baseline live capture.** Injected into `com.openai.chatgpt` via `../gozinject`, fixed config `docs/baseline/jnilog.json` = `{"categories":["methods","fields","lookups"],"array_items":16}`. 1233 rendered lines → `docs/baseline/live_before.raw.log`. **Finding (see `docs/baseline/NOTES.md`):** the chatgpt SIGSEGV (`SEGV_ACCERR` @ `libpairipcore.so` 0x1ad0000) reproduces **without injection** — it is pairip anti-tamper self-terminating on a rooted device, not a jnilog defect. Regression gate for this target = "still injects + logs the same ~1233 consistent lines, no crash frame in `libjnilog.so`, no `JNI ERROR`/ART abort from our hooks."
- ◑ **0.4 Stress config for leak repro.** Deferred to F1 phase; needs a longer-lived object-heavy target (chatgpt dies in ~2s). Will provoke F1/F6 drops and watch `dumpsys meminfo`/gref overflow there.
- ☑ **0.5 Snapshot recordings out of the way.** `rec.ascii`/`rec2.ascii` moved to `docs/baseline/`; `*.ascii` added to `.gitignore` (F15).

> Regression gate for the whole session: **host tests stay green**, **live render output is
> byte-identical except where a fix intentionally changes it**, and **no new logcat crashes**.

---

## 3. Phase 1 — Event-pipe reliability & ref-safety (🔴 highest value)

This phase fixes the failure-mode accounting that undermines the v1.0.3 stability claim.

### F1 — gref leak on drop/overflow/non-attach  🔴 CRITICAL
**Where:**
- `src/cbridge/event_pipe.c:60-69` — `event_pipe_defer_render_push` does `NewGlobalRef`.
- `src/cbridge/event_pipe.c:71-73` — `event_pipe_render_refs_reset` zeroes count, **no DeleteGlobalRef**.
- `src/cbridge/event_pipe.c:108-121` — `append_render_refs` overflow path resets without freeing.
- `src/cbridge/event_pipe.c:166-178` — `send_record` drops on EAGAIN; refs already serialized but datagram lost → consumer never frees.
- `src/go/event_pipe.go:206-218` — `dispatchObjReturn` leaks "by design" when `consumerEnv==nil`.

**Why it's critical:** the global-ref table is bounded (~51200). The architecture *intends*
to drop under load; every dropped object event leaks one+ gref. Sustained object-heavy
traffic with a lagging reader exhausts the table → `JNI ERROR: global reference table
overflow` → app abort. This reintroduces the exact instability v1.0.3 fixed.

**Fix (C side):** every path that abandons the sidecar must `DeleteGlobalRef` first. The
hook thread always has a valid `JNIEnv*`, so thread it into the emit/reset path.
- Add `void event_pipe_render_refs_drop(JNIEnv *env)` that iterates `g_tls_refs.refs[0..nrefs)`
  and `(*env)->DeleteGlobalRef(env, (jobject)refs[i])` then zeroes. Keep the no-arg
  `event_pipe_render_refs_reset()` **only** for the success path (where the consumer now owns them).
- In each `event_pipe_emit_*`, the early-return-on-overflow branches currently call
  `event_pipe_render_refs_reset()` → change to `event_pipe_render_refs_drop(env)`. This
  requires each emit to have `env` in scope; the hook-side callers already pass `env` to the
  visualize step — plumb it through to the emit signature (or stash the hook-thread env in the
  TLS sidecar struct at push time: add `JNIEnv *env;` to `struct tls_render_refs`, set it in
  `event_pipe_defer_render_push`, and use it in `_drop`). **TLS-stash is the lower-churn option.**
- In `send_record`: on send failure, the datagram (and thus the refs) is lost. The refs were
  already appended to the datagram by `append_render_refs`, which **already cleared the TLS
  sidecar**. So by the time `send_record` fails, the C side no longer holds the refs and the
  consumer will never get them. **Resolution:** do not clear the TLS sidecar inside
  `append_render_refs`; instead, have the emit function (a) build datagram, (b) `send_record`,
  (c) on success `event_pipe_render_refs_reset()` (consumer owns them), (d) on failure
  `event_pipe_render_refs_drop(env)`. This makes the ref ownership transfer atomic with send
  success. Refactor `append_render_refs` to read the TLS sidecar **without** resetting it.

**Fix (Go side, `dispatchObjReturn` non-attach):** if `consumerEnv==nil` we cannot
`DeleteGlobalRef`. Options, pick one and document:
  - (preferred) **Never enter this state**: gate object-deferral on a global "consumer
    attached" atomic the C side checks before `NewGlobalRef` — if the consumer never attached,
    don't defer (fall back to inline minimal rendering or emit a placeholder *without* a gref).
    Add `int event_pipe_consumer_ready(void)` set by the reader after a successful attach;
    `event_pipe_defer_render_push` returns -1 when not ready (callers already handle -1).
  - This **also** fixes the F1 non-attach leak and the F24 "reader never attached" silent hole.

**Acceptance:**
- Unit: a host-buildable fake that simulates push→drop asserts the drop path calls Delete.
- Device: run §0.4 stress capture for ≥10 min; `dumpsys meminfo` gref count stable; no
  global-ref-table-overflow in logcat. Compare against baseline (which should leak).

- ☑ F1 implemented · ☑ host-tested · ☑ device-tested
  **Implementation:** consumer-ready gate (`event_pipe_consumer_ready`/`event_pipe_set_consumer_ready`)
  so a gref is minted only when the reader is attached + draining; TLS sidecar stashes the hook
  thread's `env`; `append_render_refs` no longer resets; `finish_record()` makes ownership transfer
  atomic with send — `render_refs_reset()` on success (consumer frees), `render_refs_drop()`
  (DeleteGlobalRef each) on overflow/EAGAIN. Same gate+free-on-drop applied to the two standalone-gref
  paths: obj-return (`hooks.c` `_log_obj_ret`) and deferred lookup (`bridge.c` `log_jni_lookup_deferred`).
  Go reader sets ready after attach, clears on exit.
  **Device (2026-05-31, eightballpool, everything-on stress):** ~19k events/s; drop monitor logged
  `14289 events dropped total` (drop path heavily exercised); `globals=` gref count flat 15616→15856
  over 75s (limit ~51200), no overflow, injected app never crashed, **no libjnilog frame in any
  tombstone**. chatgpt happy-path op-frequency identical to baseline.

---

### F24 — observability for drops + buffer sizing + reader shutdown  🟡 (do with F1)
**Where:** `event_pipe.c:144-161` (init), `:166-178` (send), `event_pipe.go:71-104` (reader).
**Fix:**
- Add `static _Atomic uint64_t g_drop_count;` incremented in `send_record` on failure; expose
  `uint64_t event_pipe_drops(void)`; have the Go reader log it every N seconds or on exit.
- After `setsockopt(SO_RCVBUF/SO_SNDBUF)`, `getsockopt` the granted value and `LOG` it once so
  the real (kernel-clamped) buffer is known.
- Reader loop (`event_pipe.go:85`) has no shutdown; acceptable for process-lifetime, but add a
  comment stating it intentionally runs until read error / process exit.

**Acceptance:** logcat shows granted buffer sizes and periodic drop counts. ☑ done
**Implementation:** `g_drop_count` (`__atomic`) incremented in `send_record` on EAGAIN, exposed via
`event_pipe_drops()`; Go `eventPipeDropMonitor` goroutine logs cumulative+delta every 30s when changed
and the reader logs the final count on exit. `event_pipe_init` now `getsockopt`s the granted
SO_SNDBUF/SO_RCVBUF and logs them once. Reader-loop shutdown semantics documented inline.
**Device:** logcat showed `event_pipe: SNDBUF requested=1048576 granted=2097152; RCVBUF requested=1048576 granted=2097152`
and `event_pipe: 14289 events dropped total (+14289 in last 30s; consumer behind)`.

---

### F7 — 512-byte truncation desyncs placeholders/UTF-8/framing  🟠 MEDIUM
**Where:** `event_pipe.c:38` (`EV_STR_MAX_BYTES 512`), `:84-97` (`append_str_max` blind clip).
**Problem:** clipping mid-byte can cut a `\x1A`+digit marker, split a UTF-8 sequence, or drop a
closing `\x02`; the sidecar still ships all N refs → `substitutePlaceholders`/`decodeArgs`
mis-parse and render the wrong object. RegisterNatives already dodges this with a 2048 cap
(`:297`).
**Fix:**
- Treat the **structured** fields (`encoded_args`, and any field carrying `\x1A`/`\x01`–`\x04`)
  differently from **leaf** strings. Raise their cap to fit a full datagram (or clip only on a
  record boundary `\x02`, never inside a `\x1A`+digit pair or a UTF-8 continuation).
- Implement `append_str_struct()` that, when it must truncate, backs up to the last byte that is
  (a) not a lone `\x1A`, (b) not mid-UTF-8 (`(b & 0xC0) != 0x80`), (c) ends a complete record.
- Leaf strings (class name, method name, caller, string content) can keep a byte cap but should
  back off UTF-8 boundaries to avoid emitting invalid UTF-8 to Go (`string(buf)` tolerates it,
  but the renderer escapes it oddly).
- Reconsider `EVENT_MAX_BYTES 8192` vs the new caps so a maximal event still fits (assert at
  build: sum of max field sizes + prefixes ≤ `EVENT_MAX_BYTES`).

**Acceptance:** craft a method call with a >512-byte `toString()` containing multibyte chars and
a deferred object; rendered line must show the object correctly, no stray `\x1A`/replacement
chars. ☐ host-tested · ☐ device-tested

---

### F8 — placeholder slot encoded as ASCII `'0'+n`  🟠 MEDIUM (latent)
**Where:** `hooks.c` (≈94,128,551), `hook_fields.c:56`, `hook_logging.c:62`, decode in
`event_pipe.go:369` (`d >= '0' && d < '0'+len(rendered)`).
**Problem:** safe only because `EVENT_PIPE_MAX_REFS == 8`. Raising it past 10 makes slots encode
as `:;<…` and silently corrupts the stream.
**Fix (choose one):**
- (minimal) Add `_Static_assert(EVENT_PIPE_MAX_REFS <= 10, "placeholder slot is a single ASCII digit");`
  next to the macro in `event_pipe.h`.
- (robust) Encode the slot as a **raw byte** (`'\x1A', (uint8_t)n`) and decode `d := s[i+1]; if int(d) < len(rendered)`.
  Removes the ceiling entirely. Update both C emit sites and `substitutePlaceholders`.
  Prefer this — it also lets us raise `EVENT_PIPE_MAX_REFS` for F7-heavy events.

**Acceptance:** set `EVENT_PIPE_MAX_REFS` to 12 temporarily in a test build; multi-object call
renders all objects correctly. ☐ done

---

### F6 — `pendingCalls` leaks unpaired CALL frames  🟠 MEDIUM
**Where:** `event_pipe.go:395` (Store), deletion only via `LoadAndDelete` in
`dispatchReturn`/`dispatchObjReturn`. Root cause of *missing returns* includes the single-slot
`tls_last_call_id` (`bridge.c:142`) being clobbered by same-thread recursion (see F19/below).
**Fix:**
- **Primary:** replace the single-slot `tls_last_call_id` with a small **per-thread stack** of
  call-ids (push in `log_jni_call`, pop in `log_jni_return`/obj-return). Bounds nesting and stops
  outer-frame orphaning. Depth cap (e.g. 32) with overflow → tolerate (don't pair) rather than
  corrupt.
- **Backstop:** in the Go reader, bound `pendingCalls` — track newest `call_id`; periodically (or
  when size crosses a threshold) evict entries whose `call_id` is older than `newest - WINDOW`,
  emitting them as standalone calls (call-without-return) so nothing is silently dropped. Log an
  eviction counter.

**Acceptance:** recursion-heavy workload (e.g. `toString()` chains) over ≥10 min: `pendingCalls`
size bounded (add a debug expvar/log), output still pairs correctly for the common case.
☐ host-tested · ☐ device-tested

---

## 4. Phase 2 — Injection / fork / process safety (🔴 stability across the fleet)

### F2 — fork-safety premise is false  🔴 CRITICAL
**Where:** `main.c:416-423` (comment claims `static g_initialized` "resets to 0 after fork()").
Same stale-across-fork issue: `bridge.c:23` `g_go_bridge_once`, `bridge.c:151`
`g_logging_ready_cached`, `rangeset.c` `g_exec_ranges`/`g_range_package_name`, and the parallel
assumption in `init.go:46`.
**Reality:** `fork()` gives the child a COW copy with the **same** value — identical to the
`pthread_once` case the comment claims to beat. A child that inherited `g_initialized==1` skips
init, keeps **zygote's** package name + stale ranges.
**Nuance (don't over-fix):** the current primary path is single-process `gozinject` (no fork
after init), and the zygote path limps via COW-inherited hooks + lazy `dlopen` re-seed
(`b2bcc71`). But per-process identity is wrong and it's a latent landmine.
**Fix:** PID-stamp init instead of trusting a static to reset.
```c
static pid_t g_init_pid = 0;
...
pthread_mutex_lock(&g_init_lock);
pid_t me = getpid();
if (g_initialized && g_init_pid == me) { pthread_mutex_unlock(&g_init_lock); return; }
g_initialized = 1; g_init_pid = me;
pthread_mutex_unlock(&g_init_lock);
```
- Apply the same PID-guard pattern to the bridge once-init and to `rangeset` seed state: on PID
  change, force a re-seed and clear `g_range_package_name` so the child re-resolves its own
  package. (rangeset already resets `g_seed_attempted` on dlopen; make it also detect PID change.)
- Fix the comment to state the real mechanism. Update `init.go` accordingly (its `sync.Once`
  survives fork too — but Go runtime is re-created in a forked child only if the child re-execs;
  document the actual Android model: zygote specialization vs gozinject single-process).

**Acceptance:** force a forking scenario (or zygote injection) and confirm the child logs its
**own** package name and re-seeded ranges, not zygote's. ☐ device-tested

---

### F3 — GOT page restored to hardcoded `PROT_READ` over 2 pages  🔴 HIGH
**Where:** `main.c:242-254` and `:265-270`:
`real_mprotect(page, page_size*2, PROT_READ|PROT_WRITE)` … swap … `real_mprotect(page, page_size*2, PROT_READ)`.
**Problems:** (a) assumes the page was read-only originally (true for full-RELRO libdl, not
guaranteed across vendor builds); (b) an 8-byte-aligned GOT slot never crosses a page boundary,
so the second page is gratuitous and its perms are clobbered too.
**Fix:**
- mprotect a **single** page containing the slot (`page`, `page_size`).
- **Capture** original perms before the write and restore exactly those. Read them from
  `/proc/self/maps` for the page (parse the `r/w/x` of the containing range) once, or keep a
  helper that maps the maps-perms to PROT flags. Default conservatively to `PROT_READ` only if
  parsing fails, but log a warning.

**Acceptance:** patch succeeds on device; libdl keeps functioning (dlopen still works post-patch);
no SIGSEGV writing the GOT page later. ☐ device-tested

---

### F23 — VM-table publish race  🟡 (do with F3)
**Where:** `main.c:494-503` (`install_vm_hooks`): `memcpy(&g_hooked_vm_table, *vm, ...)` then
`*vm = &g_hooked_vm_table`. Readers calling `(*vm)->...` during the memcpy can read a
half-populated table.
**Fix:** build into a **local** `JNINativeInterface tmp` (or a second static), fully populate,
then publish with a single `__atomic_store_n(vm, &g_hooked_vm_table, __ATOMIC_RELEASE)` after the
table is complete. Never memcpy into the table that is already published.
**Acceptance:** stress install during live JNI traffic (inject mid-run); no crash. ☐ device-tested

---

### F11 — `mprotect` interposer returns -1 w/o errno; hot-path fopen/lock  🟠 MEDIUM
**Where:** `main.c:222` (`if (!real_mprotect) return -1;` — errno stale) and the synchronous
`dladdr`/`c_path_contains_package`/`fopen("/proc/self/cmdline")` work inside every `PROT_EXEC`
`mprotect`.
**Fix:**
- If `real_mprotect` unresolved, fall back to the raw syscall: `return syscall(SYS_mprotect, addr, len, prot);`
  so the whole process never breaks. On any error path, set `errno` properly.
- Move the range-tracking/package-classification work **off** the synchronous mprotect path:
  record the `(addr,len)` cheaply (lock-free ring or atomic append) and let the consumer/reader
  thread classify, or guard with a `g_in_hook`-style reentrancy flag so the JIT's mprotect can't
  recurse into fopen/lock.
**Acceptance:** JIT-heavy app runs without added latency spikes or deadlocks; `mprotect` semantics
unchanged for the host process. ☐ device-tested

---

## 5. Phase 3 — C/Go correctness hardening (🟠 races, framing, portability)

### F9 — wire string values split on raw control bytes  🟠 MEDIUM
**Where:** `value.go:99-116`, `:210-238` split on `\x01`–`\x04`; encoder `visualize.c`
`vis_encode_typed_args`. A Java string containing U+0001–U+0004 mis-splits (no panic — resilience
holds — but corrupt output). The PBT generator likely excludes these bytes, masking it.
**Fix:** make values **length-prefixed** rather than delimiter-split (mirror what the outer
datagram already does in `event_pipe.go:parseStringsCount`). This is an encoder+decoder change:
- C `vis_encode_typed_args`: emit `sigChar, u16 len, bytes [, u16 extralen, extra]` per record.
- Go `decodeArgs`/`buildJNIValue`/array items: parse by length, not `strings.Split`.
- Keep a version byte or a build-locked invariant so C and Go never disagree.
- **Alternative (lower churn):** keep delimiters but have C **escape** any `\x01`–`\x04` (and the
  escape char) in string payloads; Go unescapes. Length-prefix is cleaner — prefer it.
**Acceptance:** PBT generator extended to include `\x01`–`\x04`, `\x1A`, newlines, and full
Unicode in string args; round-trip property passes; device capture of an app that logs such
strings renders correctly. ☐ host-tested · ☐ device-tested

---

### F10 — cache read path hands out raw pointers under released rwlock  🟠 MEDIUM
**Where:** `hook_common.c` `lookup_method_info`/`lookup_field_info` return `entry->name/...`
pointers; the rwlock is released by the caller; a concurrent `cache_*_signature` does in-place
`strncpy` on the same slot. Mitigated by immediate `copy_cache_str` in `hook_logging.c:124-128`,
`:305-315`, but the copy itself races → torn (never crashing) string.
**Fix:** copy **while holding the rdlock**. Change `lookup_*_info` to take caller-owned output
buffers and `memcpy` into them under the lock, returning success/fail — never expose internal
pointers. Removes the race entirely.
**Acceptance:** ThreadSanitizer-style reasoning + heavy concurrent `GetMethodID`/`GetFieldID`
device workload: names never garbled. ☐ device-tested

---

### F18 — `cfg_cache` racing first lookup double round-trip; never freed  🟡 LOW
**Where:** `hook_common.c:285-298`.
**Fix:** acceptable as a process-lifetime cache; (a) note in a comment that two threads may both
pay the first Go round-trip for a fresh key (loser's work discarded, not leaked); (b) optionally
single-flight per key with a tiny "in-progress" marker. Low priority. ☐ done

---

### F19 — `tls_last_call_id` via `extern` redecl of a `static`  🟡 LOW
**Where:** `hooks.c:184-188` redeclares `extern __thread uint64_t tls_last_call_id;` to reach the
`static __thread` in `bridge.c:143`. Works only because of the concatenated TU.
**Fix:** when implementing the F6 call-id **stack**, define it once in a shared header with a
proper accessor API (`tls_push_call_id`, `tls_pop_call_id`) — eliminates the static/extern hack.
**Acceptance:** compiles without relying on TU concat for this symbol; F6 stack lives here.
☐ done (folded into F6)

---

### F20 — `lookup_field_info` ART `GetName` on cache hit; TLS buffer lifetime  🟡 LOW
**Where:** `hook_common.c:209` calls `art_get_field_name` before the rdlock and unconditionally;
result stored in `static __thread art_name_buf[128]`.
**Fix:** only call `art_get_field_name` on cache **miss**; document the TLS-buffer "valid until
next same-thread call" contract at the return site. Saves an ART call on the hot path.
**Acceptance:** field-heavy workload shows no behavior change, fewer ART calls. ☐ done

---

### F22 — `%g` lossy/locale; `'D'` memcpy assumes LP64  🟡 LOW
**Where:** `visualize.c:361,373,461,641` (`%g`), `:531-537` and `:638-648` (`'D'` memcpy of
`uintptr_t`).
**Fix:** use `%.17g` for round-trippable doubles; `_Static_assert(sizeof(uintptr_t) >= 8)` (or
guard the double path) so a future 32-bit reuse fails loudly rather than silently capturing 4 of 8
bytes. arm64-only today, so low risk. ☐ done

---

## 6. Phase 4 — Range / maps robustness (🟡 coverage correctness)

### F12 — `/proc/self/maps` lines truncated at 512 B  🟡 LOW
**Where:** `main.c:342` (`char line[512]`, `fgets`), `rangeset.c:444`, plus `cmdline` at
`rangeset.c:126`/`main.c:444`.
**Fix:** detect non-newline-terminated reads and drain, or switch to `getline()` (bionic has it).
Long `/data/app/~~base64==/pkg==/lib/arm64/libfoo.so (deleted)` paths can exceed 511 → currently
the lib is silently dropped from seeding.
**Acceptance:** an app with a deeply-nested split-APK lib path is correctly range-seeded (its JNI
calls are logged). ☐ device-tested

---

### F13 — anonymous executable (JIT/packed) regions never range-added  🟡 LOW
**Where:** `rangeset.c:480` (`path[0] != '/'` skip); the `mprotect` interposer only adds when
`dladdr` yields a filename (won't for anon maps).
**Fix:** when a region is executable and has no resolvable system-lib filename, include it (or
track via the `mprotect(PROT_EXEC)` hook even with no name) so PairIP/packed code running from
anonymous maps isn't misclassified as non-app and dropped.
**Acceptance:** capture from a packed app (jiagu/PairIP) shows JNI calls originating from
anonymous exec regions. ☐ device-tested

---

### F14 — `g_seed_attempted` is `volatile` not atomic  🟡 LOW
**Where:** `rangeset.c:44`, read `:285`, written `:62/:292/:417`.
**Fix:** convert to `__atomic_load_n/__atomic_store_n` with ACQUIRE/RELEASE to match the
`g_exec_range_count` discipline used elsewhere in the file. ☐ done

> Consider also (perf, not a bug): `c_is_in_exec_range` is an O(n) linear scan under a mutex on
> the hot path (`rangeset.c:249-272`). If profiling shows contention, switch to a sorted array +
> binary search or a seqlock (write-rare/read-frequent). Track separately — not required for
> "no known defects," but state-of-the-art. ☐ evaluated

---

## 7. Phase 5 — Test honesty (🔴 make the green checkmark mean something)

### F5 — collapse the drifted host mirror; cover the real socket path  🔴 HIGH
**Where:** `types_host.go` (`//go:build !android`) duplicates formatter/decoder and still
implements a **deleted** `pushCallFrame`/`popCallFrame` LIFO stack + renamed gate funcs;
production pairs via `pendingCalls sync.Map` in `event_pipe.go` (`//go:build android`, never
compiled by `go test`). Tests in `pbt_test.go`, `concurrency_test.go` exercise the mirror.
**Fix (structural):**
1. Extract platform-independent logic into **untagged** files compiled in both builds:
   - the `lineFormatter` and all `formatJNIValue`/emit formatting (move out of `logger.go`'s
     android assumptions; keep only cgo sinks behind `//go:build android`).
   - `decodeArgs`/`buildJNIValue`/`buildReturnValue` are already untagged in `value.go` — good;
     keep them the single source.
   - the **pairing** logic: factor `dispatchCall`/`dispatchReturn`/`dispatchObjReturn` pure parts
     (call-id keyed pairing over an injectable map + emit callback) into an untagged file so host
     tests drive the *real* code, with cgo `vis_*` calls behind a small interface that the host
     test stubs.
2. Delete the dead `threadStacks`/`pushCallFrame`/`popCallFrame` from `types_host.go` and the
   tests that drive them (or repoint those tests at the real `pendingCalls` pairing).
3. Make host gate tests call the **same** body as the cgo `//export config_function_*` (factor
   into `func configFunctionEnabledImpl(name string) bool`), so `config.go:255-276` is the tested
   code, not a parallel copy.
4. Extend the wire round-trip generator to exercise F9 (control bytes, Unicode, `\x1A`).
**Acceptance:** `go test` compiles and exercises the android pairing/formatter logic (via the
extracted untagged files); removing a real production function breaks a test. Coverage report
shows `event_pipe.go` pairing covered. ☐ done

### Test-quality nits (do alongside F5)  🟡
- `pbt_test.go:800-810` — Property 11(d) never asserts `[]`-suffix count equals `[` prefix count;
  make it real.
- `TestThreeTierGateFiltering` (`pbt_test.go:1722`) derives "expected" from the same maps under
  test (tautological); re-derive from raw `Functions`/`Categories` + category expansion.
- Remove unused `formatter` vars (`pbt_test.go:1448,1802` etc.).
- Add a C-side test harness (even a tiny `cgo`-less unit exe over `vis_encode_typed_args` and the
  call-id stack) so the encoder isn't only validated by a Go reimplementation. ☐ done

---

## 8. Phase 6 — Docs, hygiene, build (🟡 truth & ergonomics)

- ☐ **F4 / F17 — Docs sync.** Add an `event_pipe` section to `.kiro/specs/jni-call-logger/design.md`
  (framing: magic `0x4A4E4945`, 32-byte header, event types 1–6, sidecar layout). Replace the
  cgo-callback sequence diagram + "Cgo Exports → Event callbacks" block (`design.md:102,109,164-177`).
  Update `requirements.md`/`tasks.md` (task 3.5 still says "dispatch via cgo exports"). Update
  README architecture lines (≈60,70,380). Fix counts: README `strings` 10→12; `design.md` `refs`
  9→10; replace "FIFO" with "LIFO" (README ≈418, design ≈144). Note that category members (207) ⊊
  total hooked slots (228).
- ☑ **F15 — Hygiene.** Added `*.ascii` to `.gitignore`; moved `rec.ascii`/`rec2.ascii` out of repo
  root into `docs/baseline/` (gitignored). Repo root clean of stray large files.
- ☑ **F16 — Build.** Dropped `-a` from `go build` in `xmake.lua` (incremental build now 0.13s vs
  full rebuild; `xmake b -r jnilog` still forces clean rebuild). Verified `.so` still valid ELF ARM64.
  (Left for later: assert a minimum NDK revision and record it.)
- ☐ **F21 — Dead code.** Remove or wire `is_self_symbol` (`main.c:328-335`) and `k_log_tag`
  (`bridge.c:22`).

---

## 9. Regression testing protocol (run every phase)

### 9.1 Build
```
xmake b jnilog            # → dist/libjnilog.so ; record sha256 + size
cd src/go && go vet ./... && go test ./... -count=1
```
Gate: build clean (no new warnings — `-Wall -Wextra` is on), host tests green.

### 9.2 Live device verification via gozinject
Device: `b83607a5` (Xiaomi M2007J3SY, Android 16, arm64-v8a). `../gozinject` is the loader
(root injector; no ptrace; vma_hide). Use the **same fixed `jnilog.json`** for every run.

Per-phase loop:
1. `xmake b jnilog` (jnilog) and rebuild/stage the payload via gozinject's flow
   (`xmake b` + `xmake run` in `../gozinject`, payload = `dist/libjnilog.so`).
2. Inject into the chosen target; capture rendered output for a fixed duration to
   `docs/baseline/live_after_<phase>.log`.
3. `diff` against `live_before.log`. Investigate **every** diff — it must be an intended change
   (e.g. F9 now renders previously-corrupt strings correctly) or it's a regression.
4. Watch logcat for: crashes, `JNI ERROR`/global-ref overflow, `art::` aborts, our own
   `LOG_DIRECT` warnings.
5. For F1/F6/F24: long-run (≥10 min) object-heavy capture; confirm `dumpsys meminfo <pid>` gref
   count is stable and the new drop/eviction counters log sane values.

### 9.3 Targets
- Primary regression target: a benign app with steady JNI traffic (pick one with predictable
  output for clean diffs).
- Anti-tamper validation: `com.openai.chatgpt` / `libpairipcore.so` (the v1.0.3 success case) —
  confirm it still doesn't crash and still logs consistently after each phase.

### 9.4 Crash triage
On any device crash: pull `adb logcat -b crash`, the tombstone (`/data/tombstones/`), and the
last N lines of capture; bisect to the phase; do not advance until green.

---

## 10. Suggested commit sequencing

1. `phase0: baseline + gitignore rec*.ascii, drop go build -a` (no functional risk)
2. `event_pipe: free deferred grefs on drop/overflow/non-attach (F1) + drop counter (F24)`
3. `event_pipe: raw-byte placeholder slots (F8) + boundary-safe truncation (F7)`
4. `event_pipe: per-thread call-id stack + bounded pendingCalls (F6/F19)`
5. `main: PID-guarded init + rangeset PID re-seed (F2)`
6. `main: single-page GOT patch w/ captured perms (F3) + atomic VM-table publish (F23)`
7. `main: mprotect syscall fallback + off-hot-path range tracking (F11)`
8. `wire: length-prefixed string values, C+Go (F9)`
9. `hook_common: copy cache strings under rdlock (F10) + field-name ART on miss only (F20)`
10. `rangeset: getline maps parsing (F12), anon-exec inclusion (F13), atomic seed flag (F14)`
11. `visualize: %.17g + LP64 static_assert (F22)`
12. `test: collapse host mirror, cover real socket path (F5) + test-quality nits`
13. `docs: sync design/requirements/tasks/README to event_pipe; fix counts & LIFO (F4/F17)`
14. `cleanup: remove dead is_self_symbol/k_log_tag (F21)`

Each commit: build + host tests + a live capture diff before moving on.

---

## 11. Definition of done

- ☐ All F1–F24 checked off or explicitly ⊘ won't-fixed with justification.
- ☐ Host tests green **and** exercise the production (android-path) formatter/decoder/pairing.
- ☐ ≥10-min object-heavy live capture: no crash, stable gref count, sane drop/eviction counters.
- ☐ `com.openai.chatgpt` capture still stable + consistent (v1.0.3 invariant preserved).
- ☐ `design.md`/`requirements.md`/`tasks.md`/README describe the socket transport and correct counts.
- ☐ Repo clean: no stray large files; `go build -a` removed; no dead code; no new compiler warnings.
- ☐ Live render output diff vs baseline fully explained (only intended changes remain).

---

### Appendix A — file map (for fast navigation)

| Area | Files |
|------|-------|
| Injection / init / fork | `src/cbridge/main.c` |
| ART symbols / bridge / call-id TLS | `src/cbridge/bridge.c`, `bridge.h` |
| Caller range filter | `src/cbridge/rangeset.c`, `src/go/rangeset.go` |
| Hook table + utility hooks | `src/cbridge/hooks.c` |
| X-macro method/field hooks | `src/cbridge/hook_methods.c`, `hook_fields.c`, `hook_internal.h` |
| Caches / reentrancy / config cache | `src/cbridge/hook_common.c` |
| Wire-encoding contexts | `src/cbridge/hook_logging.c` |
| Object introspection / arg encode | `src/cbridge/visualize.c`, `visualize.h` |
| **C→Go event socket** | `src/cbridge/event_pipe.c`, `event_pipe.h`, `src/go/event_pipe.go` |
| Go decode / types | `src/go/value.go` |
| Go formatter / sinks | `src/go/logger.go` |
| Go config / gates / call-key | `src/go/config.go` |
| Go signature parser | `src/go/signature.go` |
| Go init / cgo callbacks | `src/go/init.go`, `src/go/main.go` |
| Host test mirror (to retire) | `src/go/types_host.go` |
| Tests | `src/go/pbt_test.go`, `concurrency_test.go`, `bench_test.go`, `testutil_test.go` |
| Build | `xmake.lua` |
| Docs / specs | `README.md`, `docs/CONFIG_SCHEMA.json`, `.kiro/specs/jni-call-logger/*` |

### Appendix B — environment (validated this session)
- Device: `b83607a5` Xiaomi M2007J3SY, Android 16, arm64-v8a, `adb` as `shell` (uid 2000).
- Toolchain: Go 1.26.3, NDK at `/opt/android-ndk` (`ANDROID_NDK_HOME`/`ANDROID_NDK` set), xmake 3.0.8.
- Loader: `../gozinject` (root injector, vma_hide kernel hook; no ptrace).
- Build artifact: `dist/libjnilog.so` (~4.6 MB, cgo c-shared, Go runtime + concatenated C TU).
