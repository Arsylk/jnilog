/*
 * rangeset.c — Pure C executable range tracking for JNI caller filtering.
 *
 * Tracks which memory ranges belong to the target package's native libraries.
 * When a JNI hook fires, we check if the caller's return address falls within
 * any registered range — if so, the call came from app code and should be
 * logged.
 *
 * Ported from rangeset.go to eliminate cgo overhead on the hot path.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <unistd.h>

#if __has_include(<android/log.h>)
#include <android/log.h>
#endif

#include "ansi.h"

#define RANGESET_LOG_TAG "JniLog"

/* ====================================================================
 * Package name
 * ==================================================================== */

#define RANGESET_MAX_PACKAGE_NAME 256

static char g_range_package_name[RANGESET_MAX_PACKAGE_NAME] = {0};
static pthread_mutex_t g_range_pkg_lock = PTHREAD_MUTEX_INITIALIZER;


/* Forward declaration — defined here because c_set_package_name (below) resets
 * it.  Prevents hot-path retry storms: once we've attempted seeding and found
 * nothing, we don't retry on every JNI call.  Reset when something changes
 * (new library loaded via dlopen, package name set). */
/* Accessed with __atomic_* (ACQUIRE/RELEASE) rather than `volatile` (F14) — to
 * match the g_exec_range_count discipline elsewhere in this file; volatile gives
 * no cross-thread ordering or atomicity guarantee. Zero-initialised as a static. */
static int g_seed_attempted;

/* PID that last seeded.  A mismatch means we've forked: the inherited package
 * name + exec ranges belong to the parent (e.g. zygote), so they get dropped
 * and re-resolved for this process (F2).  The primary gozinject model injects
 * into the already-forked, specialized app child (it traps setArgV0 *after*
 * fork), so there is no fork after our load and this never triggers there — it
 * is a latent-landmine guard for any future zygote-resident model. */
static pid_t g_seed_pid = 0;

void c_set_package_name(const char *name) {
  pthread_mutex_lock(&g_range_pkg_lock);
  if (name != NULL) {
    strncpy(g_range_package_name, name, RANGESET_MAX_PACKAGE_NAME - 1);
    g_range_package_name[RANGESET_MAX_PACKAGE_NAME - 1] = '\0';
  } else {
    g_range_package_name[0] = '\0';
  }
#if __has_include(<android/log.h>)
  __android_log_print(ANDROID_LOG_INFO, RANGESET_LOG_TAG,
                      "c_set_package_name: set to '" C_CYAN "%s" C_RESET "'", g_range_package_name);
#endif
  pthread_mutex_unlock(&g_range_pkg_lock);

  /* Reset the seed-attempted flag so the hot path will re-attempt seeding
   * now that we have a valid package name. */
  __atomic_store_n(&g_seed_attempted, 0, __ATOMIC_RELEASE);
}

/* Strictly speaking the package-name buffer can be rewritten more than once
 * (refresh_package_name_if_needed retries until a non-zygote name resolves),
 * so callers must either hold the lock or accept the snapshot we return. To
 * avoid handing out a raw pointer that races with the writer, return a
 * thread-local snapshot. */
const char *c_get_package_name(void) {
  static __thread char snap[RANGESET_MAX_PACKAGE_NAME];
  pthread_mutex_lock(&g_range_pkg_lock);
  strncpy(snap, g_range_package_name, sizeof(snap) - 1);
  snap[sizeof(snap) - 1] = '\0';
  pthread_mutex_unlock(&g_range_pkg_lock);
  return snap;
}

