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

static __thread int g_in_hook = 0;
static __thread int g_jni_critical = 0;

int is_reentrant_call(void) { return g_in_hook; }
void set_reentrant_call(int val) { g_in_hook = val; }

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

int should_log_from_caller(JNIEnv *env, void *caller) {
  if (env == NULL || caller == NULL) return 0;
  if (!c_has_exec_ranges()) c_seed_exec_ranges_from_maps();
  return c_is_in_exec_range((uintptr_t)caller);
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
