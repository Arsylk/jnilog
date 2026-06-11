set_project("jnilog")
set_version("1.1.0")

add_rules("mode.debug", "mode.release")

set_plat("android")
set_arch("arm64-v8a")

-- Include directories
add_includedirs("src/cbridge")

-- C flags
add_cflags("-fPIC", "-Wall", "-Wextra", "-D_GNU_SOURCE")
add_cxxflags("-fPIC", "-Wall", "-Wextra")

-- ── User-tunable build options ────────────────────────────────────────────
-- API level: change with `xmake f --api_level=24` (default: 21).
-- Host: auto-detected from os.host()/os.arch(); override with
--       `xmake f --ndk_host=linux-x86_64`.
option("api_level")
set_default("21")
set_showmenu(true)
set_description("Android API level passed to aarch64-linux-android<N>-clang")
option_end()

option("ndk_host")
set_default("")
set_showmenu(true)
set_description(
	"NDK prebuilt host triple (linux-x86_64, darwin-x86_64, darwin-arm64, windows-x86_64). Auto-detected when empty."
)
option_end()

local function detect_ndk_host()
	local user = get_config("ndk_host")
	if user and user ~= "" then
		return user
	end
	local host, arch = os.host(), os.arch()
	if host == "macosx" then
		if arch == "arm64" then
			return "darwin-arm64"
		end
		return "darwin-x86_64"
	elseif host == "windows" then
		return "windows-x86_64"
	end
	return "linux-x86_64"
end

target("jnilog")
set_kind("phony")
set_targetdir("dist")

