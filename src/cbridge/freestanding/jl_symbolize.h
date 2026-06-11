/*
 * jl_symbolize.h — freestanding dladdr replacement (no libc / no libdl).
 *
 * The hot path symbolizes each JNI call's caller address ("libfoo.so!0xoff")
 * via dladdr. dladdr is GOT-routed (a co-injected libc logger would see it per
 * event), so we replace it with a lookup against a cached module table built
 * from /proc/self/maps.
 *
 * jl_dladdr fills dli_fname (into a per-thread buffer, so the result survives a
 * concurrent table rebuild) and dli_fbase; it leaves dli_sname/dli_saddr NULL.
 * The bridge's address_of_r then renders "basename!0x<addr-fbase>", which is
 * exactly bionic's output for the common case (caller in a stripped app lib,
 * where bionic also yields no dli_sname). It diverges only when bionic WOULD
 * resolve an exported symbol — accepted, since callers are app/packer code.
 *
 * The table is built lazily and refreshed on dlopen (c_reset_seed_attempted
 * calls jl_symbolize_refresh). Reading /proc/self/maps is a kernel snapshot, so
 * the walk cannot crash on a racing dlopen (danger #7) — it may at worst see a
 * momentarily-inconsistent module set, which only affects best-effort display.
 *
 * Self note: our own staged .so is captured early (constructor, pre-vma_hide)
 * while it is still in /proc/self/maps; later refreshes omit it (hidden), which
 * is fine — callers are never inside our payload.
 */
#ifndef JL_SYMBOLIZE_H
#define JL_SYMBOLIZE_H

#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <dlfcn.h> /* Dl_info */
#include "jl_io.h"
#include "jl_lock.h"
#include "jl_str.h"

#include <elf.h>

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

/* JL_MOD_MAX sizes the symbolizer's module table. A heavily-split, SDK-laden
 * app (e.g. a packed Play-Store title) can map 800+ distinct files; at 384 the
 * walk used to stop partway up the address space and every higher-addressed
 * library symbolized to a bare address. 1024 covers that with headroom (the
 * table is process-lifetime static BSS in the payload — a few hundred KB). */
#define JL_MOD_MAX  1024
#define JL_MOD_PATH 256

typedef struct {
    uintptr_t        base, end;
    /* fend = end of the file-backed extent. Equals `end` for an ordinary
     * library; stays below it when a contiguous executable ANONYMOUS region was
     * absorbed into this module (see jl_modtable_build_locked). An address in
     * [fend, end) is in the library's in-memory-only code (a packer's decrypted
     * .bss / JIT arena), not its on-disk image — jl_dladdr flags it so the
     * renderer can mark it. */
    uintptr_t        fend;
    char             path[JL_MOD_PATH];
    /* .dynsym/.dynstr cache for dli_sname, parsed lazily on first lookup in this
     * module (and re-parsed after a table rebuild). NULL sym => unparseable or
     * not yet tried. Pointers index into the module's own mapped memory. */
    const Elf64_Sym *sym;
    const char      *str;
    uint32_t         nsym;
    int              syms_tried;
} jl_mod_t;

static jl_mod_t   jl_mods[JL_MOD_MAX];
static int        jl_mod_count = 0;
static int        jl_mod_built = 0;
static jl_mutex_t jl_mod_lock = JL_MUTEX_INIT;

/* Locate the pathname (6th field) of a /proc/self/maps line, or NULL. */
static inline const char *jl_maps_path(const char *line) {
    int spaces = 0;
    for (const char *p = line; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            spaces++;
            while (*(p + 1) == ' ' || *(p + 1) == '\t') p++;
            if (spaces == 5) return p + 1;
        }
    }
    return (const char *)0;
}

