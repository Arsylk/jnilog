#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <elf.h>

#include <jni.h>
#include <android/log.h>

#include "bridge.h"
#include "hook_internal.h"
#include "visualize.h"
#include "ansi.h"

#include "_cgo_export.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* dlopen-subsystem log shape (see go-logging spec): every line starts with a
 * dim "[dlopen]" tag — distinguishes them at-a-glance from JNI-call lines,
 * which never carry a subsystem prefix — followed by a short lowercase verb
 * message and key=value pairs with snake_case keys. The tag is the only
 * decoration the JNI lines never use, so visual separation is automatic. */
#define DL_TAG     C_DIM "[dlopen]" C_RESET
#define DL_KEY(k)  C_GRAY k "=" C_RESET

/* sanitize_for_log copies src into dst with every byte < 0x20 (and 0x7f)
 * replaced by '?'. Used on attacker-controlled dlopen() filenames so a
 * malicious caller can't inject ANSI escapes through the logcat stream. */
static const char* sanitize_for_log(char* dst, size_t dstsize, const char* src) {
    if (!dst || dstsize == 0) return "";
    if (!src) { dst[0] = '\0'; return dst; }
    size_t i = 0;
    for (; i + 1 < dstsize && src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    dst[i] = '\0';
    return dst;
}

/* Global state */
static int g_initialized = 0;
static pid_t g_init_pid = 0;   /* PID that ran init; detects a forked child (F2) */
static int g_injection_mode = 0;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* Original function pointers */
static jint (*original_JNI_GetCreatedJavaVMs)(JavaVM**, jsize, jsize*) = NULL;
static jint (*original_JNI_OnLoad)(JavaVM* vm, void* reserved) = NULL;

/* ── LD_PRELOAD-style symbol interpositions ───────────────────────────────
 * Each wrapper resolves its real counterpart lazily via pthread_once so that
 * dlsym(RTLD_NEXT) is never called from bridge_init() / the constructor
 * thread (which runs while the linker lock is still held by the dlopen
 * caller, making any RTLD_NEXT lookup on another thread deadlock).
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct android_dlextinfo android_dlextinfo;
static void* (*real_android_dlopen_ext)(const char*, int, const android_dlextinfo*) = NULL;
static pthread_once_t android_dlopen_ext_resolve_once = PTHREAD_ONCE_INIT;
static void resolve_android_dlopen_ext(void) {
    real_android_dlopen_ext = (void* (*)(const char*, int, const android_dlextinfo*))
        dlsym(RTLD_NEXT, "android_dlopen_ext");
}

void* android_dlopen_ext(const char* path, int flags, const android_dlextinfo* extinfo) {
    pthread_once(&android_dlopen_ext_resolve_once, resolve_android_dlopen_ext);
    void* (*real)(const char*, int, const android_dlextinfo*) = real_android_dlopen_ext;
    if (!real) return NULL;
    void* handle = real(path, flags, extinfo);
    if (handle) {
        char path_safe[512];
        LOG_DIRECT(ANDROID_LOG_DEBUG,
                   DL_TAG " open "
                   DL_KEY("fn")     C_MAGENTA  "android_dlopen_ext" C_RESET " "
                   DL_KEY("path")   C_CYAN     "%s" C_RESET " "
                   DL_KEY("flags")  C_GRAY     "0x%x" C_RESET " "
                   DL_KEY("handle") C_LAVENDER "%p" C_RESET,
                   sanitize_for_log(path_safe, sizeof(path_safe), path), flags, handle);
        c_reset_seed_attempted();
        c_seed_exec_ranges_from_maps();
    }
    return handle;
}

static uintptr_t maps_find_lib_base(const char* lib_suffix);

/* mprotect: jnilog exports its own interposer (see below) that goes straight to
 * the raw syscall, so no dlsym(RTLD_NEXT,"mprotect") resolution is needed. */

typedef void* (*loader_dlopen_fn)(const char*, int, const void*);
static loader_dlopen_fn orig_loader_dlopen = NULL;

static void* hooked_loader_dlopen(const char* filename, int flags, const void* caller_addr) {
    loader_dlopen_fn orig = orig_loader_dlopen;
    if (!orig) return NULL;
    void* handle = orig(filename, flags, caller_addr);
    if (handle) {
        char fn_safe[512];
        LOG_DIRECT(ANDROID_LOG_DEBUG,
                   DL_TAG " open "
                   DL_KEY("fn")     C_MAGENTA  "__loader_dlopen" C_RESET " "
                   DL_KEY("path")   C_CYAN     "%s" C_RESET " "
                   DL_KEY("flags")  C_GRAY     "0x%x" C_RESET " "
                   DL_KEY("handle") C_LAVENDER "%p" C_RESET,
                   sanitize_for_log(fn_safe, sizeof(fn_safe), filename), flags, handle);
        c_reset_seed_attempted();
        c_seed_exec_ranges_from_maps();
    }
    return handle;
}

typedef void* (*loader_android_dlopen_ext_fn)(const char*, int, const void*, const void*);
static loader_android_dlopen_ext_fn orig_loader_android_dlopen_ext = NULL;

static void* hooked_loader_android_dlopen_ext(const char* filename, int flags,
                                               const void* extinfo, const void* caller_addr) {
    loader_android_dlopen_ext_fn orig = orig_loader_android_dlopen_ext;
    if (!orig) return NULL;
    void* handle = orig(filename, flags, extinfo, caller_addr);
    if (handle) {
        char fn_safe[512];
        LOG_DIRECT(ANDROID_LOG_DEBUG,
                   DL_TAG " open "
                   DL_KEY("fn")     C_MAGENTA  "__loader_android_dlopen_ext" C_RESET " "
                   DL_KEY("path")   C_CYAN     "%s" C_RESET " "
                   DL_KEY("flags")  C_GRAY     "0x%x" C_RESET " "
                   DL_KEY("handle") C_LAVENDER "%p" C_RESET,
                   sanitize_for_log(fn_safe, sizeof(fn_safe), filename), flags, handle);

        /* If the package name isn't resolved yet, try to extract it from the
         * loaded library path. Paths like /data/app/~~xxx==/com.pkg.name-yyy==/...
         * contain the package name between the second == and the dash-hash suffix. */
        if (filename && c_get_package_name()[0] == '\0') {
            const char *marker = strstr(filename, "/data/app/");
            if (marker) {
                const char *pkg_start = strstr(marker + 10, "==/");
                if (pkg_start) {
                    pkg_start += 3; /* skip "==/" */
                    const char *pkg_end = strchr(pkg_start, '-');
                    if (pkg_end && (pkg_end - pkg_start) > 3 && (pkg_end - pkg_start) < 256) {
                        char pkg_buf[256];
                        size_t len = (size_t)(pkg_end - pkg_start);
                        memcpy(pkg_buf, pkg_start, len);
                        pkg_buf[len] = '\0';
                        /* Validate it looks like a package name (contains dots) */
                        if (strchr(pkg_buf, '.') != NULL) {
                            c_set_package_name(pkg_buf);
                        }
                    }
                }
            }
        }

        c_reset_seed_attempted();
        c_seed_exec_ranges_from_maps();
    }
    return handle;
}

/* Read the current memory protection of the page containing `addr` from
 * /proc/self/maps, as PROT_* flags.  Returns -1 if the range isn't found or the
 * line can't be parsed (caller falls back conservatively).  Only the leading
 * "lo-hi perms" fields are needed, so the 512-byte line cap is irrelevant here
 * (perms sit at the very start of the line). (F3) */
static int page_prot_from_maps(uintptr_t addr) {
    jl_linereader lr;
    if (jl_lr_open(&lr, "/proc/self/maps", O_RDONLY | O_CLOEXEC) < 0) return -1;
    char line[512];
    int prot = -1;
    while (jl_lr_next(&lr, line, sizeof(line))) {
        uintptr_t lo = 0, hi = 0;
        char perms[5] = {0};
        if (jl_parse_maps_head(line, &lo, &hi, perms)) {
            if (addr >= lo && addr < hi) {
                prot = 0;
                if (perms[0] == 'r') prot |= PROT_READ;
                if (perms[1] == 'w') prot |= PROT_WRITE;
                if (perms[2] == 'x') prot |= PROT_EXEC;
                break;
            }
        }
    }
    jl_lr_close(&lr);
    return prot;
}

/* Atomically swap a single GOT slot to `new_fn`, restoring the page's ORIGINAL
 * protection afterward (F3).  An 8-byte-aligned GOT slot never spans two pages,
 * so one page is sufficient — the old code mprotect'd a gratuitous 2-page span
 * and clobbered the neighbouring page's perms.  Original perms are read from
 * /proc/self/maps and restored exactly (full-RELRO libs are r--p, but vendor
 * builds may differ); on a parse failure we conservatively restore PROT_READ
 * and warn.  The store is release-ordered: in stealth mode the constructor runs
 * on a detached worker thread while the process is live, so another thread may
 * dispatch through this slot concurrently. */
static void patch_got_slot(void** got_slot, void* new_fn, long page_size,
                           const char* what) {
    void* page = (void*)((uintptr_t)got_slot & ~((uintptr_t)page_size - 1));
    int orig_prot = page_prot_from_maps((uintptr_t)got_slot);
    jl_syscall3(__NR_mprotect, (long)page, (long)page_size, PROT_READ | PROT_WRITE);
    __atomic_store_n((uintptr_t*)got_slot, (uintptr_t)new_fn, __ATOMIC_RELEASE);
    if (orig_prot < 0) {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   DL_TAG " GOT page perms unread for %s; restoring PROT_READ", what);
        orig_prot = PROT_READ;
    }
    jl_syscall3(__NR_mprotect, (long)page, (long)page_size, (long)orig_prot);
}