static void refresh_package_name_if_needed(void) {
  pthread_mutex_lock(&g_range_pkg_lock);

  if (g_range_package_name[0] != '\0' &&
      strstr(g_range_package_name, "zygote") == NULL &&
      strstr(g_range_package_name, "app_process") == NULL &&
      strstr(g_range_package_name, "<pre-initialized>") == NULL) {
    pthread_mutex_unlock(&g_range_pkg_lock);
    return;
  }

  /* Try dladdr on ourselves first — works immediately when loaded in stealth
   * mode (library path is /data/data/<pkg>/.cache_XXX or similar). */
  Dl_info self_info;
  if (dladdr((void *)refresh_package_name_if_needed, &self_info) &&
      self_info.dli_fname != NULL && self_info.dli_fname[0] != '\0') {
    const char *path = self_info.dli_fname;
    const char *pkg_start = NULL;

    if (strncmp(path, "/data/data/", 11) == 0) {
      pkg_start = path + 11;
    } else if (strncmp(path, "/data/user/0/", 13) == 0) {
      pkg_start = path + 13;
    }

    if (pkg_start != NULL) {
      const char *pkg_end = strchr(pkg_start, '/');
      if (pkg_end != NULL) {
        size_t len = (size_t)(pkg_end - pkg_start);
        if (len > 0 && len < RANGESET_MAX_PACKAGE_NAME) {
          strncpy(g_range_package_name, pkg_start, len);
          g_range_package_name[len] = '\0';
#if __has_include(<android/log.h>)
          __android_log_print(ANDROID_LOG_INFO, RANGESET_LOG_TAG,
                              "package name resolved from lib path: '%s'",
                              g_range_package_name);
#endif
          pthread_mutex_unlock(&g_range_pkg_lock);
          return;
        }
      }
    }
  }

  /* Fallback: read /proc/self/cmdline (available after zygote specialization). */
  int cmdfd = jl_openat("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  if (cmdfd >= 0) {
    char buf[RANGESET_MAX_PACKAGE_NAME];
    long r = jl_read(cmdfd, buf, sizeof(buf) - 1);
    jl_close(cmdfd);
    if (r > 0) {
      size_t n = (size_t)r;
      size_t len = 0;
      while (len < n && buf[len] != '\0') len++;
      buf[len] = '\0';
      if (len > 0 &&
          strstr(buf, "zygote") == NULL &&
          strstr(buf, "app_process") == NULL &&
          strstr(buf, "<pre-initialized>") == NULL) {
        strncpy(g_range_package_name, buf, RANGESET_MAX_PACKAGE_NAME - 1);
        g_range_package_name[RANGESET_MAX_PACKAGE_NAME - 1] = '\0';
#if __has_include(<android/log.h>)
        __android_log_print(ANDROID_LOG_INFO, RANGESET_LOG_TAG,
                            "package name resolved from cmdline: '%s'",
                            g_range_package_name);
#endif
      }
    }
  }

  pthread_mutex_unlock(&g_range_pkg_lock);
}

int c_path_contains_package(const char *path) {
  refresh_package_name_if_needed();
  if (path == NULL || path[0] == '\0')
    return 0;
  /* Take a snapshot under the lock so a concurrent refresh_package_name_if_needed
   * cannot torn-overwrite the strstr target mid-scan. */
  char snap[RANGESET_MAX_PACKAGE_NAME];
  pthread_mutex_lock(&g_range_pkg_lock);
  strncpy(snap, g_range_package_name, sizeof(snap) - 1);
  snap[sizeof(snap) - 1] = '\0';
  pthread_mutex_unlock(&g_range_pkg_lock);
  if (snap[0] == '\0')
    return 0;
  return strstr(path, snap) != NULL ? 1 : 0;
}

/* ====================================================================
 * Own executable range (self-exclusion)
 * ==================================================================== */