/* caller holds jl_mod_lock */
static void jl_modtable_build_locked(void) {
    jl_mod_count = 0;
    jl_linereader lr;
    if (jl_lr_open(&lr, "/proc/self/maps", O_RDONLY | O_CLOEXEC) < 0) { jl_mod_built = 1; return; }
    char line[1024];
    while (jl_lr_next(&lr, line, sizeof line)) {
        uintptr_t lo, hi;
        char perms[5];
        if (!jl_parse_maps_head(line, &lo, &hi, perms)) continue;
        const char *path = jl_maps_path(line);
        if (!path || path[0] != '/') {
            /* Non-file-backed mapping. Packers (e.g. 360 Jiagu) decrypt the app's
             * real native code into the EXECUTABLE .bss tail of their loader lib —
             * an anonymous "[anon:.bss]" region the kernel maps because the lib's
             * PT_LOAD p_memsz exceeds p_filesz, contiguous with the lib's
             * file-backed mappings but with no '/' path. Dropping every non-'/'
             * line made that code (often the MAJORITY of an app's JNI calls)
             * symbolize to a bare ASLR'd address. Absorb a STRICTLY-contiguous
             * (lo == prev.end) EXECUTABLE anonymous region into the preceding
             * file-backed module so it resolves to "<lib>+0x<off>". Strict
             * contiguity means a guard page ("---p") between the lib and a
             * detached JIT/arena breaks the chain — a genuinely standalone
             * anonymous arena is never mis-attributed. `end` grows; `fend` stays
             * at the file-backed extent so the in-memory region can be flagged. */
            if (perms[2] == 'x' && jl_mod_count > 0 &&
                lo == jl_mods[jl_mod_count - 1].end) {
                jl_mods[jl_mod_count - 1].end = hi;
            }
            continue;
        }
        /* strip " (deleted)" suffix the kernel adds for unlinked files */
        char *del = jl_strstr(path, " (deleted)");
        if (del) *del = '\0';
        /* merge consecutive mappings of the same file into one [base,end] */
        if (jl_mod_count > 0 && jl_strcmp(jl_mods[jl_mod_count - 1].path, path) == 0) {
            if (hi > jl_mods[jl_mod_count - 1].end) {
                jl_mods[jl_mod_count - 1].end = hi;
                jl_mods[jl_mod_count - 1].fend = hi;
            }
            continue;
        }
        if (jl_mod_count >= JL_MOD_MAX) break;
        jl_mods[jl_mod_count].base = lo;
        jl_mods[jl_mod_count].end = hi;
        jl_mods[jl_mod_count].fend = hi;
        jl_strncpy(jl_mods[jl_mod_count].path, path, JL_MOD_PATH - 1);
        jl_mods[jl_mod_count].path[JL_MOD_PATH - 1] = '\0';
        jl_mod_count++;
    }
    jl_lr_close(&lr);
    jl_mod_built = 1;
}

/* Force a table rebuild on the next lookup (called from the dlopen path). */
static inline void jl_symbolize_refresh(void) {
    jl_mutex_lock(&jl_mod_lock);
    jl_mod_built = 0;
    jl_mutex_unlock(&jl_mod_lock);
}

/* Parse the ELF mapped at [base,end): locate .dynsym/.dynstr and the symbol
 * count. Returns 1 on success. Fully bounds-checked — any anomaly returns 0 and
 * the caller falls back to the offset-only "lib!0xoff" form. d_ptr values are
 * relocated when already absolute (>= base) else biased by base (the link-time
 * relative offsets the bridge sees are always small, well below base). */
static int jl_elf_dynsyms(uintptr_t base, uintptr_t end,
                          const Elf64_Sym **out_sym, const char **out_str,
                          uint32_t *out_n) {
    *out_sym = (const Elf64_Sym *)0; *out_str = (const char *)0; *out_n = 0;
    if (base + sizeof(Elf64_Ehdr) > end) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return 0;
    if (eh->e_phoff == 0 ||
        base + eh->e_phoff + (uintptr_t)eh->e_phnum * sizeof(Elf64_Phdr) > end)
        return 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr); break; }
    if (!dyn || (uintptr_t)dyn < base || (uintptr_t)dyn >= end) return 0;

    const Elf64_Sym *symtab = (const Elf64_Sym *)0;
    const char *strtab = (const char *)0;
    const uint32_t *hash = (const uint32_t *)0, *gnu = (const uint32_t *)0;
    for (const Elf64_Dyn *d = dyn; (uintptr_t)(d + 1) <= end && d->d_tag != DT_NULL; d++) {
        uintptr_t v = (uintptr_t)d->d_un.d_ptr;
        uintptr_t a = (v >= base) ? v : base + v;
        if (d->d_tag == DT_SYMTAB)        symtab = (const Elf64_Sym *)a;
        else if (d->d_tag == DT_STRTAB)   strtab = (const char *)a;
        else if (d->d_tag == DT_HASH)     hash = (const uint32_t *)a;
        else if (d->d_tag == DT_GNU_HASH) gnu  = (const uint32_t *)a;
    }
    if (!symtab || !strtab) return 0;
    if ((uintptr_t)symtab < base || (uintptr_t)symtab >= end ||
        (uintptr_t)strtab < base || (uintptr_t)strtab >= end) return 0;

    uint32_t nsym = 0;
    if (hash && (uintptr_t)(hash + 2) <= end) {
        nsym = hash[1]; /* nchain == number of dynamic symbols */
    } else if (gnu && (uintptr_t)(gnu + 4) <= end) {
        uint32_t nbuckets = gnu[0], symoffset = gnu[1], bloom_size = gnu[2];
        const uint32_t *buckets = gnu + 4 + bloom_size * 2; /* 64-bit bloom words */
        const uint32_t *chain = buckets + nbuckets;
        if ((uintptr_t)chain > end) return 0;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++) {
            if ((uintptr_t)(buckets + i + 1) > end) return 0;
            if (buckets[i] > last) last = buckets[i];
        }
        if (last < symoffset) nsym = symoffset;
        else {
            for (;;) {
                if ((uintptr_t)(chain + (last - symoffset) + 1) > end) return 0;
                uint32_t h = chain[last - symoffset];
                last++;
                if (h & 1u) break;
            }
            nsym = last;
        }
    } else if ((uintptr_t)strtab > (uintptr_t)symtab) {
        nsym = (uint32_t)(((uintptr_t)strtab - (uintptr_t)symtab) / sizeof(Elf64_Sym));
    }
    if (nsym == 0 || nsym > 500000) return 0;
    if ((uintptr_t)symtab + (uintptr_t)nsym * sizeof(Elf64_Sym) > end)
        nsym = (uint32_t)((end - (uintptr_t)symtab) / sizeof(Elf64_Sym));
    if (nsym == 0) return 0;
    *out_sym = symtab; *out_str = strtab; *out_n = nsym;
    return 1;
}

