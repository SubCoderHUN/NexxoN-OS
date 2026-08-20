/* ============================================================================
 * NexxoN OS - Minimal x86_64 ELF64 loader
 * ============================================================================ */
#include "elf64.h"
#include "string.h"
#include "debug.h"

/* ---- On-disk ELF64 structures (little-endian, packed) ------------------- */
typedef struct PACKED {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct PACKED {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct PACKED {
    int64_t  d_tag;
    uint64_t d_val;
} elf64_dyn_t;

typedef struct PACKED {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62
#define PT_LOAD   1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PF_X      1
#define PF_W      2
#define DT_NULL   0
#define DT_RELA   7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define R_X86_64_RELATIVE 8

static void fail(elf64_image_t *o, const char *why) {
    o->ok = false;
    strncpy(o->err, why, sizeof(o->err) - 1);
    o->err[sizeof(o->err) - 1] = 0;
}

static bool add_overflows(uint64_t a, uint64_t b) {
    return a + b < a;
}

void elf64_load(const uint8_t *data, uint32_t len,
                uint8_t *region, uint32_t region_sz, elf64_image_t *out) {
    memset(out, 0, sizeof(*out));

    if (len < sizeof(elf64_ehdr_t)) { fail(out, "file too small"); return; }
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) {
        fail(out, "not an ELF"); return;
    }
    if (eh->e_ident[4] != 2) { fail(out, "not ELF64"); return; }      /* CLASS64 */
    if (eh->e_ident[5] != 1) { fail(out, "not little-endian"); return; }
    if (eh->e_machine != EM_X86_64) { fail(out, "not x86_64"); return; }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        fail(out, "not EXEC/DYN"); return;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
        eh->e_phentsize < sizeof(elf64_phdr_t)) {
        fail(out, "no program headers"); return;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len) {
        fail(out, "phdrs out of range"); return;
    }

    bool is_pie = (eh->e_type == ET_DYN);
    uint64_t base = is_pie ? (uint64_t)(uintptr_t)region : 0;
    out->is_pie = is_pie;
    out->base   = base;

    /* First pass: find the vaddr span so we can bounds-check the region. */
    uint64_t lo = ~0ULL, hi = 0, dyn_off = 0, dyn_sz = 0, phdr_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) { dyn_off = ph->p_offset; dyn_sz = ph->p_filesz; }
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz < ph->p_filesz ||
            add_overflows(ph->p_offset, ph->p_filesz) ||
            ph->p_offset + ph->p_filesz > len) {
            fail(out, "segment past EOF");
            return;
        }
        if (add_overflows(ph->p_vaddr, ph->p_memsz)) {
            fail(out, "segment address overflow");
            return;
        }
        if (ph->p_vaddr < lo) lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi) hi = ph->p_vaddr + ph->p_memsz;
        uint64_t phbytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
        if (eh->e_phoff >= ph->p_offset &&
            eh->e_phoff + phbytes <= ph->p_offset + ph->p_filesz) {
            phdr_vaddr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
        }
    }
    if (hi == 0) { fail(out, "no LOAD segments"); return; }

    /* The shared-CR3 compatibility process must never be able to copy an ELF
     * segment over the kernel.  PIE is based inside the owned arena.  A fixed
     * ET_EXEC is accepted only if its requested virtual range is already that
     * same arena; ordinary 0x400000 executables wait for the private-CR3
     * milestone instead of being loaded unsafely. */
    uint64_t region_lo = (uint64_t)(uintptr_t)region;
    uint64_t region_hi = region_lo + region_sz;
    if (region_hi < region_lo) { fail(out, "region address overflow"); return; }
    if (is_pie) {
        if (hi > region_sz) { fail(out, "region too small"); return; }
    } else if (lo < region_lo || hi > region_hi) {
        fail(out, "fixed EXEC outside private region");
        return;
    }
    uint64_t span = hi - lo;

    /* Second pass: copy + zero-fill each LOAD segment. */
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint8_t *dst = (uint8_t *)(uintptr_t)(base + ph->p_vaddr);
        if (ph->p_filesz) memcpy(dst, data + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    /* A Linux kernel maps ET_DYN but does NOT apply its dynamic relocations.
     * A no-interpreter static PIE (musl/glibc) contains startup code that
     * derives the load bias from AT_PHDR and relocates itself before libc
     * initialization.  Applying RELATIVE entries here as v1 did makes that
     * startup add the bias a second time and corrupts every relocated pointer.
     * Validate the table location, then deliberately leave it untouched. */
    if (is_pie && dyn_off && dyn_sz) {
        if (add_overflows(dyn_off, dyn_sz) || dyn_off + dyn_sz > len) {
            fail(out, "dynamic table past EOF");
            return;
        }
        const elf64_dyn_t *dyn = (const elf64_dyn_t *)(data + dyn_off);
        uint64_t rela = 0, relasz = 0, relaent = sizeof(elf64_rela_t);
        for (uint32_t k = 0; k < dyn_sz / sizeof(elf64_dyn_t); k++) {
            if (dyn[k].d_tag == DT_NULL) break;
            switch (dyn[k].d_tag) {
                case DT_RELA:    rela    = dyn[k].d_val; break;
                case DT_RELASZ:  relasz  = dyn[k].d_val; break;
                case DT_RELAENT: relaent = dyn[k].d_val; break;
            }
        }
        if (rela && relasz && relaent >= sizeof(elf64_rela_t)) {
            if (rela < lo || add_overflows(rela, relasz) || rela + relasz > hi) {
                fail(out, "relocations outside image");
                return;
            }
            uint32_t count = (uint32_t)(relasz / relaent);
            debug_printf("[elf64] deferred %u PIE relocation(s) to static runtime\n",
                         count);
        }
    }

    if (eh->e_entry < lo || eh->e_entry >= hi) {
        fail(out, "entry outside LOAD segments");
        return;
    }
    out->entry = base + eh->e_entry;
    out->load_lo = base + lo;
    out->load_hi = base + hi;
    out->brk   = base + hi;
    out->phdr  = phdr_vaddr ? base + phdr_vaddr : 0;
    out->phent = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->ok    = true;
    debug_printf("[elf64] loaded %s: base=0x%x entry=0x%x span=%u bytes\n",
                 is_pie ? "PIE" : "EXEC", (uint32_t)base,
                 (uint32_t)out->entry, (uint32_t)span);
}