before_build(function(target)
	-- Resolve NDK path from environment variables (Requirement 19.3, 19.6)
	local ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK")
	if not ndk or ndk == "" then
		raise(
			"ERROR: Android NDK not found. Set ANDROID_NDK_HOME or ANDROID_NDK environment variable to the NDK root directory."
		)
	end

	local ndk_host = detect_ndk_host()
	local api = get_config("api_level") or "21"

	-- Verify NDK toolchain exists at the resolved path (Requirement 19.6)
	local toolchain = path.join(ndk, "toolchains", "llvm", "prebuilt", ndk_host, "bin")
	local cc = path.join(toolchain, "aarch64-linux-android" .. api .. "-clang")
	local cxx = path.join(toolchain, "aarch64-linux-android" .. api .. "-clang++")
	if not os.isfile(cc) then
		raise(
			"ERROR: NDK toolchain not found at '%s' (host=%s api=%s). Ensure ANDROID_NDK_HOME/ANDROID_NDK points to a valid NDK installation containing aarch64-linux-android%s-clang. Override host with `xmake f --ndk_host=...` or API with `xmake f --api_level=...`.",
			cc,
			ndk_host,
			api,
			api
		)
	end

	-- Output artifact: dist/libjnilog.so (Requirement 19.2)
	local so_out = path.join(os.projectdir(), target:targetdir(), "libjnilog.so")

	-- Build with go build -buildmode=c-shared targeting aarch64-linux-android<api>
	-- (Requirements 19.1, 19.2, 19.3, 19.4, 19.5, 19.7)
	print(string.format("Building unified libjnilog.so (aarch64-linux-android%s, host=%s)...", api, ndk_host))
	local go_src = path.join(os.projectdir(), "src", "go")
	local oldir = os.cd(go_src)
	-- NOTE: no `-a`. The phony target re-runs before_build on every invocation,
	-- and Go's build cache tracks cgo inputs (including the generated
	-- _cgo_export.h) correctly, so a force-rebuild only wastes time. Use
	-- `xmake b -r jnilog` if a clean rebuild is ever needed.
	os.vrunv("go", {
		"build",
		"-buildmode=c-shared",
		"-o",
		so_out,
		".",
	}, {
		envs = {
			GOOS = "android",
			GOARCH = "arm64",
			CGO_ENABLED = "1",
			CC = cc,
			CXX = cxx,
			-- Freestanding C-bridge: stop the compiler from synthesizing libc
			-- calls (memcpy/memset/strlen for struct copies, FORTIFY _chk
			-- variants) so the only libc imports left are ones we deliberately
			-- keep. Passed via CGO_CFLAGS env (not a #cgo directive) because
			-- cgo's source-directive allowlist rejects -fno-builtin; the env
			-- var is trusted and bypasses that check. Applies to every C TU in
			-- the package, including cbridge_all.c. Verified by the readelf
			-- import gate. See src/cbridge/freestanding/jl_libc.h.
			-- Note: replicate Go's default "-g -O2" because setting CGO_CFLAGS
			-- replaces (not appends to) that default.
			CGO_CFLAGS = "-g -O2 -fno-builtin -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0",
		},
	})
	os.cd(oldir)

	print("Built: " .. so_out)

	-- ── event-pipe wire guard ───────────────────────────────────────────────
	-- Every event_pipe_emit_* MUST declare nstrings == its append_str(buf …)
	-- count. The Go consumer reads that count from the datagram header and each
	-- case requires an exact number of strings — an off-by-one silently DROPS
	-- every event of that type (this is what made field-access logging emit
	-- nothing). See src/cbridge/event_pipe.c.
	local ep = io.readfile(path.join(os.projectdir(), "src", "cbridge", "event_pipe.c"))
	if ep then
		local fn, ns, appends, bad = nil, nil, 0, {}
		for line in (ep .. "\n"):gmatch("(.-)\n") do
			if line:find("event_pipe_emit_[%w_]+%(") then
				fn = line:match("event_pipe_emit_[%w_]+"); ns = nil; appends = 0
			end
			local m = line:match("nstrings = %*/%s*(%d+)")
			if m then ns = tonumber(m) end
			if line:find("append_str(buf", 1, true) or line:find("append_str_max(buf", 1, true) then
				appends = appends + 1
			end
			if line:match("^}") and fn then
				if ns ~= nil and ns ~= appends then
					table.insert(bad, string.format("%s (nstrings=%d, appends=%d)", fn, ns, appends))
				end
				fn = nil
			end
		end
		if #bad > 0 then
			raise("event-pipe wire mismatch — nstrings != append_str count: "
				.. table.concat(bad, "; ")
				.. ". The Go consumer will silently drop these events.")
		end
		print("event-pipe wire guard: ok")
	end

	-- ── freestanding import-regression gate ─────────────────────────────────
	-- These libc symbols were migrated into the freestanding C-bridge
	-- (src/cbridge/freestanding/*.h) and must never reappear as dynamic
	-- imports. A reappearance means a stray bare libc call (e.g. a plain
	-- strlen()) crept back into src/cbridge and would once again be visible to
	-- a co-injected GOT-patching libc logger. Fail the build if so.
	local denylist = {
		"dladdr", "fclose", "fcntl", "fgets", "fopen", "fputc", "fread",
		"fwrite", "getline", "getpid", "getsockopt", "pthread_rwlock_rdlock",
		"pthread_rwlock_unlock", "pthread_rwlock_wrlock", "send", "setsockopt",
		"snprintf", "socketpair", "sscanf", "strchr", "strcmp", "strdup",
		"strlen", "strncpy", "strrchr", "strstr", "syscall", "sysconf",
		"vsnprintf",
	}
	import("lib.detect.find_program")
	local readelf = find_program("llvm-readelf") or find_program("readelf")
	local out = readelf and os.iorunv(readelf, { "--dyn-syms", so_out }) or nil
	if out then
		local bad = {}
		for line in out:gmatch("[^\n]+") do
			if line:find("UND") then
				for _, s in ipairs(denylist) do
					if line:find("%f[%w_]" .. s .. "%f[^%w_]") then
						table.insert(bad, s)
					end
				end
			end
		end
		if #bad > 0 then
			raise("freestanding import-regression: a migrated libc symbol "
				.. "reappeared as a dynamic import: " .. table.concat(bad, ", ")
				.. ". A bare libc call crept back into src/cbridge — route it "
				.. "through src/cbridge/freestanding/jl_libc.h.")
		end
		print("freestanding import gate: ok (no migrated libc symbol re-imported)")
	else
		print("freestanding import gate: skipped (no readelf found)")
	end
end)
target_end()

-- Host regression test for the freestanding jl_vsnprintf (jl_fmt.h). Compiles
-- with the host compiler (jl_fmt.h is self-contained, no NDK needed) and diffs
-- against the platform libc snprintf. See src/cbridge/freestanding/tests/.
task("fmttest")
set_menu({ description = "Build & run the host jl_fmt (vsnprintf) regression test" })
on_run(function()
	local src = path.join(os.projectdir(), "src/cbridge/freestanding/tests/jl_fmt_test.c")
	local bin = path.join(os.tmpdir(), "jl_fmt_test")
	local cc = os.getenv("CC_HOST") or "cc"
	-- -Wno-format: the test deliberately exercises edge specifiers (e.g. "% o").
	os.vrunv(cc, { "-O2", "-Wno-format", "-o", bin, src })
	os.execv(bin, {})
end)
task_end()

-- Push native library to device (builds first if needed)
task("push")
set_menu({ description = "Push libjnilog.so to device via adb" })
on_run(function()
	import("core.project.project")
	os.exec("xmake build jnilog")
	print("Pushing libjnilog.so to device...")
	os.exec("adb push dist/libjnilog.so /data/local/tmp/")
end)
task_end()

-- Open the jnilog.json TUI configurator (separate Go module, tools/jnilogcfg).
-- NOTE: not named "config" — that is a reserved xmake built-in task.
task("cfgtui")
set_menu({ description = "Open the jnilog.json TUI/CLI configurator (tools/jnilogcfg)" })
on_run(function()
	local dir = path.join(os.projectdir(), "tools", "jnilogcfg")
	local file = path.join(os.projectdir(), "docs", "jnilog.json")
	os.execv("go", { "run", ".", "-f", file }, { curdir = dir })
end)
task_end()

-- Build the jnilogcfg configurator (host binary) into dist/. The tool is a
-- separate Go module (tools/jnilogcfg) so it must be compiled from its own dir.
task("cfgbuild")
set_menu({ description = "Build the jnilogcfg configurator into dist/jnilogcfg" })
on_run(function()
	local dir = path.join(os.projectdir(), "tools", "jnilogcfg")
	local bin = "jnilogcfg"
	if is_host("windows") then
		bin = "jnilogcfg.exe"
	end
	local out = path.join(os.projectdir(), "dist", bin)
	os.mkdir(path.join(os.projectdir(), "dist"))
	print("Building jnilogcfg (host) -> " .. out)
	os.execv("go", { "build", "-trimpath", "-o", out, "." }, { curdir = dir })
	print("Built: " .. out)
end)
task_end()
