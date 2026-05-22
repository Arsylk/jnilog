set_project("jnilog_payload")
set_version("1.0.2")

add_rules("mode.debug", "mode.release")

set_plat("android")
set_arch("arm64-v8a")

-- Include directories
add_includedirs("src/cbridge")

-- C flags
add_cflags("-fPIC", "-Wall", "-Wextra", "-D_GNU_SOURCE")
add_cxxflags("-fPIC", "-Wall", "-Wextra")

target("jnilog")
    set_kind("phony")
    set_targetdir("dist")

    before_build(function(target)
        -- Resolve NDK path from environment variables (Requirement 19.3, 19.6)
        local ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK")
        if not ndk or ndk == "" then
            raise("ERROR: Android NDK not found. Set ANDROID_NDK_HOME or ANDROID_NDK environment variable to the NDK root directory.")
        end

        -- Verify NDK toolchain exists at the resolved path (Requirement 19.6)
        local toolchain = path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")
        local cc = path.join(toolchain, "aarch64-linux-android21-clang")
        if not os.isfile(cc) then
            raise("ERROR: NDK toolchain not found at '%s'. Ensure ANDROID_NDK_HOME or ANDROID_NDK points to a valid NDK installation containing the aarch64-linux-android21 toolchain.", cc)
        end

        -- Output artifact: build/libjnilog.so (Requirement 19.2)
        local so_out = path.join(os.projectdir(), target:targetdir(), "libjnilog.so")

        -- Build with go build -buildmode=c-shared targeting aarch64-linux-android21
        -- (Requirements 19.1, 19.2, 19.3, 19.4, 19.5, 19.7)
        print("Building unified libjnilog.so (aarch64-linux-android21)...")
        local go_src = path.join(os.projectdir(), "src", "go")
        local oldir = os.cd(go_src)
        os.vrunv("go", {
            "build",
            "-a",
            "-buildmode=c-shared",
            "-o", so_out,
            "."
        }, {
            envs = {
                GOOS = "android",
                GOARCH = "arm64",
                CGO_ENABLED = "1",
                CC = cc,
                CXX = path.join(toolchain, "aarch64-linux-android21-clang++")
            }
        })
        os.cd(oldir)

        print("Built: " .. so_out)
    end)
target_end()

-- Push native library to device (builds first if needed)
task("push")
    set_menu({description = "Push libjnilog.so to device via adb"})
    on_run(function()
        import("core.project.project")
        os.exec("xmake build jnilog")
        print("Pushing libjnilog.so to device...")
        os.exec("adb push dist/libjnilog.so /data/local/tmp/")
    end)
task_end()