static void install_loader_dlopen_hook(void) {
    uintptr_t base = maps_find_lib_base("/libdl.so");
    if (!base) {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   DL_TAG " hook install skipped "
                   DL_KEY("reason") C_YELLOW "lib_not_found" C_RESET " "
                   DL_KEY("lib")    C_CYAN   "libdl.so" C_RESET);
        return;
    }

    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)base;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return;

    Elf64_Phdr* phdr = (Elf64_Phdr*)(base + ehdr->e_phoff);
    Elf64_Dyn*  dyn  = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn*)(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return;

    Elf64_Sym*  symtab      = NULL;
    const char* strtab      = NULL;
    Elf64_Rela* rela_plt    = NULL;
    size_t      rela_plt_sz = 0;

    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab      = (Elf64_Sym*) (base + d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab      = (const char*)(base + d->d_un.d_ptr); break;
            case DT_JMPREL:   rela_plt    = (Elf64_Rela*)(base + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: rela_plt_sz = d->d_un.d_val;                        break;
            default: break;
        }
    }

    if (!symtab || !strtab || !rela_plt || !rela_plt_sz) {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   DL_TAG " hook install skipped "
                   DL_KEY("reason") C_YELLOW "plt_sections_missing" C_RESET " "
                   DL_KEY("lib")    C_CYAN   "libdl.so" C_RESET);
        return;
    }

    long page_size = jl_page_size();
    if (page_size <= 0) page_size = 4096;

    size_t n = rela_plt_sz / sizeof(Elf64_Rela);
    int found_dlopen = 0, found_dlopen_ext = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t    sym_idx  = ELF64_R_SYM(rela_plt[i].r_info);
        const char* sym_name = strtab + symtab[sym_idx].st_name;

        if (!found_dlopen && strcmp(sym_name, "__loader_dlopen") == 0) {
            void** got_slot = (void**)(base + rela_plt[i].r_offset);
            orig_loader_dlopen = (loader_dlopen_fn)*got_slot;
            patch_got_slot(got_slot, (void*)hooked_loader_dlopen, page_size,
                           "__loader_dlopen");
            LOG_DIRECT(ANDROID_LOG_INFO,
                       DL_TAG " hook installed "
                       DL_KEY("fn")   C_MAGENTA  "__loader_dlopen" C_RESET " "
                       DL_KEY("lib")  C_CYAN     "libdl.so" C_RESET " "
                       DL_KEY("orig") C_LAVENDER "%p" C_RESET,
                       (void*)orig_loader_dlopen);
            found_dlopen = 1;
        } else if (!found_dlopen_ext && strcmp(sym_name, "__loader_android_dlopen_ext") == 0) {
            void** got_slot = (void**)(base + rela_plt[i].r_offset);
            orig_loader_android_dlopen_ext = (loader_android_dlopen_ext_fn)*got_slot;
            patch_got_slot(got_slot, (void*)hooked_loader_android_dlopen_ext, page_size,
                           "__loader_android_dlopen_ext");
            LOG_DIRECT(ANDROID_LOG_INFO,
                       DL_TAG " hook installed "
                       DL_KEY("fn")   C_MAGENTA  "__loader_android_dlopen_ext" C_RESET " "
                       DL_KEY("lib")  C_CYAN     "libdl.so" C_RESET " "
                       DL_KEY("orig") C_LAVENDER "%p" C_RESET,
                       (void*)orig_loader_android_dlopen_ext);
            found_dlopen_ext = 1;
        }

        if (found_dlopen && found_dlopen_ext) break;
    }

    if (!found_dlopen)
        LOG_DIRECT(ANDROID_LOG_WARN,
                   DL_TAG " hook install skipped "
                   DL_KEY("reason") C_YELLOW  "symbol_not_in_plt" C_RESET " "
                   DL_KEY("fn")     C_MAGENTA "__loader_dlopen" C_RESET " "
                   DL_KEY("lib")    C_CYAN    "libdl.so" C_RESET);
    if (!found_dlopen_ext)
        LOG_DIRECT(ANDROID_LOG_WARN,
                   DL_TAG " hook install skipped "
                   DL_KEY("reason") C_YELLOW  "symbol_not_in_plt" C_RESET " "
                   DL_KEY("fn")     C_MAGENTA "__loader_android_dlopen_ext" C_RESET " "
                   DL_KEY("lib")    C_CYAN    "libdl.so" C_RESET);
}

