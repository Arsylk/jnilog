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
#include <errno.h>
#include <elf.h>

#include <jni.h>
#include <android/log.h>

#include "bridge.h"
#include "hook_internal.h"
#include "visualize.h"

#include "_cgo_export.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Global state */
static int g_initialized = 0;
static int g_injection_mode = 0;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

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
    if (handle) c_seed_exec_ranges_from_maps();
    return handle;
}

static uintptr_t maps_find_lib_base(const char* lib_suffix);

/* mprotect */
static int (*real_mprotect)(void*, size_t, int) = NULL;
static pthread_once_t mprotect_resolve_once = PTHREAD_ONCE_INIT;
static void resolve_mprotect(void) {
    real_mprotect = (int (*)(void*, size_t, int))dlsym(RTLD_NEXT, "mprotect");
}

typedef void* (*loader_dlopen_fn)(const char*, int, const void*);
static loader_dlopen_fn orig_loader_dlopen = NULL;

static void* hooked_loader_dlopen(const char* filename, int flags, const void* caller_addr) {
    loader_dlopen_fn orig = orig_loader_dlopen;
    if (!orig) return NULL;
    void* handle = orig(filename, flags, caller_addr);
    if (handle) {
        LOG_DIRECT(ANDROID_LOG_DEBUG, "__loader_dlopen(%s, 0x%x) = %p",
                   filename ? filename : "<null>", flags, handle);
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
        LOG_DIRECT(ANDROID_LOG_DEBUG, "__loader_android_dlopen_ext(%s, 0x%x) = %p",
                   filename ? filename : "<null>", flags, handle);
        c_seed_exec_ranges_from_maps();
    }
    return handle;
}

static void install_loader_dlopen_hook(void) {
    uintptr_t base = maps_find_lib_base("/libdl.so");
    if (!base) {
        LOG_DIRECT(ANDROID_LOG_WARN, "install_loader_dlopen_hook: libdl.so not found");
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
                   "install_loader_dlopen_hook: missing PLT sections in libdl.so");
        return;
    }

    pthread_once(&mprotect_resolve_once, resolve_mprotect);
    if (!real_mprotect) {
        LOG_DIRECT(ANDROID_LOG_WARN,
                   "install_loader_dlopen_hook: real_mprotect not available");
        return;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    size_t n = rela_plt_sz / sizeof(Elf64_Rela);
    int found_dlopen = 0, found_dlopen_ext = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t    sym_idx  = ELF64_R_SYM(rela_plt[i].r_info);
        const char* sym_name = strtab + symtab[sym_idx].st_name;

        if (!found_dlopen && strcmp(sym_name, "__loader_dlopen") == 0) {
            void** got_slot = (void**)(base + rela_plt[i].r_offset);
            orig_loader_dlopen = (loader_dlopen_fn)*got_slot;
            void* page = (void*)((uintptr_t)got_slot & ~((uintptr_t)page_size - 1));
            real_mprotect(page, (size_t)page_size * 2, PROT_READ | PROT_WRITE);
            *got_slot = (void*)hooked_loader_dlopen;
            real_mprotect(page, (size_t)page_size * 2, PROT_READ);
            LOG_DIRECT(ANDROID_LOG_INFO,
                       "install_loader_dlopen_hook: patched __loader_dlopen in libdl.so GOT (orig=%p)",
                       (void*)orig_loader_dlopen);
            found_dlopen = 1;
        } else if (!found_dlopen_ext && strcmp(sym_name, "__loader_android_dlopen_ext") == 0) {
            void** got_slot = (void**)(base + rela_plt[i].r_offset);
            orig_loader_android_dlopen_ext = (loader_android_dlopen_ext_fn)*got_slot;
            void* page = (void*)((uintptr_t)got_slot & ~((uintptr_t)page_size - 1));
            real_mprotect(page, (size_t)page_size * 2, PROT_READ | PROT_WRITE);
            *got_slot = (void*)hooked_loader_android_dlopen_ext;
            real_mprotect(page, (size_t)page_size * 2, PROT_READ);
            LOG_DIRECT(ANDROID_LOG_INFO,
                       "install_loader_dlopen_hook: patched __loader_android_dlopen_ext in libdl.so GOT (orig=%p)",
                       (void*)orig_loader_android_dlopen_ext);
            found_dlopen_ext = 1;
        }

        if (found_dlopen && found_dlopen_ext) break;
    }

    if (!found_dlopen)
        LOG_DIRECT(ANDROID_LOG_WARN,
                   "install_loader_dlopen_hook: __loader_dlopen not found in libdl.so PLT");
    if (!found_dlopen_ext)
        LOG_DIRECT(ANDROID_LOG_WARN,
                   "install_loader_dlopen_hook: __loader_android_dlopen_ext not found in libdl.so PLT");
}