/* dladdr replacement. Returns non-zero (like dladdr) when addr is in a known
 * file mapping. dli_fname points into a per-thread buffer valid until this
 * thread's next jl_dladdr call; dli_sname (when resolved) points into the
 * module's mapped .dynstr. */
static __thread char jl_dladdr_name[JL_MOD_PATH];

/* Set by the last jl_dladdr that returned non-zero: 1 when the address fell in
 * the module's in-memory-only region (a contiguous executable anonymous mapping
 * absorbed past the file-backed extent — a packer's decrypted .bss), else 0.
 * The renderer (address_of_r) reads it to mark such addresses. Per-thread, and
 * only meaningful immediately after a successful jl_dladdr on the same thread. */
static __thread int jl_dladdr_synth;

static int jl_dladdr(const void *addr, Dl_info *info) {
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = 0, end = 0, fend = 0;
    const Elf64_Sym *sym = (const Elf64_Sym *)0;
    const char *str = (const char *)0;
    uint32_t nsym = 0;
    int found = 0;

    jl_mutex_lock(&jl_mod_lock);
    if (!jl_mod_built) jl_modtable_build_locked();
    for (int i = 0; i < jl_mod_count; i++) {
        if (a >= jl_mods[i].base && a < jl_mods[i].end) {
            if (!jl_mods[i].syms_tried) {
                jl_mods[i].syms_tried = 1;
                if (!jl_elf_dynsyms(jl_mods[i].base, jl_mods[i].end,
                                    &jl_mods[i].sym, &jl_mods[i].str, &jl_mods[i].nsym))
                    jl_mods[i].sym = (const Elf64_Sym *)0;
            }
            base = jl_mods[i].base; end = jl_mods[i].end; fend = jl_mods[i].fend;
            sym = jl_mods[i].sym; str = jl_mods[i].str; nsym = jl_mods[i].nsym;
            jl_strncpy(jl_dladdr_name, jl_mods[i].path, JL_MOD_PATH - 1);
            jl_dladdr_name[JL_MOD_PATH - 1] = '\0';
            found = 1;
            break;
        }
    }
    jl_mutex_unlock(&jl_mod_lock);
    if (!found) return 0;
    jl_dladdr_synth = (a >= fend) ? 1 : 0;

    info->dli_fbase = (void *)base;
    info->dli_fname = jl_dladdr_name;
    info->dli_sname = (const char *)0;
    info->dli_saddr = (void *)0;

    /* Nearest covering symbol (matches bionic: addr must fall within
     * [st_value, st_value+st_size), or == st_value for size-0 symbols). The
     * scan runs outside the lock on stable copies; sym/str index the module's
     * own mapped memory. */
    if (sym && str && nsym) {
        uintptr_t rel = a - base;
        for (uint32_t i = 0; i < nsym; i++) {
            const Elf64_Sym *s = &sym[i];
            if (s->st_shndx == SHN_UNDEF || s->st_value == 0) continue;
            unsigned t = ELF64_ST_TYPE(s->st_info);
            if (t != STT_FUNC && t != STT_OBJECT) continue;
            if (rel < s->st_value) continue;
            uintptr_t sz = s->st_size;
            if (sz ? (rel < s->st_value + sz) : (rel == s->st_value)) {
                const char *nm = str + s->st_name;
                if ((uintptr_t)nm < end && nm[0]) {
                    info->dli_sname = nm;
                    info->dli_saddr = (void *)(base + s->st_value);
                }
                break;
            }
        }
    }
    return 1;
}

#endif /* JL_SYMBOLIZE_H */