/* Reentrancy guard for the mprotect interposer (F11): the classification work
 * below (dladdr, package refresh) may itself mprotect, and the JIT calls
 * mprotect on a hot path — we must not recurse into the linker lock / a /proc
 * read from inside our own hook. */
static __thread int g_in_mprotect_hook = 0;

int mprotect(void* addr, size_t len, int prot) {
    /* Always go straight to the kernel — bionic's mprotect is a thin syscall
     * wrapper, so this is functionally identical while needing no dlsym to
     * resolve a "real" mprotect. jl_syscall3 returns -errno without touching
     * libc errno, so set errno to keep this exported interposer faithful to the
     * libc ABI for its callers (incl. the Go runtime). (F11) */
    long sr = jl_syscall3(__NR_mprotect, (long)addr, (long)len, (long)prot);
    int result;
    if (sr < 0) { errno = (int)(-sr); result = -1; }
    else result = 0;
    /* Track newly-executable app regions for caller-range filtering, but never
     * re-entrantly (see g_in_mprotect_hook) and never letting this best-effort
     * bookkeeping change the syscall's result/errno for the caller. (F11) */
    if (result == 0 && (prot & PROT_EXEC) && !g_in_mprotect_hook) {
        int saved_errno = errno;
        g_in_mprotect_hook = 1;
        Dl_info info;
        if (dladdr(addr, &info) && info.dli_fname != NULL &&
            !c_is_system_lib_path(info.dli_fname) &&
            c_path_contains_package(info.dli_fname)) {
            c_add_exec_range((uintptr_t)addr, len);
        }
        g_in_mprotect_hook = 0;
        errno = saved_errno;
    }
    return result;
}