static int mapped_memset(vmm_pd_t *pd, uint64_t va, int value, uint64_t len) {
    while (len) {
        uintptr_t pa = vmm_resolve(pd, (uintptr_t)va);
        if (!pa) return -1;
        uint32_t chunk = VMM_PAGE_SIZE - (uint32_t)(va & (VMM_PAGE_SIZE - 1u));
        if ((uint64_t)chunk > len) chunk = (uint32_t)len;
        memset((void *)pa, value, chunk);
        va += chunk;
        len -= chunk;
    }
    return 0;
}

static int mapped_copy(vmm_pd_t *pd, uint64_t va,
                       const uint8_t *src, uint64_t len) {
    while (len) {
        uintptr_t pa = vmm_resolve(pd, (uintptr_t)va);
        if (!pa) return -1;
        uint32_t chunk = VMM_PAGE_SIZE - (uint32_t)(va & (VMM_PAGE_SIZE - 1u));
        if ((uint64_t)chunk > len) chunk = (uint32_t)len;
        memcpy((void *)pa, src, chunk);
        va += chunk;
        src += chunk;
        len -= chunk;
    }
    return 0;
}

static bool parse_interp_path(const uint8_t *data, uint32_t len,
                              const elf64_ehdr_t *eh, char *path, uint32_t cap) {
    if (!path || cap == 0) return false;
    path[0] = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_INTERP) continue;
        if (!ph->p_offset || !ph->p_filesz ||
            add_overflows(ph->p_offset, ph->p_filesz) ||
            ph->p_offset + ph->p_filesz > len ||
            ph->p_filesz >= cap)
            return false;
        memcpy(path, data + ph->p_offset, (size_t)ph->p_filesz);
        path[ph->p_filesz] = 0;
        return path[0] == '/';
    }
    return false;
}

