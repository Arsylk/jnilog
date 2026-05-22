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

#if __has_include(<android/log.h>)
#include <android/log.h>
#endif

#define RANGESET_LOG_TAG "JNILog"

/* ANSI color codes for logcat output */
#define C_RESET   "\x1b[0m"
#define C_DIM     "\x1b[2m"
#define C_CYAN    "\x1b[36m"
#define C_YELLOW  "\x1b[33m"
#define C_GREEN   "\x1b[32m"
#define C_ORANGE  "\x1b[38;5;214m"
#define C_LAVENDER "\x1b[38;2;180;190;254m"

/* ====================================================================
 * Package name
 * ==================================================================== */

#define RANGESET_MAX_PACKAGE_NAME 256

static char g_range_package_name[RANGESET_MAX_PACKAGE_NAME] = {0};
static pthread_mutex_t g_range_pkg_lock = PTHREAD_MUTEX_INITIALIZER;

static struct link_map *g_last_link_map = NULL;

/* Forward declaration — defined here because c_set_package_name (below) resets
 * it.  Prevents hot-path retry storms: once we've attempted seeding and found
 * nothing, we don't retry on every JNI call.  Reset when something changes
 * (new library loaded via dlopen, package name set). */
static volatile int g_seed_attempted = 0;

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

  /* Reset the link_map cursor so the next c_seed_exec_ranges_from_maps()
   * performs a full rescan even if it already ran before the name was known. */
  g_last_link_map = NULL;

  /* Reset the seed-attempted flag so the hot path will re-attempt seeding
   * now that we have a valid package name. */
  g_seed_attempted = 0;
}