/* VM hooking */
static const struct JNIInvokeInterface* g_original_vm_table = NULL;
static struct JNIInvokeInterface g_hooked_vm_table = {0};
static JavaVM* g_hooked_vm = NULL;
static pthread_mutex_t g_vm_hook_lock = PTHREAD_MUTEX_INITIALIZER;

static void try_install_hooks(JavaVM* vm);
static void try_install_hooks_from_created_vms(void);
static void install_vm_hooks(JavaVM* vm);
static void restore_vm_hooks(void);
static jint hooked_vm_GetEnv(JavaVM* vm, void** penv, jint version);

JNIEXPORT jint JNICALL JNI_GetCreatedJavaVMs(JavaVM** p_vm, jsize buf_len, jsize* n_vms);
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved);

/* --- Helpers --- */

static uintptr_t maps_find_lib_base(const char* lib_suffix) {
    jl_linereader lr;
    if (jl_lr_open(&lr, "/proc/self/maps", O_RDONLY | O_CLOEXEC) < 0) return 0;
    /* 1 KB line buffer so a long split-APK path isn't truncated (which could
     * hide the matching lib's line and make us miss its base). (F12) */
    char line[1024];
    uintptr_t base = 0;
    while (jl_lr_next(&lr, line, sizeof(line))) {
        char* path = strrchr(line, '/');
        if (!path) continue;
        char* nl = path + strlen(path) - 1;
        while (nl >= path && (*nl == '\n' || *nl == '\r' || *nl == ' ')) *nl-- = '\0';
        size_t sl = strlen(lib_suffix), pl = strlen(path);
        if (pl < sl || strcmp(path + pl - sl, lib_suffix) != 0) continue;
        const char* p = line;
        base = (uintptr_t)jl_parse_hex(&p);
        break;
    }
    jl_lr_close(&lr);
    return base;
}