static void note_deferred_rela(const uint8_t *data, uint32_t len,
                               uint64_t dyn_off, uint64_t dyn_sz,
                               uint64_t lo, uint64_t hi, const char *kind) {
    if (!dyn_off || !dyn_sz) return;
    if (add_overflows(dyn_off, dyn_sz) || dyn_off + dyn_sz > len) return;
    const elf64_dyn_t *dyn = (const elf64_dyn_t *)(data + dyn_off);
    uint64_t rela = 0, relasz = 0, relaent = sizeof(elf64_rela_t);
    for (uint32_t k = 0; k < dyn_sz / sizeof(elf64_dyn_t); k++) {
        if (dyn[k].d_tag == DT_NULL) break;
        switch (dyn[k].d_tag) {
            case DT_RELA:    rela    = dyn[k].d_val; break;
            case DT_RELASZ:  relasz  = dyn[k].d_val; break;
            case DT_RELAENT: relaent = dyn[k].d_val; break;
        }
    }
    if (!rela || !relasz || relaent < sizeof(elf64_rela_t)) return;
    if (rela < lo || add_overflows(rela, relasz) || rela + relasz > hi) return;
    debug_printf("[elf64] deferred %u %s relocation(s)\n",
                 (uint32_t)(relasz / relaent), kind);
}

static void map_segments(const uint8_t *data, uint32_t len,
                         const elf64_ehdr_t *eh, vmm_pd_t *pd, uint64_t base,
                         uint64_t lo, uint64_t hi, elf64_image_t *out) {
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t seg_lo = base + ph->p_vaddr;
        uint64_t seg_hi = seg_lo + ph->p_memsz;
        uint64_t page = seg_lo & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
        uint64_t last = (seg_hi + VMM_PAGE_SIZE - 1u) &
                        ~(uint64_t)(VMM_PAGE_SIZE - 1u);
        for (; page < last; page += VMM_PAGE_SIZE) {
            uintptr_t pa = 0, old_flags = 0;
            bool already_user =
                vmm_query_page(pd, (uintptr_t)page, &pa, &old_flags) == 0 &&
                (old_flags & VMM_FLAG_USER);
            if (!already_user) {
                pa = vmm_frame_alloc();
                if (!pa) { fail(out, "out of user frames"); return; }
            } else {
                pa &= ~(uintptr_t)(VMM_PAGE_SIZE - 1u);
            }
            bool writable = (ph->p_flags & PF_W) ||
                            (already_user && (old_flags & VMM_FLAG_RW));
            bool executable = (ph->p_flags & PF_X) ||
                              (already_user && !(old_flags & VMM_FLAG_NX));
            uintptr_t flags = VMM_FLAG_USER |
                              (writable ? VMM_FLAG_RW : 0) |
                              (executable ? 0 : VMM_FLAG_NX);
            if (vmm_map_page(pd, (uintptr_t)page, pa, flags) != 0) {
                if (!already_user) vmm_frame_free((uint32_t)pa);
                fail(out, "cannot map LOAD page");
                return;
            }
        }
        if (mapped_memset(pd, seg_lo, 0, ph->p_memsz) != 0 ||
            (ph->p_filesz &&
             mapped_copy(pd, seg_lo, data + ph->p_offset,
                         ph->p_filesz) != 0)) {
            fail(out, "cannot initialise LOAD segment");
            return;
        }
    }

    out->entry = base + eh->e_entry;
    out->base = base;
    out->load_lo = base + lo;
    out->load_hi = base + hi;
    out->brk = base + hi;
    out->ok = true;
}

static bool validate_headers(const uint8_t *data, uint32_t len,
                             const elf64_ehdr_t **eh_out) {
    if (len < sizeof(elf64_ehdr_t)) return false;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F'))
        return false;
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 ||
        eh->e_machine != EM_X86_64)
        return false;
    if (!eh->e_phoff || !eh->e_phnum ||
        eh->e_phentsize < sizeof(elf64_phdr_t) ||
        add_overflows(eh->e_phoff,
                      (uint64_t)eh->e_phnum * eh->e_phentsize) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len)
        return false;
    *eh_out = eh;
    return true;
}

