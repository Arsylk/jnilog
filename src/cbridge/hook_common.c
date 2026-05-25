#include "hook_internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

PJNINativeInterface g_original_jni_table = NULL;
pthread_mutex_t g_hook_lock = PTHREAD_MUTEX_INITIALIZER;

static method_sig_entry_t g_method_cache[METHOD_CACHE_SIZE];
static pthread_rwlock_t g_method_cache_lock = PTHREAD_RWLOCK_INITIALIZER;

static field_sig_entry_t g_field_cache[METHOD_CACHE_SIZE];
static pthread_rwlock_t g_field_cache_lock = PTHREAD_RWLOCK_INITIALIZER;

/* g_in_hook has counter semantics rather than a plain 0/1 flag. Every
 * `set_reentrant_call(1)` increments; every `set_reentrant_call(0)` decrements
 * (saturating at zero). `is_reentrant_call()` reports true whenever the
 * thread is inside ANY hook scope.
 *
 * Why a counter: vis_* helpers nest inside one another (e.g.
 * vis_object_class_name → vis_class_name → CallObjectMethod), and each one
 * brackets its own JNI sub-calls with set(1)/set(0). With plain 0/1 flag
 * semantics, the inner helper's set(0) on exit would clear the flag for the
 * still-active outer scope — and the next JNI call from the original code
 * would re-enter the hooks as non-reentrant, recursing infinitely. The
 * counter pattern lets `set(1)` / `set(0)` pairs nest correctly without
 * touching the ~30 individual brackets across visualize.c / hooks.c. */
static __thread int g_in_hook = 0;
static __thread int g_jni_critical = 0;

int is_reentrant_call(void) { return g_in_hook > 0; }
void set_reentrant_call(int val) {
  if (val) {
    g_in_hook++;
  } else if (g_in_hook > 0) {
    g_in_hook--;
  }
}

int is_jni_critical(void) { return g_jni_critical; }
void set_jni_critical(int val) { g_jni_critical = val; }

void address_of_r(void *addr, char *buf, size_t bufsz) {
  if (buf == NULL || bufsz == 0u) return;
  if (addr == NULL) {
    (void)snprintf(buf, bufsz, "null");
    return;
  }
  Dl_info info;
  if (dladdr(addr, &info) && info.dli_fname != NULL) {
    const char *name = strrchr(info.dli_fname, '/');
    name = name ? name + 1 : info.dli_fname;
    if (info.dli_sname != NULL) {
      (void)snprintf(buf, bufsz, "%s!%s+0x%lx", name, info.dli_sname,
                    (unsigned long)((uintptr_t)addr - (uintptr_t)info.dli_saddr));
      return;
    }
    (void)snprintf(buf, bufsz, "%s!0x%lx", name,
                   (unsigned long)((uintptr_t)addr - (uintptr_t)info.dli_fbase));
    return;
  }
  (void)snprintf(buf, bufsz, "0x%lx", (unsigned long)(uintptr_t)addr);
}

static size_t method_cache_hash(jmethodID method_id) {
  uintptr_t x = (uintptr_t)method_id;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  x ^= x >> 33;
  return (size_t)(x & (METHOD_CACHE_SIZE - 1u));
}

void cache_method_signature(jmethodID method_id, const char *name, const char *sig, const char *clazz) {
  if (method_id == NULL || sig == NULL || sig[0] == '\0') return;
  (void)pthread_rwlock_wrlock(&g_method_cache_lock);
  size_t start = method_cache_hash(method_id);
  size_t first_free = METHOD_CACHE_SIZE;
  for (size_t n = 0; n < METHOD_CACHE_SIZE; ++n) {
    size_t idx = (start + n) & (METHOD_CACHE_SIZE - 1u);
    method_sig_entry_t *entry = &g_method_cache[idx];
    if (entry->in_use) {
      if (entry->method_id == method_id) {
        if (name) {
          (void)strncpy(entry->name, name, METHOD_NAME_MAX - 1u);
          entry->name[METHOD_NAME_MAX - 1u] = '\0';
        }
        if (clazz) {
          (void)strncpy(entry->clazz, clazz, METHOD_NAME_MAX - 1u);
          entry->clazz[METHOD_NAME_MAX - 1u] = '\0';
        }
        (void)strncpy(entry->signature, sig, METHOD_SIG_MAX - 1u);
        entry->signature[METHOD_SIG_MAX - 1u] = '\0';
        (void)pthread_rwlock_unlock(&g_method_cache_lock);
        return;
      }
      continue;
    }
    if (first_free == METHOD_CACHE_SIZE) first_free = idx;
    break;
  }
  if (first_free != METHOD_CACHE_SIZE) {
    method_sig_entry_t *entry = &g_method_cache[first_free];
    entry->method_id = method_id;
    if (name) {
      (void)strncpy(entry->name, name, METHOD_NAME_MAX - 1u);
      entry->name[METHOD_NAME_MAX - 1u] = '\0';
    } else entry->name[0] = '\0';
    if (clazz) {
      (void)strncpy(entry->clazz, clazz, METHOD_NAME_MAX - 1u);
      entry->clazz[METHOD_NAME_MAX - 1u] = '\0';
    } else entry->clazz[0] = '\0';
    (void)strncpy(entry->signature, sig, METHOD_SIG_MAX - 1u);
    entry->signature[METHOD_SIG_MAX - 1u] = '\0';
    entry->in_use = 1;
  }
  (void)pthread_rwlock_unlock(&g_method_cache_lock);
}