static void* elf_find_sym(uintptr_t base, const char* sym_name) {
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)base;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return NULL;
    Elf64_Dyn* dyn = NULL;
    Elf64_Phdr* phdr = (Elf64_Phdr*)(base + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn*)(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return NULL;
    Elf64_Sym* symtab = NULL;
    const char* strtab = NULL;
    uint32_t* gnu_hash = NULL;
    uintptr_t syment = sizeof(Elf64_Sym);
    uintptr_t strsz = 0;
    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab   = (Elf64_Sym*)(base + d->d_un.d_ptr);   break;
            case DT_STRTAB:   strtab   = (const char*)(base + d->d_un.d_ptr);  break;
            case DT_GNU_HASH: gnu_hash = (uint32_t*)(base + d->d_un.d_ptr);    break;
            case DT_SYMENT:   syment   = d->d_un.d_val;                         break;
            case DT_STRSZ:    strsz    = d->d_un.d_val;                         break;
            default: break;
        }
    }
    if (!symtab || !strtab || !gnu_hash) return NULL;
    uint32_t nbuckets  = gnu_hash[0];
    uint32_t symndx    = gnu_hash[1];
    uint32_t maskwords = gnu_hash[2];
    uint32_t* buckets  = gnu_hash + 4 + maskwords * 2;
    uint32_t* chains   = buckets + nbuckets;
    for (uint32_t b = 0; b < nbuckets; b++) {
        uint32_t idx = buckets[b];
        if (idx < symndx) continue;
        for (;;) {
            Elf64_Sym* sym = (Elf64_Sym*)((uint8_t*)symtab + idx * syment);
            if (sym->st_name && sym->st_value) {
                uint32_t off = sym->st_name;
                if (!strsz || off < strsz) {
                    if (strcmp(strtab + off, sym_name) == 0)
                        return (void*)(base + sym->st_value);
                }
            }
            uint32_t chain_val = chains[idx - symndx];
            if (chain_val & 1) break;
            idx++;
        }
    }
    return NULL;
}


/* --- Init --- */