/* The one mechanism that keeps jnilog from logging its OWN JNI calls.
 *
 * The injector stages this payload under a RANDOM name inside the app sandbox
 * (e.g. /data/data/<pkg>/.org.chromium.<hex>.tmp). That path matches the
 * package-name filter and is NOT the literal "libjnilog", so every name-based
 * self-check (a strstr "libjnilog") was dead on the real payload — and without
 * a working exclusion the renamed payload seeds its own segment as an app range
 * and logs every JNI call our own formatter makes (observed once: 99% of output
 * was self-noise). Worse, shortly after dlopen the injector vma-hides us from
 * /proc/maps AND soinfo-unlinks us from the linker solist, so any LATER lookup
 * of ourselves — by name, by path, by maps, or by dladdr — fails outright; a
 * per-reseed dladdr(self) returns NULL once we're hidden.
 *
 * So capture our load base + image extent ONCE, here, by IDENTITY (dladdr on
 * our own code, then our own program headers) while we are still resolvable, and
 * store it as plain numbers. c_is_self_addr() then answers "is this caller our
 * own code?" on the hot path with two compares — name-independent, rename-proof,
 * and immune to vma_hide / soinfo-unlink. */
static uintptr_t g_self_base = 0, g_self_end = 0;

void c_capture_self_range(void) {
  if (g_self_base != 0) return;
  Dl_info info;
  if (!dladdr((void *)c_capture_self_range, &info) || info.dli_fbase == NULL) {
#if __has_include(<android/log.h>)
    __android_log_print(ANDROID_LOG_WARN, RANGESET_LOG_TAG,
                        "c_capture_self_range: dladdr(self) failed — self-exclusion degraded");
#endif
    return;
  }
  uintptr_t base = (uintptr_t)info.dli_fbase;
  uintptr_t end = 0;
  /* base maps our ELF header (the first PT_LOAD at file offset 0); walk the
   * program headers to find the end of the highest PT_LOAD so the range covers
   * every segment a return address could land in. */
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_phoff != 0) {
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
      if (ph[i].p_type != PT_LOAD) continue;
      uintptr_t seg_end = base + ph[i].p_vaddr + ph[i].p_memsz;
      if (seg_end > end) end = seg_end;
    }
  }
  g_self_base = base;
  /* Unreadable phdrs are not expected; fall back to a bounded span rather than 0
   * (which would silently disable the check). */
  g_self_end = (end > base) ? end : base + 0x800000;
#if __has_include(<android/log.h>)
  __android_log_print(ANDROID_LOG_INFO, RANGESET_LOG_TAG,
                      "self range: " C_LAVENDER "%lx-%lx" C_RESET,
                      (unsigned long)g_self_base, (unsigned long)g_self_end);
#endif
}

int c_is_self_addr(uintptr_t addr) {
  return g_self_base != 0 && addr >= g_self_base && addr < g_self_end;
}

/* ====================================================================
 * Exec range set
 * ==================================================================== */

#define MAX_EXEC_RANGES 1024

typedef struct {
  uintptr_t base;
  uintptr_t end;
} c_exec_range_t;

static c_exec_range_t g_exec_ranges[MAX_EXEC_RANGES];
static int g_exec_range_count = 0;
static pthread_mutex_t g_range_lock = PTHREAD_MUTEX_INITIALIZER;

int c_add_exec_range(uintptr_t base, uintptr_t size) {
  if (size == 0)
    return 0;

  uintptr_t end = base + size;
  if (end <= base)
    return 0; /* overflow */

  pthread_mutex_lock(&g_range_lock);

  for (int i = 0; i < g_exec_range_count; i++) {
    /* Already contained */
    if (base >= g_exec_ranges[i].base && end <= g_exec_ranges[i].end) {
      pthread_mutex_unlock(&g_range_lock);
      return 0;
    }
    /* Overlapping — merge */
    if (!(end < g_exec_ranges[i].base || base > g_exec_ranges[i].end)) {
      if (base < g_exec_ranges[i].base)
        g_exec_ranges[i].base = base;
      if (end > g_exec_ranges[i].end)
        g_exec_ranges[i].end = end;
      pthread_mutex_unlock(&g_range_lock);
      return 1;
    }
  }

  if (g_exec_range_count < MAX_EXEC_RANGES) {
    g_exec_ranges[g_exec_range_count].base = base;
    g_exec_ranges[g_exec_range_count].end = end;
    /* Release-store paired with the acquire-loads in c_has_exec_ranges /
     * c_should_try_seed so observers of count>0 also see the writes above. */
    __atomic_store_n(&g_exec_range_count, g_exec_range_count + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_range_lock);
    return 1;
  }

  pthread_mutex_unlock(&g_range_lock);
  return 0;
}