method_info_t lookup_method_info(jmethodID method_id) {
  method_info_t info = {NULL, NULL, NULL};
  if (method_id == NULL) return info;
  (void)pthread_rwlock_rdlock(&g_method_cache_lock);
  size_t start = method_cache_hash(method_id);
  for (size_t n = 0; n < METHOD_CACHE_SIZE; ++n) {
    size_t idx = (start + n) & (METHOD_CACHE_SIZE - 1u);
    const method_sig_entry_t *entry = &g_method_cache[idx];
    if (!entry->in_use) {
      (void)pthread_rwlock_unlock(&g_method_cache_lock);
      return info;
    }
    if (entry->method_id == method_id) {
      info.name = entry->name;
      info.sig = entry->signature;
      info.clazz = entry->clazz;
      (void)pthread_rwlock_unlock(&g_method_cache_lock);
      return info;
    }
  }
  (void)pthread_rwlock_unlock(&g_method_cache_lock);
  return info;
}

static size_t field_cache_hash(jfieldID field_id) {
  uintptr_t x = (uintptr_t)field_id;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  x ^= x >> 33;
  return (size_t)(x & (METHOD_CACHE_SIZE - 1u));
}

void cache_field_signature(jfieldID field_id, const char *name, const char *sig, const char *clazz) {
  if (field_id == NULL || sig == NULL || sig[0] == '\0') return;
  (void)pthread_rwlock_wrlock(&g_field_cache_lock);
  size_t start = field_cache_hash(field_id);
  size_t first_free = METHOD_CACHE_SIZE;
  for (size_t n = 0; n < METHOD_CACHE_SIZE; ++n) {
    size_t idx = (start + n) & (METHOD_CACHE_SIZE - 1u);
    field_sig_entry_t *entry = &g_field_cache[idx];
    if (entry->in_use) {
      if (entry->field_id == field_id) {
        if (name) {
          (void)strncpy(entry->name, name, METHOD_NAME_MAX - 1u);
          entry->name[METHOD_NAME_MAX - 1u] = '\0';
        }
        if (clazz) {
          (void)strncpy(entry->clazz, clazz, METHOD_NAME_MAX - 1u);
          entry->clazz[METHOD_NAME_MAX - 1u] = '\0';
        }
        (void)strncpy(entry->signature, sig, METHOD_SIG_MAX - 1u);
        entry->signature[METHOD_SIG_MAX - 1u] = '\0';
        (void)pthread_rwlock_unlock(&g_field_cache_lock);
        return;
      }
      continue;
    }
    if (first_free == METHOD_CACHE_SIZE) first_free = idx;
    break;
  }
  if (first_free != METHOD_CACHE_SIZE) {
    field_sig_entry_t *entry = &g_field_cache[first_free];
    entry->field_id = field_id;
    if (name) {
      (void)strncpy(entry->name, name, METHOD_NAME_MAX - 1u);
      entry->name[METHOD_NAME_MAX - 1u] = '\0';
    } else entry->name[0] = '\0';
    if (clazz) {
      (void)strncpy(entry->clazz, clazz, METHOD_NAME_MAX - 1u);
      entry->clazz[METHOD_NAME_MAX - 1u] = '\0';
    } else entry->clazz[0] = '\0';
    (void)strncpy(entry->signature, sig, METHOD_SIG_MAX - 1u);
    entry->signature[METHOD_SIG_MAX - 1u] = '\0';
    entry->in_use = 1;
  }
  (void)pthread_rwlock_unlock(&g_field_cache_lock);
}