static void init_once_handler(void) {
    /* Fork-safety (F2): a `static g_initialized` is NOT reset by fork() — the
     * child gets a COW copy with the SAME value, identical to the pthread_once
     * case.  So we PID-stamp instead of trusting the flag: re-init iff we are
     * the same process that set it.  In the gozinject model this runs exactly
     * once anyway — gozinject traps setArgV0 and injects into the already-forked,
     * specialized app child, so there is no fork after the constructor runs.
     * The PID guard is a latent-landmine guard for any future zygote-resident
     * model; per-process *identity* (package name + exec ranges) is re-resolved
     * on PID change inside rangeset's c_seed_exec_ranges_from_maps. */
    pthread_mutex_lock(&g_init_lock);
    pid_t me = getpid();
    if (g_initialized && g_init_pid == me) { pthread_mutex_unlock(&g_init_lock); return; }
    g_initialized = 1;
    g_init_pid = me;
    pthread_mutex_unlock(&g_init_lock);

    LOG_DIRECT(ANDROID_LOG_INFO, "init_once_handler: starting pid=%d", getpid());

    int degraded = 0;

    static uintptr_t libart_base = 0;
    if (!libart_base) libart_base = maps_find_lib_base("/libart.so");
    if (libart_base) {
        original_JNI_GetCreatedJavaVMs = (jint (*)(JavaVM**, jsize, jsize*))
            elf_find_sym(libart_base, "JNI_GetCreatedJavaVMs");
        original_JNI_OnLoad = (jint (*)(JavaVM*, void*))
            elf_find_sym(libart_base, "JNI_OnLoad");
    } else {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   "init_once_handler: libart.so not found in /proc/self/maps, "
                   "continuing in degraded mode (no ART symbol resolution)");
        degraded = 1;
    }
    if (!degraded && !original_JNI_GetCreatedJavaVMs) {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   "init_once_handler: JNI_GetCreatedJavaVMs not resolved from libart.so, "
                   "continuing in degraded mode (JNI hook installation deferred)");
        degraded = 1;
    }
    LOG_DIRECT(ANDROID_LOG_INFO, "init_once_handler: JNI_GetCreatedJavaVMs=%p JNI_OnLoad=%p",
               (void*)original_JNI_GetCreatedJavaVMs, (void*)original_JNI_OnLoad);

    bridge_init();
    install_loader_dlopen_hook();
    try_install_hooks_from_created_vms();

    if (degraded) {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   "init_once_handler: initialization completed in degraded mode (pid=%d)",
                   getpid());
    } else {
        LOG_DIRECT(ANDROID_LOG_INFO, "init_once_handler: initialization successful");
    }
}

/* --- JNI Hooks --- */

static void try_install_hooks_from_created_vms(void) {
    JavaVM* vm = NULL;
    jsize n_vms = 0;
    if (!original_JNI_GetCreatedJavaVMs) return;
    if (original_JNI_GetCreatedJavaVMs && original_JNI_GetCreatedJavaVMs(&vm, 1, &n_vms) == JNI_OK && n_vms > 0 && vm) {
        try_install_hooks(vm);
    }
}

static void try_install_hooks(JavaVM* vm) {
    JNIEnv* env = NULL;
    if (!vm || !*vm) return;

    bridge_activate_go();

    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
        install_jni_hooks(env);
    } else {
        JNIEnv* attached_env = NULL;
        if ((*vm)->AttachCurrentThread(vm, &attached_env, NULL) == JNI_OK && attached_env) {
            install_jni_hooks(attached_env);
            (*vm)->DetachCurrentThread(vm);
        }
    }
    vis_set_vm(vm);
    install_vm_hooks(vm);
}