/* Drop all seeded ranges.  Used on a detected fork — the inherited ranges
 * belong to the parent process and must be re-seeded for the child (F2). */
static void c_clear_exec_ranges(void) {
  pthread_mutex_lock(&g_range_lock);
  /* Release-store paired with the acquire-loads in c_has_exec_ranges /
   * c_should_try_seed so a concurrent reader never sees count==0 with stale
   * entries (it would just re-seed, which is safe). */
  __atomic_store_n(&g_exec_range_count, 0, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_range_lock);
}

int c_is_in_exec_range(uintptr_t addr) {
  if (addr == 0)
    return 0;

  /* Hold the lock for the entire scan. The earlier snapshot-then-scan
   * pattern was unsafe because c_add_exec_range mutates an existing entry's
   * base/end fields in-place under the merge branch (lines 213-217). A
   * lockless reader could observe (new_base, old_end) or (old_base, new_end)
   * — torn (base,end) pairs — causing transient mis-classification of the
   * caller address. The scan is bounded by MAX_EXEC_RANGES (1024) and
   * read-only, so taking the mutex adds only a few hundred cycles on the
   * hot path. */
  pthread_mutex_lock(&g_range_lock);
  int count = g_exec_range_count;
  int hit = 0;
  for (int i = 0; i < count; i++) {
    if (addr >= g_exec_ranges[i].base && addr < g_exec_ranges[i].end) {
      hit = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_range_lock);
  return hit;
}

int c_has_exec_ranges(void) {
  /* Acquire-load to pair with the release-store inside c_add_exec_range,
   * so callers observing count>0 also see the matching range writes. */
  return __atomic_load_n(&g_exec_range_count, __ATOMIC_ACQUIRE) > 0;
}

/* Returns true if we should attempt seeding (either we have no ranges AND
 * haven't tried yet, or we already have ranges).  This prevents the hot-path
 * from calling c_seed_exec_ranges_from_maps() on every JNI hook. */
int c_should_try_seed(void) {
  if (__atomic_load_n(&g_exec_range_count, __ATOMIC_ACQUIRE) > 0) return 0;
  if (__atomic_load_n(&g_seed_attempted, __ATOMIC_ACQUIRE)) return 0;
  return 1;
}

/* Reset the seed-attempted flag so the next should_log_from_caller will
 * re-attempt seeding.  Called when something changes (dlopen, package name). */
void c_reset_seed_attempted(void) {
  __atomic_store_n(&g_seed_attempted, 0, __ATOMIC_RELEASE);
  /* A dlopen/package change also invalidates the symbolizer module table so
   * the next caller lookup re-reads /proc/self/maps. */
  jl_symbolize_refresh();
}

/* ====================================================================
 * solist scanning via dl_iterate_phdr (linker-locked iteration)
 * ==================================================================== */

int c_is_system_lib_path(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] != '/') return 1;
  size_t len = strlen(path);
  if (len > 5 && (strcmp(path + len - 5, ".odex") == 0 ||
                  strcmp(path + len - 5, ".vdex") == 0)) return 1;
  if (len > 4 && (strcmp(path + len - 4, ".oat") == 0 ||
                  strcmp(path + len - 4, ".art") == 0)) return 1;
  return strncmp(path, "/system/", 8) == 0 ||
         strncmp(path, "/system_ext/", 12) == 0 ||
         strncmp(path, "/apex/", 6) == 0 ||
         strncmp(path, "/vendor/", 8) == 0 ||
         strncmp(path, "/product/", 9) == 0 ||
         strncmp(path, "/odm/", 5) == 0 ||
         strstr(path, "/bionic") != NULL;
}

/* Forward declaration for /proc/self/maps fallback */
static int c_seed_exec_ranges_from_proc_maps(void);