static void map_elf_image(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                            uint64_t base, bool require_pie, bool allow_exec,
                            uint64_t user_min, uint64_t user_max,
                            bool require_phdr, elf64_image_t *out,
                            const char *kind) {
    memset(out, 0, sizeof(*out));
    if (!pd) { fail(out, "no process address space"); return; }

    const elf64_ehdr_t *eh;
    if (!validate_headers(data, len, &eh)) {
        fail(out, "bad ELF header"); return;
    }
    bool pie = eh->e_type == ET_DYN;
    bool exec = eh->e_type == ET_EXEC;
    if (require_pie && !pie) { fail(out, "expected ET_DYN"); return; }
    if (!pie && !exec) { fail(out, "unsupported ELF64 image"); return; }
    if (!pie && !allow_exec) { fail(out, "expected ET_DYN"); return; }

    uint64_t lo = ~0ULL, hi = 0, phdr_vaddr = 0, dyn_off = 0, dyn_sz = 0;
    bool entry_exec = false;
    for (int i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) {
            dyn_off = ph->p_offset;
            dyn_sz = ph->p_filesz;
        }
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz < ph->p_filesz ||
            add_overflows(ph->p_offset, ph->p_filesz) ||
            ph->p_offset + ph->p_filesz > len ||
            add_overflows(ph->p_vaddr, ph->p_memsz) ||
            ((ph->p_offset ^ ph->p_vaddr) & (VMM_PAGE_SIZE - 1u))) {
            fail(out, "invalid LOAD segment"); return;
        }
        if ((ph->p_flags & PF_X) && eh->e_entry >= ph->p_vaddr &&
            eh->e_entry < ph->p_vaddr + ph->p_memsz)
            entry_exec = true;
        if (ph->p_vaddr < lo) lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi) hi = ph->p_vaddr + ph->p_memsz;
        uint64_t phbytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
        if (eh->e_phoff >= ph->p_offset &&
            eh->e_phoff + phbytes <= ph->p_offset + ph->p_filesz)
            phdr_vaddr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
    }
    if (hi == 0 || add_overflows(base, hi) || base + lo < user_min ||
        base + hi > user_max || base + hi < base + lo) {
        fail(out, "ELF outside private user window"); return;
    }
    if (eh->e_entry < lo || eh->e_entry >= hi || !entry_exec) {
        fail(out, "entry outside executable LOAD"); return;
    }
    if (require_phdr && !phdr_vaddr) {
        fail(out, "program headers are not mapped"); return;
    }

    map_segments(data, len, eh, pd, base, lo, hi, out);
    if (!out->ok) return;

    if (pie && dyn_off && dyn_sz)
        note_deferred_rela(data, len, dyn_off, dyn_sz, lo, hi, kind);

    out->phdr = phdr_vaddr ? base + phdr_vaddr : 0;
    out->phent = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->is_pie = pie;
    debug_printf("[elf64] mapped private %s: base=%p entry=%p span=%u bytes\n",
                 kind, (void *)(uintptr_t)base,
                 (void *)(uintptr_t)out->entry, (uint32_t)(hi - lo));
}

void elf64_map_exec(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                    uint64_t pie_base, uint64_t user_min, uint64_t user_max,
                    elf64_mapped_t *out) {
    memset(out, 0, sizeof(*out));
    const elf64_ehdr_t *eh;
    if (!validate_headers(data, len, &eh)) {
        fail(&out->exec, "bad ELF header"); return;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        fail(&out->exec, "unsupported ELF64 image"); return;
    }

    out->has_interp = parse_interp_path(data, len, eh, out->interp_path,
                                        sizeof(out->interp_path));
    uint64_t base = eh->e_type == ET_DYN ? pie_base : 0;
    map_elf_image(data, len, pd, base, eh->e_type == ET_DYN, true,
                  user_min, user_max, true, &out->exec,
                  out->has_interp ? "dynamic-PIE" : (eh->e_type == ET_DYN
                                                     ? "PIE" : "EXEC"));
}

void elf64_map_so(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                  uint64_t load_base, uint64_t user_min, uint64_t user_max,
                  elf64_image_t *out) {
    map_elf_image(data, len, pd, load_base, true, false,
                  user_min, user_max, false, out, "shared-object");
}

void elf64_map_load(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                    uint64_t pie_base, uint64_t user_min, uint64_t user_max,
                    elf64_image_t *out) {
    elf64_mapped_t mapped;
    elf64_map_exec(data, len, pd, pie_base, user_min, user_max, &mapped);
    *out = mapped.exec;
}
