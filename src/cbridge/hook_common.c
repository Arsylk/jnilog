#include "hook_internal.h"
#include <dlfcn.h>
#include <errno.h>
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
    /* jl_dladdr_synth is set when addr is in the module's in-memory-only region
     * (a packer's decrypted/JIT .bss absorbed past the file image). Emit a
     * "<lib>+0x<off>" WIRE form (vs the on-disk "<lib>!0x<off>"). The '+' is only
     * an internal discriminator for the Go renderer (formatAddress), which
     * displays both with the project's "<lib>!offset" structure but colors this
     * one's "!" differently — so the offset is understood as into the runtime
     * image, not a byte offset to look up in the on-disk library. */
    (void)snprintf(buf, bufsz, jl_dladdr_synth ? "%s+0x%lx" : "%s!0x%lx", name,
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

method_info_t lookup_method_info(jmethodID method_id,
                                 char *name_buf, size_t name_sz,
                                 char *sig_buf, size_t sig_sz,
                                 char *clazz_buf, size_t clazz_sz) {
  method_info_t info = {NULL, NULL, NULL};
  if (name_sz)  name_buf[0]  = '\0';
  if (sig_sz)   sig_buf[0]   = '\0';
  if (clazz_sz) clazz_buf[0] = '\0';
  if (method_id == NULL) return info;
  (void)pthread_rwlock_rdlock(&g_method_cache_lock);
  size_t start = method_cache_hash(method_id);
  for (size_t n = 0; n < METHOD_CACHE_SIZE; ++n) {
    size_t idx = (start + n) & (METHOD_CACHE_SIZE - 1u);
    const method_sig_entry_t *entry = &g_method_cache[idx];
    if (!entry->in_use) break;
    if (entry->method_id == method_id) {
      /* Copy each field into the caller's buffer WHILE holding the read lock,
       * so a concurrent cache_method_signature wrlock cannot torn-overwrite a
       * slot between the lookup and the copy (F10). */
      if (name_sz)  { strncpy(name_buf,  entry->name,      name_sz  - 1u); name_buf[name_sz   - 1u] = '\0'; info.name  = name_buf; }
      if (sig_sz)   { strncpy(sig_buf,   entry->signature, sig_sz   - 1u); sig_buf[sig_sz     - 1u] = '\0'; info.sig   = sig_buf; }
      if (clazz_sz) { strncpy(clazz_buf, entry->clazz,     clazz_sz - 1u); clazz_buf[clazz_sz - 1u] = '\0'; info.clazz = clazz_buf; }
      break;
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

field_info_t lookup_field_info(jfieldID field_id,
                               char *name_buf, size_t name_sz,
                               char *sig_buf, size_t sig_sz,
                               char *clazz_buf, size_t clazz_sz) {
  field_info_t info = {NULL, NULL, NULL};
  if (name_sz)  name_buf[0]  = '\0';
  if (sig_sz)   sig_buf[0]   = '\0';
  if (clazz_sz) clazz_buf[0] = '\0';
  if (field_id == NULL) return info;

  int found = 0;
  (void)pthread_rwlock_rdlock(&g_field_cache_lock);
  size_t start = field_cache_hash(field_id);
  for (size_t n = 0; n < METHOD_CACHE_SIZE; ++n) {
    size_t idx = (start + n) & (METHOD_CACHE_SIZE - 1u);
    const field_sig_entry_t *entry = &g_field_cache[idx];
    if (!entry->in_use) break;
    if (entry->field_id == field_id) {
      /* Copy under the read lock — see F10 note in lookup_method_info. */
      if (name_sz)  { strncpy(name_buf,  entry->name,      name_sz  - 1u); name_buf[name_sz   - 1u] = '\0'; info.name  = name_buf; }
      if (sig_sz)   { strncpy(sig_buf,   entry->signature, sig_sz   - 1u); sig_buf[sig_sz     - 1u] = '\0'; info.sig   = sig_buf; }
      if (clazz_sz) { strncpy(clazz_buf, entry->clazz,     clazz_sz - 1u); clazz_buf[clazz_sz - 1u] = '\0'; info.clazz = clazz_buf; }
      found = 1;
      break;
    }
  }
  (void)pthread_rwlock_unlock(&g_field_cache_lock);

  /* F20: consult ART for the field name ONLY on a cache miss (the old code
   * called art_get_field_name on every lookup and discarded it on a hit).
   * Copy straight into the caller's buffer, so there is also no shared
   * __thread art_name_buf whose "valid until the next same-thread call"
   * lifetime would need documenting. */
  if (!found && name_sz) {
    const char *art_name = art_get_field_name(field_id);
    if (art_name) {
      strncpy(name_buf, art_name, name_sz - 1u);
      name_buf[name_sz - 1u] = '\0';
      info.name = name_buf;
    }
  }
  return info;
}

int should_log_from_caller(JNIEnv *env, void *caller) {
  if (env == NULL || caller == NULL) return 0;
  /* Hard backstop: NEVER log a caller inside our own payload, regardless of what
   * got seeded. Identity-based (c_capture_self_range), so it holds even though
   * the injector stages us under a random name and vma-hides us — the staged
   * path matches the package filter and would otherwise be seeded as in-scope. */
  if (c_is_self_addr((uintptr_t)caller)) return 0;
  if (c_should_try_seed()) c_seed_exec_ranges_from_maps();
  if (!c_has_exec_ranges()) return 0;
  return c_is_in_exec_range((uintptr_t)caller);
}

/* ── Config query cache ───────────────────────────────────────────────────
 * Each JNI function name crosses cgo exactly once.  Subsequent hits do an
 * O(1) pointer comparison in a linear-probe hash table.
 *
 * Callers are expected to pass stable interned string literals (the JNI
 * function names baked into rodata).  `key` keeps the caller pointer purely
 * for that identity fast path and is never dereferenced; `owned` is a private
 * strdup'd copy and is the only pointer we ever strcmp.  This way a caller that
 * mistakenly passes a transient (stack/heap) pointer can at worst produce a
 * stale result — never a dangling-pointer dereference.
 * ──────────────────────────────────────────────────────────────────────── */
#define CONFIG_CACHE_SIZE 256
#define CONFIG_CACHE_MASK (CONFIG_CACHE_SIZE - 1)

typedef struct {
  const char *key;      /* caller's interned pointer — identity fast path, never dereferenced */
  char       *owned;    /* strdup'd copy of the name — the only pointer we dereference */
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
      /* F18: drop the lock for the (blocking) Go round-trip.  On re-acquire the
       * slot may have been populated by another thread that raced us:
       *   - SAME key (or a same-key strdup): the winner's result is identical to
       *     ours, so we adopt it.
       *   - DIFFERENT hash-colliding key: the winner's result answers a
       *     different name — we must NOT read it.  We return our own Go answer
       *     and `name` gets cached on a later probe into a free slot.
       * The identity re-check below distinguishes the two.  e->owned (strdup) is
       * intentionally never freed: a process-lifetime cache bounded by
       * CONFIG_CACHE_SIZE. */
      pthread_mutex_unlock(&g_cfg_cache_lock);
      int allowed = 1;
      if (config_function_blacklisted((char *)name)) allowed = 0;
      else if (!config_function_enabled((char *)name)) allowed = 0;
      pthread_mutex_lock(&g_cfg_cache_lock);
      /* Re-check the slot under the lock.  Write owned (and key) before result
       * so a concurrent reader that sees result != -1 also sees a fully
       * populated entry. */
      if (e->result == -1) {
        /* Slot still fresh — we win; populate it. */
        e->owned = strdup(name); e->key = name; e->result = allowed;
        *out = e->result;
      } else if (e->owned && strcmp(e->owned, name) == 0) {
        /* Same key won the race — adopt its identical result. */
        *out = e->result;
      } else {
        /* A different, hash-colliding key won this slot while we queried Go.
         * Its result answers a different name, so return our own Go answer. */
        *out = allowed;
      }
      pthread_mutex_unlock(&g_cfg_cache_lock);
      return 1;
    }
    if (e->key == name) { *out = e->result; pthread_mutex_unlock(&g_cfg_cache_lock); return 1; }
    if (e->owned && strcmp(e->owned, name) == 0) { e->key = name; *out = e->result; pthread_mutex_unlock(&g_cfg_cache_lock); return 1; }
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
  long page_size = jl_page_size();
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