/* dl_iterate_phdr callback context — populated once, then driven by the loader
 * iterating each loaded module under its internal lock. Replaces the manual
 * r_debug→link_map walk which had no synchronization with the dynamic linker
 * and could SIGSEGV on concurrent dlclose. */
typedef struct {
  char pkg_snap[RANGESET_MAX_PACKAGE_NAME];
  int added;
} seed_ctx_t;

static int seed_iter_cb(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  seed_ctx_t *ctx = (seed_ctx_t *)data;

  const char *lib_name = info->dlpi_name;
  if (lib_name == NULL || lib_name[0] == '\0') return 0;

  /* Exclude our own payload by load-base identity (rename-proof; see
   * c_capture_self_range). dlpi_addr is the module's load bias, == our base. */
  if (c_is_self_addr((uintptr_t)info->dlpi_addr)) return 0;

  /* Skip system libs / non-package paths. pkg_snap is the locked snapshot
   * (no race with a concurrent refresh_package_name_if_needed writer). */
  if (c_is_system_lib_path(lib_name)) return 0;
  if (ctx->pkg_snap[0] == '\0' || strstr(lib_name, ctx->pkg_snap) == NULL) return 0;

  for (uint16_t i = 0; i < info->dlpi_phnum; i++) {
    if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
    uintptr_t start = (uintptr_t)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    uintptr_t size_mem = info->dlpi_phdr[i].p_memsz;
    if (size_mem > 0 && c_add_exec_range(start, size_mem)) {
#if __has_include(<android/log.h>)
      __android_log_print(ANDROID_LOG_DEBUG, RANGESET_LOG_TAG,
                          C_DIM "range added:" C_RESET " " C_LAVENDER "%lx-%lx" C_RESET " " C_YELLOW "%s" C_RESET,
                          (unsigned long)start, (unsigned long)(start + size_mem), lib_name);
#endif
      ctx->added++;
    }
  }
  return 0; /* keep iterating */
}

