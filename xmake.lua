set_project("jnilog_payload")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")

set_plat("android")
set_arch("arm64-v8a")

-- Include directories
add_includedirs("src/shared")
add_includedirs("src/cbridge")

-- C flags
add_cflags("-fPIC", "-Wall", "-Wextra", "-D_GNU_SOURCE")
add_cxxflags("-fPIC", "-Wall", "-Wextra")

target("jnilog")
    set_kind("phony")

    before_build(function(target)
        local ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK") or "/opt/android-ndk"
        local toolchain = path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")
        local so_out = path.join(target:targetdir(), "libjnilog.so")

        print("Building unified libjnilog.so...")
        os.vrunv("go", {
            "build",
            "-a",
            "-buildmode=c-shared",
            "-o", so_out,
            "./src/go"
        }, {
            envs = {
                GOOS = "android",
                GOARCH = "arm64",
                CGO_ENABLED = "1",
                CC = path.join(toolchain, "aarch64-linux-android21-clang"),
                CXX = path.join(toolchain, "aarch64-linux-android21-clang++")
            }
        })
    end)

    set_targetdir("build")
target_end()

-- Run tests
task("run-test")
    on_run(function()
        print("Push library to device...")
        os.exec("adb push build/libjnilog.so /data/local/tmp/")
    end)
task_end()