int mprotect(void* addr, size_t len, int prot) {
    pthread_once(&mprotect_resolve_once, resolve_mprotect);
    int (*real)(void*, size_t, int) = real_mprotect;
    if (!real) return -1;
    int result = real(addr, len, prot);
    if (result == 0 && (prot & PROT_EXEC)) {
        Dl_info info;
        if (dladdr(addr, &info) && info.dli_fname != NULL &&
            !c_is_system_lib_path(info.dli_fname) &&
            c_path_contains_package(info.dli_fname)) {
            c_add_exec_range((uintptr_t)addr, len);
        }
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

static int is_self_symbol(const char* name, void* sym) {
    if (!sym) return 0;
    if (strcmp(name, "JNI_GetCreatedJavaVMs") == 0 && sym == (void*)JNI_GetCreatedJavaVMs) return 1;
    if (strcmp(name, "JNI_OnLoad") == 0 && sym == (void*)JNI_OnLoad) return 1;
    if (strcmp(name, "android_dlopen_ext") == 0 && sym == (void*)android_dlopen_ext) return 1;
    if (strcmp(name, "mprotect") == 0 && sym == (void*)mprotect) return 1;
    return 0;
}

/* --- Helpers --- */

static uintptr_t maps_find_lib_base(const char* lib_suffix) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        char* path = strrchr(line, '/');
        if (!path) continue;
        char* nl = path + strlen(path) - 1;
        while (nl >= path && (*nl == '\n' || *nl == '\r' || *nl == ' ')) *nl-- = '\0';
        size_t sl = strlen(lib_suffix), pl = strlen(path);
        if (pl < sl || strcmp(path + pl - sl, lib_suffix) != 0) continue;
        unsigned long lo = 0;
        sscanf(line, "%lx", &lo);
        base = (uintptr_t)lo;
        break;
    }
    fclose(f);
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
    if (g_initialized) return;

    LOG_DIRECT(ANDROID_LOG_INFO, "init_once_handler: starting pid=%d", getpid());

    static uintptr_t libart_base = 0;
    if (!libart_base) libart_base = maps_find_lib_base("/libart.so");
    if (libart_base) {
        original_JNI_GetCreatedJavaVMs = (jint (*)(JavaVM**, jsize, jsize*))
            elf_find_sym(libart_base, "JNI_GetCreatedJavaVMs");
        original_JNI_OnLoad = (jint (*)(JavaVM*, void*))
            elf_find_sym(libart_base, "JNI_OnLoad");
    }
    LOG_DIRECT(ANDROID_LOG_INFO, "init_once_handler: JNI_GetCreatedJavaVMs=%p JNI_OnLoad=%p",
               (void*)original_JNI_GetCreatedJavaVMs, (void*)original_JNI_OnLoad);

    bridge_init();
    install_loader_dlopen_hook();
    try_install_hooks_from_created_vms();

    g_initialized = 1;
    LOG_DIRECT(ANDROID_LOG_INFO, "init_once_handler: initialization successful");
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
        g_original_vm_table = *vm;
        memcpy(&g_hooked_vm_table, g_original_vm_table, sizeof(g_hooked_vm_table));
        g_hooked_vm_table.GetEnv = hooked_vm_GetEnv;
    }
    *vm = &g_hooked_vm_table;
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
    pthread_once(&init_once, init_once_handler);
    LOG_DIRECT(ANDROID_LOG_INFO, "library constructor: initialization complete");
    return NULL;
}

__attribute__((constructor)) void library_constructor(void) {
    LOG_DIRECT(ANDROID_LOG_INFO, "=== libjnilog constructor start (PID=%d) ===", getpid());
    
    const char* ld_preload = getenv("LD_PRELOAD");
    if (ld_preload && strstr(ld_preload, "jnilog")) {
        pthread_once(&init_once, init_once_handler);
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
    pthread_once(&init_once, init_once_handler);
    if (!original_JNI_GetCreatedJavaVMs) return JNI_ERR;
    jint res = original_JNI_GetCreatedJavaVMs(p_vm, buf_len, n_vms);
    if (res == JNI_OK && p_vm && *p_vm) try_install_hooks(*p_vm);
    return res;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    pthread_once(&init_once, init_once_handler);
    bridge_activate_go();
    try_install_hooks(vm);
    if (original_JNI_OnLoad) return original_JNI_OnLoad(vm, reserved);
    return JNI_VERSION_1_6;
}