field_info_t lookup_field_info(jfieldID field_id) {
  field_info_t info = {NULL, NULL, NULL};
  if (field_id == NULL) return info;

  const char* art_name = art_get_field_name(field_id);
  
  (void)pthread_rwlock_rdlock(&g_field_cache_lock);
  size_t start = field_cache_hash(field_id);
  for (size_t n = 0; n < METHOD_CACHE_SIZE; ++n) {
    size_t idx = (start + n) & (METHOD_CACHE_SIZE - 1u);
    const field_sig_entry_t *entry = &g_field_cache[idx];
    if (!entry->in_use) {
      break;
    }
    if (entry->field_id == field_id) {
      info.name = entry->name;
      info.sig = entry->signature;
      info.clazz = entry->clazz;
      break;
    }
  }
  (void)pthread_rwlock_unlock(&g_field_cache_lock);

  if (art_name && info.name == NULL) {
    static __thread char art_name_buf[128];
    strncpy(art_name_buf, art_name, sizeof(art_name_buf)-1);
    info.name = art_name_buf;
  }

  return info;
}

/* PairIP integrity-check filter.
 *
 * PairIP (libpairipcore.so) is Google Play Asset Pairing Integrity Protection's
 * bytecode VM. It walks the app's JNI call graph and times JNI dispatches to
 * detect tampering.  When jnilog's hook bodies fire `log_jni_return` for
 * PairIP-issued JNI calls, the per-call cgo overhead into Go's runtime trips
 * a PairIP integrity check (the only log_jni_* function that does so —
 * confirmed by isolated bisection: lookup-only and call-only both survive,
 * return-only kills).
 *
 * Fix: skip all jnilog work when the caller PC is inside libpairipcore.so.
 * PairIP gets clean passthrough; every other caller still gets full logging.
 *
 * The pairip range is resolved lazily on first hook fire via /proc/self/maps
 * walk and cached.  A 0 end-address indicates "no pairip mapping in process"
 * (most apps), in which case the check is a no-op single comparison.
 */
#include <pthread.h>
#include <stdint.h>
static uintptr_t g_pairip_start = 0;
static uintptr_t g_pairip_end   = 0;
static int       g_pairip_scanned = 0;

/* Cheap range cache for libpairipcore.so.  We do NOT cache "not found" — only
 * cache the positive result.  libpairipcore.so is loaded by chatgpt's class
 * loader AFTER our payload's constructor runs, so an early scan returns
 * nothing and we keep trying until the library appears.  Once found, the
 * cache prevents per-call /proc/self/maps walks. */
/* dl_iterate_phdr callback collects pairip's load span. */
struct pairip_collect { uintptr_t lo, hi; int matched; };
static int pairip_iter_cb(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  struct pairip_collect *c = (struct pairip_collect *)data;
  if (!info->dlpi_name || !strstr(info->dlpi_name, "libpairipcore.so")) return 0;
  for (unsigned i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *p = &info->dlpi_phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
    uintptr_t s = info->dlpi_addr + p->p_vaddr;
    uintptr_t e = s + p->p_memsz;
    if (c->lo == 0 || s < c->lo) c->lo = s;
    if (e > c->hi) c->hi = e;
  }
  c->matched++;
  return 0;
}

static void scan_for_pairip(void) {
  struct pairip_collect c = {0, 0, 0};
  dl_iterate_phdr(pairip_iter_cb, &c);
  if (c.hi > 0) {
    __atomic_store_n(&g_pairip_start, c.lo, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pairip_end,   c.hi, __ATOMIC_RELEASE);
    LOG_DIRECT(ANDROID_LOG_INFO,
               "pairip range cached: [%p, %p) — JNI calls from this range will not be logged",
               (void*)c.lo, (void*)c.hi);
  }
}

static int caller_is_pairip(uintptr_t pc) {
  uintptr_t end = __atomic_load_n(&g_pairip_end, __ATOMIC_ACQUIRE);
  if (end == 0) {
    /* Cache empty — re-scan lazily.  Bounded by "library either appears
     * shortly after app boot or never" — costs one scan per JNI hook
     * during the brief window before libpairipcore loads. */
    scan_for_pairip();
    end = __atomic_load_n(&g_pairip_end, __ATOMIC_ACQUIRE);
    if (end == 0) return 0;
  }
  uintptr_t start = __atomic_load_n(&g_pairip_start, __ATOMIC_RELAXED);
  return pc >= start && pc < end;
}