const char *c_get_package_name(void) {
  /* Safe after init — package name is set once and never changed */
  return g_range_package_name;
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
          g_last_link_map = NULL;
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
  FILE *f = fopen("/proc/self/cmdline", "r");
  if (f != NULL) {
    char buf[RANGESET_MAX_PACKAGE_NAME];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n > 0) {
      size_t len = 0;
      while (len < n && buf[len] != '\0') len++;
      buf[len] = '\0';
      if (len > 0 &&
          strstr(buf, "zygote") == NULL &&
          strstr(buf, "app_process") == NULL &&
          strstr(buf, "<pre-initialized>") == NULL) {
        strncpy(g_range_package_name, buf, RANGESET_MAX_PACKAGE_NAME - 1);
        g_range_package_name[RANGESET_MAX_PACKAGE_NAME - 1] = '\0';
        g_last_link_map = NULL;
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
  if (g_range_package_name[0] == '\0')
    return 0;
  return strstr(path, g_range_package_name) != NULL ? 1 : 0;
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
    g_exec_range_count++;
    pthread_mutex_unlock(&g_range_lock);
    return 1;
  }

  pthread_mutex_unlock(&g_range_lock);
  return 0;
}

int c_is_in_exec_range(uintptr_t addr) {
  if (addr == 0)
    return 0;

  /* Snapshot count under lock, then scan locklessly (ranges are append-only) */
  pthread_mutex_lock(&g_range_lock);
  int count = g_exec_range_count;
  pthread_mutex_unlock(&g_range_lock);

  for (int i = 0; i < count; i++) {
    if (addr >= g_exec_ranges[i].base && addr < g_exec_ranges[i].end) {
      return 1;
    }
  }
  return 0;
}

int c_has_exec_ranges(void) { return g_exec_range_count > 0; }

/* Returns true if we should attempt seeding (either we have no ranges AND
 * haven't tried yet, or we already have ranges).  This prevents the hot-path
 * from calling c_seed_exec_ranges_from_maps() on every JNI hook. */
int c_should_try_seed(void) {
  if (g_exec_range_count > 0) return 0;  /* already have ranges */
  if (g_seed_attempted) return 0;         /* already tried and failed */
  return 1;
}

/* Reset the seed-attempted flag so the next should_log_from_caller will
 * re-attempt seeding.  Called when something changes (dlopen, package name). */
void c_reset_seed_attempted(void) {
  g_seed_attempted = 0;
}

/* ====================================================================
 * solist scanning via link_map
 * ==================================================================== */

#include <sys/auxv.h>

static struct r_debug *find_r_debug(void) {
  ElfW(Phdr) *phdr = (ElfW(Phdr) *)getauxval(AT_PHDR);
  if (!phdr) return NULL;

  unsigned long phnum = getauxval(AT_PHNUM);
  if (!phnum) return NULL;

  ElfW(Addr) pt_phdr_vaddr = 0;
  ElfW(Addr) pt_dynamic_vaddr = 0;
  int found_phdr = 0, found_dynamic = 0;

  for (unsigned long i = 0; i < phnum; i++) {
    if (phdr[i].p_type == PT_PHDR) {
      pt_phdr_vaddr = phdr[i].p_vaddr;
      found_phdr = 1;
    } else if (phdr[i].p_type == PT_DYNAMIC) {
      pt_dynamic_vaddr = phdr[i].p_vaddr;
      found_dynamic = 1;
    }
  }

  if (!found_phdr || !found_dynamic) return NULL;

  uintptr_t load_base = (uintptr_t)phdr - pt_phdr_vaddr;
  ElfW(Dyn) *dyn = (ElfW(Dyn) *)(load_base + pt_dynamic_vaddr);

  for (; dyn->d_tag != DT_NULL; dyn++) {
    if (dyn->d_tag == DT_DEBUG) {
      return (struct r_debug *)(uintptr_t)dyn->d_un.d_ptr;
    }
  }

  return NULL;
}

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

int c_seed_exec_ranges_from_maps(void) {
  /* Refresh the package name first — the process may have been specialized
   * (renamed from zygote64 to com.termux) since the last attempt. */
  refresh_package_name_if_needed();

  int added = 0;

  struct r_debug *dbg = find_r_debug();
  if (dbg && dbg->r_map) {
    struct link_map *last_map = dbg->r_map;
    while (last_map->l_next) {
      last_map = last_map->l_next;
    }

    /* Skip re-scan only when the link_map tail hasn't changed AND we already
     * have a valid package name (otherwise the previous scan was a no-op). */
    int skip_link_map = (g_last_link_map == last_map && g_range_package_name[0] != '\0' &&
        strstr(g_range_package_name, "zygote") == NULL &&
        strstr(g_range_package_name, "app_process") == NULL &&
        strstr(g_range_package_name, "<pre-initialized>") == NULL);

    if (!skip_link_map) {
      g_last_link_map = last_map;

      for (struct link_map *map = dbg->r_map; map; map = map->l_next) {
        const char* lib_name = map->l_name;
        Dl_info info;
        if (dladdr((void*)map->l_addr, &info) && info.dli_fname != NULL && info.dli_fname[0] != '\0') {
          lib_name = info.dli_fname;
        }

        if (lib_name == NULL || lib_name[0] == '\0') {
          continue;
        }

        /* Exclude our own payload module by load base address */
        Dl_info self_info;
        if (dladdr((void*)c_seed_exec_ranges_from_maps, &self_info)) {
          if (map->l_addr == (ElfW(Addr))self_info.dli_fbase) {
            continue;
          }
        }

        /* Fallback string-based exclusion */
        if (strstr(lib_name, "libjnilog") != NULL) {
          continue;
        }

        int has_pkg = c_path_contains_package(lib_name);
        if (!has_pkg) {
          continue;
        }

        ElfW(Addr) load_bias = map->l_addr;
        ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)load_bias;

    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
      continue;
    }

    ElfW(Phdr) *phdr = (ElfW(Phdr) *)((uintptr_t)ehdr + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
      if (phdr[i].p_type == PT_LOAD) {
        uintptr_t start = (uintptr_t)(load_bias + phdr[i].p_vaddr);
        uintptr_t size_mem = phdr[i].p_memsz;
        
        if (size_mem > 0 && c_add_exec_range(start, size_mem)) {
#if __has_include(<android/log.h>)
          __android_log_print(ANDROID_LOG_DEBUG, RANGESET_LOG_TAG,
                              C_DIM "range added:" C_RESET " " C_LAVENDER "%lx-%lx" C_RESET " " C_YELLOW "%s" C_RESET,
                              (unsigned long)start, (unsigned long)(start + size_mem), map->l_name);
#endif
          added++;
        }
      }
    }
    } /* end for (link_map) */
    } /* end if (!skip_link_map) */
  } /* end if (dbg && dbg->r_map) */

  /* Fallback: if link_map scan found nothing, try /proc/self/maps directly.
   * This catches libraries in classloader namespaces (System.loadLibrary). */
  if (!c_has_exec_ranges()) {
    added += c_seed_exec_ranges_from_proc_maps();
#if __has_include(<android/log.h>)
    if (added == 0 && g_range_package_name[0] != '\0' &&
        strstr(g_range_package_name, "zygote") == NULL) {
      __android_log_print(ANDROID_LOG_WARN, RANGESET_LOG_TAG,
                          "seed fallback: no ranges found for pkg '%s' (maps scan returned 0)",
                          g_range_package_name);
    }
#endif
  }

  /* Mark that we've attempted seeding.  If we found nothing, the hot path
   * won't retry until something resets this flag (dlopen, set_package_name). */
  g_seed_attempted = 1;

  return added;
}

/* Fallback: scan /proc/self/maps for executable regions matching the package name.
 * This catches libraries loaded via classloader namespaces that don't appear in
 * the global link_map (e.g., app native libraries loaded via System.loadLibrary). */
static int c_seed_exec_ranges_from_proc_maps(void) {
  if (g_range_package_name[0] == '\0' ||
      strstr(g_range_package_name, "zygote") != NULL ||
      strstr(g_range_package_name, "app_process") != NULL ||
      strstr(g_range_package_name, "<pre-initialized>") != NULL) {
    return 0;
  }

  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return 0;

  int added = 0;
  char line[512];

  while (fgets(line, sizeof(line), f)) {
    /* Parse: start-end perms offset dev inode pathname */
    uintptr_t start, end;
    char perms[5];
    int n = sscanf(line, "%lx-%lx %4s", (unsigned long*)&start, (unsigned long*)&end, perms);
    if (n < 3) continue;

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

    /* Skip our own payload */
    if (strstr(path, "libjnilog") != NULL) continue;

    /* Check if path contains the package name */
    if (strstr(path, g_range_package_name) == NULL) continue;

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

  fclose(f);
  return added;
}

/* ====================================================================
 * Initialization: read package name from /proc/self/cmdline, seed ranges
 * ==================================================================== */

void c_init_range_tracking(void) {
  refresh_package_name_if_needed();

  pthread_mutex_lock(&g_range_pkg_lock);
  char buf[RANGESET_MAX_PACKAGE_NAME];
  strncpy(buf, g_range_package_name, sizeof(buf));
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
