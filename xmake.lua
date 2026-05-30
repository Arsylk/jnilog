set_project("jnilog")
set_version("1.0.3")

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
	os.vrunv("go", {
		"build",
		"-a",
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
		},
	})
	os.cd(oldir)

	print("Built: " .. so_out)
end)
target_end()

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