int should_log_from_caller(JNIEnv *env, void *caller) {
  if (env == NULL || caller == NULL) return 0;
  if (c_should_try_seed()) c_seed_exec_ranges_from_maps();
  if (!c_has_exec_ranges()) return 0;
  (void)caller_is_pairip;  /* old PairIP-skip retained for reference; unused now */
  return c_is_in_exec_range((uintptr_t)caller);
}

/* ── Config query cache ───────────────────────────────────────────────────
 * Each JNI function name crosses cgo exactly once.  Subsequent hits do an
 * O(1) pointer comparison in a linear-probe hash table.
 * ──────────────────────────────────────────────────────────────────────── */
#define CONFIG_CACHE_SIZE 256
#define CONFIG_CACHE_MASK (CONFIG_CACHE_SIZE - 1)

typedef struct {
  const char *name;
  int         result;   /* -1 = unchecked, 0 = blocked, 1 = allowed */
} config_cache_entry_t;

static config_cache_entry_t g_cfg_cache[CONFIG_CACHE_SIZE];
static pthread_mutex_t      g_cfg_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int                  g_cfg_cache_init = 0;

static uint32_t cfg_hash(const char *s) {
  uint32_t h = 5381;
  while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
  return h;
}

static int cfg_lookup(const char *name, int *out) {
  pthread_mutex_lock(&g_cfg_cache_lock);
  if (!g_cfg_cache_init) {
    memset(g_cfg_cache, 0, sizeof(g_cfg_cache));
    for (int i = 0; i < CONFIG_CACHE_SIZE; i++) g_cfg_cache[i].result = -1;
    g_cfg_cache_init = 1;
  }
  uint32_t hash = cfg_hash(name);
  for (int i = 0; i < CONFIG_CACHE_SIZE; i++) {
    uint32_t idx = (hash + (uint32_t)i) & CONFIG_CACHE_MASK;
    config_cache_entry_t *e = &g_cfg_cache[idx];
    if (e->result == -1) {
      pthread_mutex_unlock(&g_cfg_cache_lock);
      int allowed = 1;
      if (config_function_blacklisted((char *)name)) allowed = 0;
      else if (!config_function_enabled((char *)name)) allowed = 0;
      pthread_mutex_lock(&g_cfg_cache_lock);
      /* Re-check the slot — another thread may have populated it. */
      if (e->result == -1) { e->name = name; e->result = allowed; }
      *out = e->result;
      pthread_mutex_unlock(&g_cfg_cache_lock);
      return 1;
    }
    if (e->name == name) { *out = e->result; pthread_mutex_unlock(&g_cfg_cache_lock); return 1; }
    if (e->name && strcmp(e->name, name) == 0) { e->name = name; *out = e->result; pthread_mutex_unlock(&g_cfg_cache_lock); return 1; }
  }
  pthread_mutex_unlock(&g_cfg_cache_lock);
  /* Cache full — fall back to direct Go-side query. */
  if (config_function_blacklisted((char *)name)) { *out = 0; return 1; }
  if (!config_function_enabled((char *)name))  { *out = 0; return 1; }
  *out = 1; return 1;
}

int config_is_allowed(const char *name) {
  if (!name) return 0;
  int result = 0;
  return cfg_lookup(name, &result) && result;
}

int should_log_jni(JNIEnv *env, void *caller, const char *jni_name) {
  if (!config_is_allowed(jni_name)) return 0;
  return should_log_from_caller(env, caller);
}

void log_missing_original(const char *name, int should_log) {
  if (should_log) log_native_warn("hook call without original JNI entry: %s", name);
}

int has_no_exception(JNIEnv *env) {
  return env != NULL && g_original_jni_table != NULL &&
         g_original_jni_table->ExceptionCheck != NULL &&
         !g_original_jni_table->ExceptionCheck(env);
}

int protect_region(prot_region_t *region, int prot) {
  if (!region || !region->page_start) return -1;
  return mprotect(region->page_start, region->region_size, prot);
}

prot_region_t jni_table_region(void *table, size_t table_size) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) page_size = 4096;
  uintptr_t start = (uintptr_t)table;
  uintptr_t page_start = start & ~((uintptr_t)page_size - 1u);
  uintptr_t end = start + table_size;
  uintptr_t page_end = (end + (uintptr_t)page_size - 1u) & ~((uintptr_t)page_size - 1u);
  prot_region_t region;
  region.page_start = (void *)page_start;
  region.region_size = (size_t)(page_end - page_start);
  return region;
}