static void install_vm_hooks(JavaVM* vm) {
    pthread_mutex_lock(&g_vm_hook_lock);
    if (g_original_vm_table == NULL) {
        /* Fully populate the hooked table BEFORE it is ever published (F23): the
         * memcpy + GetEnv patch happen under the lock and only once (guarded by
         * g_original_vm_table==NULL), so the table is never memcpy'd into while
         * already reachable via *vm. */
        g_original_vm_table = *vm;
        memcpy(&g_hooked_vm_table, g_original_vm_table, sizeof(g_hooked_vm_table));
        g_hooked_vm_table.GetEnv = hooked_vm_GetEnv;
    }
    /* Publish with a release-store so a reader on another core that observes the
     * new table pointer via (*vm)->... is guaranteed to also see the fully
     * populated table contents (the plain store left this ordering implicit). */
    __atomic_store_n(vm, (const struct JNIInvokeInterface*)&g_hooked_vm_table,
                     __ATOMIC_RELEASE);
    g_hooked_vm = vm;
    pthread_mutex_unlock(&g_vm_hook_lock);
}

static void restore_vm_hooks(void) {
    pthread_mutex_lock(&g_vm_hook_lock);
    if (g_hooked_vm && g_original_vm_table && *g_hooked_vm == &g_hooked_vm_table) {
        *g_hooked_vm = g_original_vm_table;
    }
    g_hooked_vm = NULL;
    pthread_mutex_unlock(&g_vm_hook_lock);
}

static jint hooked_vm_GetEnv(JavaVM* vm, void** penv, jint version) {
    if (g_original_vm_table == NULL || g_original_vm_table->GetEnv == NULL) return JNI_ERR;
    jint result = g_original_vm_table->GetEnv(vm, penv, version);
    if (result == JNI_OK && penv && *penv) {
        bridge_activate_go();
        install_jni_hooks(*penv);
    }
    return result;
}




/* --- Constructor / Destructor --- */

static void* constructor_init_worker(void* arg) {
    init_once_handler();
    LOG_DIRECT(ANDROID_LOG_INFO, "library constructor: initialization complete");
    return NULL;
}

__attribute__((constructor)) void library_constructor(void) {
    LOG_DIRECT(ANDROID_LOG_INFO, "=== libjnilog constructor start (PID=%d) ===", getpid());

    /* Capture our own [base,end) NOW — synchronously, inside the constructor,
     * which runs during dlopen while we are still mapped and BEFORE the injector
     * vma-hides / soinfo-unlinks us, and before any exec-range seeding (the
     * detached init worker, goBridgeInit, and the dlopen hooks all seed later).
     * If this were deferred to the worker thread it could race vma_hide and lose
     * our identity, which would let our own staged segment be seeded as an
     * in-scope app range — and we would log every JNI call our formatter makes.
     * Idempotent (guarded), so the later c_init_range_tracking call is a no-op. */
    c_capture_self_range();

    const char* ld_preload = getenv("LD_PRELOAD");
    if (ld_preload && strstr(ld_preload, "jnilog")) {
        init_once_handler();
    } else {
        g_injection_mode = 1;
        pthread_t tid;
        if (pthread_create(&tid, NULL, constructor_init_worker, NULL) == 0) {
            pthread_detach(tid);
        } else {
            LOG_DIRECT(ANDROID_LOG_WARN, "constructor worker creation failed, deferring init to JNI entrypoints");
        }
    }
}

__attribute__((destructor)) static void library_destructor(void) {
    restore_vm_hooks();
}

/* --- JNI exports --- */

JNIEXPORT jint JNICALL JNI_GetCreatedJavaVMs(JavaVM** p_vm, jsize buf_len, jsize* n_vms) {
    init_once_handler();
    if (!original_JNI_GetCreatedJavaVMs) return JNI_ERR;
    jint res = original_JNI_GetCreatedJavaVMs(p_vm, buf_len, n_vms);
    /* Only deref *p_vm when the underlying call actually wrote one — caller may
     * legitimately pass buf_len=0 to probe the count, in which case *p_vm is
     * uninitialized. */
    if (res == JNI_OK && p_vm && buf_len > 0 && n_vms && *n_vms > 0 && *p_vm) {
        try_install_hooks(*p_vm);
    }
    return res;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    init_once_handler();
    bridge_activate_go();
    try_install_hooks(vm);
    if (original_JNI_OnLoad) return original_JNI_OnLoad(vm, reserved);
    return JNI_VERSION_1_6;
}