int c_seed_exec_ranges_from_maps(void) {
  /* Fork detection (F2): if we're a *different* process than the one that last
   * seeded (a non-zero prior PID that isn't ours), the inherited package name +
   * exec ranges are the parent's — drop them so the refresh + maps walk below
   * re-establish OUR identity.  On the very first seed (prior PID 0) we only
   * record our PID and clear NOTHING, so the package name goBridgeInit just set
   * is preserved (re-resolving it would be fragile under stealth soinfo
   * unlinking).  Cheap (cached getpid()), off the hot path (runs on dlopen /
   * first-seed, not per JNI event). */
  pid_t me = getpid();
  pid_t prev = __atomic_load_n(&g_seed_pid, __ATOMIC_ACQUIRE);
  if (prev != me) {
    if (prev != 0) {
      c_clear_exec_ranges();
      pthread_mutex_lock(&g_range_pkg_lock);
      g_range_package_name[0] = '\0';
      pthread_mutex_unlock(&g_range_pkg_lock);
      __atomic_store_n(&g_seed_attempted, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_seed_pid, me, __ATOMIC_RELEASE);
  }

  /* Refresh the package name first — the process may have been specialized
   * (renamed from zygote64 to com.termux) since the last attempt. */
  refresh_package_name_if_needed();

  int added = 0;

  /* Snapshot the package name under the lock; the callback sees a stable copy. */
  seed_ctx_t ctx;
  pthread_mutex_lock(&g_range_pkg_lock);
  strncpy(ctx.pkg_snap, g_range_package_name, sizeof(ctx.pkg_snap) - 1);
  ctx.pkg_snap[sizeof(ctx.pkg_snap) - 1] = '\0';
  pthread_mutex_unlock(&g_range_pkg_lock);
  ctx.added = 0;

  /* Self-exclusion is keyed off the identity range captured at init
   * (c_capture_self_range), not a per-reseed dladdr(self) — which returns NULL
   * once the injector has vma-hidden us. */

  /* Only iterate when we have a real package name. The seed-attempted flag in
   * c_should_try_seed prevents repeat iteration once we've tried; the previous
   * code's link_map-tail-identity skip optimization is no longer needed. */
  if (ctx.pkg_snap[0] != '\0' &&
      strstr(ctx.pkg_snap, "zygote") == NULL &&
      strstr(ctx.pkg_snap, "app_process") == NULL &&
      strstr(ctx.pkg_snap, "<pre-initialized>") == NULL) {
    /* dl_iterate_phdr holds bionic's loader lock while iterating, so we
     * can't be racing with dlopen/dlclose on another thread. */
    dl_iterate_phdr(seed_iter_cb, &ctx);
  }

  added += ctx.added;

  /* Fallback: if link_map scan found nothing, try /proc/self/maps directly.
   * This catches libraries in classloader namespaces (System.loadLibrary). */
  if (!c_has_exec_ranges()) {
    added += c_seed_exec_ranges_from_proc_maps();
#if __has_include(<android/log.h>)
    if (added == 0) {
      char warn_snap[RANGESET_MAX_PACKAGE_NAME];
      pthread_mutex_lock(&g_range_pkg_lock);
      strncpy(warn_snap, g_range_package_name, sizeof(warn_snap) - 1);
      warn_snap[sizeof(warn_snap) - 1] = '\0';
      pthread_mutex_unlock(&g_range_pkg_lock);
      if (warn_snap[0] != '\0' && strstr(warn_snap, "zygote") == NULL) {
        __android_log_print(ANDROID_LOG_WARN, RANGESET_LOG_TAG,
                            "seed fallback: no ranges found for pkg '%s' (maps scan returned 0)",
                            warn_snap);
      }
    }
#endif
  }

  /* Mark that we've attempted seeding.  If we found nothing, the hot path
   * won't retry until something resets this flag (dlopen, set_package_name). */
  __atomic_store_n(&g_seed_attempted, 1, __ATOMIC_RELEASE);

  return added;
}

/* Fallback: scan /proc/self/maps for executable regions matching the package name.
 * This catches libraries loaded via classloader namespaces that don't appear in
 * the global link_map (e.g., app native libraries loaded via System.loadLibrary). */
static int c_seed_exec_ranges_from_proc_maps(void) {
  /* Snapshot g_range_package_name under the lock; refresh_package_name_if_needed
   * can torn-overwrite it from another thread. */
  char pkg_snap[RANGESET_MAX_PACKAGE_NAME];
  pthread_mutex_lock(&g_range_pkg_lock);
  strncpy(pkg_snap, g_range_package_name, sizeof(pkg_snap) - 1);
  pkg_snap[sizeof(pkg_snap) - 1] = '\0';
  pthread_mutex_unlock(&g_range_pkg_lock);
  if (pkg_snap[0] == '\0' ||
      strstr(pkg_snap, "zygote") != NULL ||
      strstr(pkg_snap, "app_process") != NULL ||
      strstr(pkg_snap, "<pre-initialized>") != NULL) {
    return 0;
  }

  jl_linereader lr;
  if (jl_lr_open(&lr, "/proc/self/maps", O_RDONLY | O_CLOEXEC) < 0) return 0;

  int added = 0;
  /* 1 KB line buffer so a long split-APK path
   * (/data/app/~~base64==/pkg==/lib/arm64/libfoo.so [ (deleted)]) is not
   * truncated at 512 B, which used to silently drop that lib from seeding (F12). */
  char line[1024];

  while (jl_lr_next(&lr, line, sizeof(line))) {
    /* Parse: start-end perms offset dev inode pathname */
    uintptr_t start, end;
    char perms[5];
    if (!jl_parse_maps_head(line, &start, &end, perms)) continue;

    /* Only executable regions */
    if (perms[2] != 'x') continue;

    /* Find the pathname (after the 5th field) */
    char *path = NULL;
    int spaces = 0;
    for (char *p = line; *p; p++) {
      if (*p == ' ' || *p == '\t') {
        spaces++;
        while (*(p+1) == ' ' || *(p+1) == '\t') p++;
        if (spaces == 5) { path = p + 1; break; }
      }
    }
    if (!path) continue;

    /* Trim trailing newline */
    size_t plen = strlen(path);
    if (plen > 0 && path[plen-1] == '\n') path[plen-1] = '\0';
    if (path[0] == '\0') continue;

    /* The kernel appends " (deleted)" once the injector unlinks our staged
     * file; strip it so the self-path compare below matches dli_fname. */
    char *deleted = strstr(path, " (deleted)");
    if (deleted) *deleted = '\0';

    /* Skip system libraries — but for /proc/self/maps we use a simpler check
     * that doesn't exclude .odex (OAT files contain app code) */
    if (path[0] != '/') continue;
    if (strncmp(path, "/system/", 8) == 0 ||
        strncmp(path, "/system_ext/", 12) == 0 ||
        strncmp(path, "/apex/", 6) == 0 ||
        strncmp(path, "/vendor/", 8) == 0 ||
        strncmp(path, "/product/", 9) == 0 ||
        strncmp(path, "/odm/", 5) == 0 ||
        strstr(path, "/bionic") != NULL) continue;

    /* Skip our own payload by load-base identity (rename-proof, and the only
     * thing that survives the injector's vma_hide / soinfo-unlink). */
    if (c_is_self_addr(start)) continue;

    /* Check if path contains the package name. Use the locked snapshot
     * (pkg_snap) taken at the top of this function — reading
     * g_range_package_name raw here would race a concurrent
     * refresh_package_name_if_needed writer mid-strstr. */
    if (strstr(path, pkg_snap) == NULL) continue;

    uintptr_t size = end - start;
    if (size > 0 && c_add_exec_range(start, size)) {
#if __has_include(<android/log.h>)
      __android_log_print(ANDROID_LOG_DEBUG, RANGESET_LOG_TAG,
                          C_DIM "range added (maps):" C_RESET " " C_LAVENDER "%lx-%lx" C_RESET " " C_YELLOW "%s" C_RESET,
                          (unsigned long)start, (unsigned long)end, path);
#endif
      added++;
    }
  }

  jl_lr_close(&lr);
  return added;
}

/* ====================================================================
 * Initialization: read package name from /proc/self/cmdline, seed ranges
 * ==================================================================== */

void c_init_range_tracking(void) {
  /* Capture our own [base,end] now, while we are still resolvable — the injector
   * vma-hides and soinfo-unlinks us shortly after dlopen returns. This is THE
   * self-exclusion (c_capture_self_range); seeding and should_log_from_caller
   * both key off it. */
  c_capture_self_range();

  refresh_package_name_if_needed();

  pthread_mutex_lock(&g_range_pkg_lock);
  char buf[RANGESET_MAX_PACKAGE_NAME];
  strncpy(buf, g_range_package_name, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  pthread_mutex_unlock(&g_range_pkg_lock);

  if (buf[0] == '\0') {
#if __has_include(<android/log.h>)
    __android_log_print(ANDROID_LOG_WARN, RANGESET_LOG_TAG,
                        "c_init_range_tracking: package name still empty (will re-seed lazily)");
#endif
    return;
  }

#if __has_include(<android/log.h>)
  __android_log_print(ANDROID_LOG_INFO, RANGESET_LOG_TAG,
                      "c_init_range_tracking: package=" C_CYAN "%s" C_RESET, buf);
#endif

  int added = c_seed_exec_ranges_from_maps();

#if __has_include(<android/log.h>)
  if (added > 0) {
    __android_log_print(
        ANDROID_LOG_INFO, RANGESET_LOG_TAG,
        "c_init_range_tracking: seeded " C_GREEN "%d" C_RESET " executable ranges", added);
  } else {
    __android_log_print(ANDROID_LOG_WARN, RANGESET_LOG_TAG,
                        "c_init_range_tracking: no ranges matched "
                        "package '" C_CYAN "%s" C_RESET "' (will re-seed lazily)",
                        buf);
  }
#endif
}
