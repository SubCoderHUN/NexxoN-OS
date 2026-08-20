/* ============================================================================
 * NexxoN OS - Minimal PE32/PE32+ loader
 * ============================================================================ */
#include "pe_loader.h"
#include "string.h"
#include "debug.h"

typedef struct PACKED {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} pe_dos_t;

typedef struct PACKED {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} pe_coff_t;

typedef struct PACKED {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
} pe_opt64_t;

typedef struct PACKED {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
} pe_opt32_t;

typedef struct PACKED {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} pe_section_t;

#define PE_MAGIC_DOS 0x5A4D
#define PE_MAGIC_NT  0x00004550u
#define PE_OPT_PE32  0x10B
#define PE_OPT_PE32P 0x20B
#define PE_MACHINE_I386  0x014C
#define PE_MACHINE_AMD64 0x8664

static void pe_fail(pe_image_t *o, const char *why) {
    if (!o) return;
    o->ok = false;
    strncpy(o->err, why, sizeof(o->err) - 1);
    o->err[sizeof(o->err) - 1] = 0;
}

static bool pe_locate(const uint8_t *data, uint32_t len,
                      const pe_coff_t **coff,
                      const void **opt, uint16_t *opt_magic,
                      const pe_section_t **secs, uint32_t *nsec,
                      pe_image_t *out) {
    if (!data || len < sizeof(pe_dos_t)) {
        pe_fail(out, "file too small"); return false;
    }
    const pe_dos_t *dos = (const pe_dos_t *)data;
    if (dos->e_magic != PE_MAGIC_DOS) {
        pe_fail(out, "missing MZ"); return false;
    }
    if (dos->e_lfanew == 0 || dos->e_lfanew + 24 > len) {
        pe_fail(out, "bad PE offset"); return false;
    }
    const uint32_t *sig = (const uint32_t *)(data + dos->e_lfanew);
    if (*sig != PE_MAGIC_NT) {
        pe_fail(out, "missing PE"); return false;
    }
    *coff = (const pe_coff_t *)(data + dos->e_lfanew + 4);
    if (dos->e_lfanew + 4 + sizeof(pe_coff_t) > len) {
        pe_fail(out, "truncated COFF"); return false;
    }
    if ((*coff)->SizeOfOptionalHeader < 2) {
        pe_fail(out, "no optional header"); return false;
    }
    *opt = (const void *)(data + dos->e_lfanew + 4 + sizeof(pe_coff_t));
    *opt_magic = *(const uint16_t *)*opt;
    if (*opt_magic != PE_OPT_PE32 && *opt_magic != PE_OPT_PE32P) {
        pe_fail(out, "bad optional magic"); return false;
    }
    uint32_t opt_sz = (*coff)->SizeOfOptionalHeader;
    uint32_t sec_off = dos->e_lfanew + 4 + sizeof(pe_coff_t) + opt_sz;
    *nsec = (*coff)->NumberOfSections;
    if (sec_off > len) {
        pe_fail(out, "section table OOB"); return false;
    }
    if (*nsec > 0 &&
        sec_off + (*nsec) * sizeof(pe_section_t) > len) {
        pe_fail(out, "section table OOB"); return false;
    }
    *secs = (const pe_section_t *)(data + sec_off);
    return true;
}

bool pe_parse(const uint8_t *data, uint32_t len, pe_image_t *out) {
    memset(out, 0, sizeof(*out));
    const pe_coff_t *coff;
    const void *opt;
    uint16_t opt_magic;
    const pe_section_t *secs;
    uint32_t nsec;
    if (!pe_locate(data, len, &coff, &opt, &opt_magic, &secs, &nsec, out))
        return false;

    out->machine = coff->Machine;
    if (coff->Machine != PE_MACHINE_I386 &&
        coff->Machine != PE_MACHINE_AMD64) {
        pe_fail(out, "unsupported machine"); return false;
    }

    if (opt_magic == PE_OPT_PE32P) {
        const pe_opt64_t *o64 = (const pe_opt64_t *)opt;
        out->entry_rva = o64->AddressOfEntryPoint;
        out->image_base = o64->ImageBase;
        out->size_of_image = o64->SizeOfImage;
        if (coff->SizeOfOptionalHeader >= 120) {
            const uint32_t *dirs =
                (const uint32_t *)((const uint8_t *)opt + 112);
            out->import_rva = dirs[2];
            out->import_sz  = dirs[3];
        }
    } else {
        const pe_opt32_t *o32 = (const pe_opt32_t *)opt;
        out->entry_rva = o32->AddressOfEntryPoint;
        out->image_base = o32->ImageBase;
        out->size_of_image = o32->SizeOfImage;
        if (coff->SizeOfOptionalHeader >= 104) {
            const uint32_t *dirs =
                (const uint32_t *)((const uint8_t *)opt + 96);
            out->import_rva = dirs[2];
            out->import_sz  = dirs[3];
        }
    }
    out->section_count = nsec;
    out->ok = true;
    return true;
}

static bool pe_copy_sections(const uint8_t *data, uint32_t len,
                             const pe_section_t *secs, uint32_t nsec,
                             uint8_t *dest, uint32_t dest_cap,
                             uint32_t size_of_image, pe_image_t *out) {
    if (!dest || size_of_image == 0 || size_of_image > dest_cap) {
        pe_fail(out, "dest too small"); return false;
    }
    memset(dest, 0, size_of_image);
    for (uint32_t i = 0; i < nsec; i++) {
        const pe_section_t *s = &secs[i];
        if (s->VirtualAddress >= size_of_image) continue;
        uint32_t vsize = s->VirtualSize ? s->VirtualSize : s->SizeOfRawData;
        if (s->VirtualAddress + vsize > size_of_image) {
            pe_fail(out, "section VA OOB"); return false;
        }
        if (s->SizeOfRawData == 0) continue;
        if (s->PointerToRawData + s->SizeOfRawData > len) {
            pe_fail(out, "section raw OOB"); return false;
        }
        uint32_t copy = s->SizeOfRawData;
        if (copy > vsize) copy = vsize;
        memcpy(dest + s->VirtualAddress,
               data + s->PointerToRawData, copy);
    }
    return true;
}

bool pe_load_flat(const uint8_t *data, uint32_t len,
                  uint8_t *dest, uint32_t dest_cap, pe_image_t *out) {
    if (!pe_parse(data, len, out)) return false;
    const pe_coff_t *coff;
    const void *opt;
    uint16_t opt_magic;
    const pe_section_t *secs;
    uint32_t nsec;
    if (!pe_locate(data, len, &coff, &opt, &opt_magic, &secs, &nsec, out))
        return false;
    if (!pe_copy_sections(data, len, secs, nsec, dest, dest_cap,
                          out->size_of_image, out))
        return false;
    out->ok = true;
    return true;
}

static int map_user_page_range(vmm_pd_t *pd, uint64_t addr, uint64_t len,
                               uintptr_t flags) {
    uint64_t page = addr & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    uint64_t end = (addr + len + VMM_PAGE_SIZE - 1u) &
                   ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    for (; page < end; page += VMM_PAGE_SIZE) {
        uintptr_t old_flags = 0;
        if (vmm_query_page(pd, (uintptr_t)page, NULL, &old_flags) == 0 &&
            (old_flags & VMM_FLAG_USER))
            continue;
        uint32_t pa = vmm_frame_alloc();
        if (!pa) return -1;
        if (vmm_map_page(pd, (uintptr_t)page, pa,
                         flags | VMM_FLAG_USER) != 0) {
            vmm_frame_free(pa);
            return -1;
        }
    }
    return 0;
}

bool pe_map_user(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                 uint64_t base, uint64_t user_min, uint64_t user_max,
                 pe_image_t *out) {
    if (!pe_parse(data, len, out)) return false;
    if (base < user_min || base + out->size_of_image > user_max) {
        pe_fail(out, "VA window OOB"); return false;
    }
    if (map_user_page_range(pd, base, out->size_of_image,
                            VMM_FLAG_RW | VMM_FLAG_NX) != 0) {
        pe_fail(out, "map failed"); return false;
    }
    uint8_t scratch[65536];
    const pe_coff_t *coff;
    const void *opt;
    uint16_t opt_magic;
    const pe_section_t *secs;
    uint32_t nsec;
    if (!pe_locate(data, len, &coff, &opt, &opt_magic, &secs, &nsec, out))
        return false;

    for (uint32_t i = 0; i < nsec; i++) {
        const pe_section_t *s = &secs[i];
        uint32_t vsize = s->VirtualSize ? s->VirtualSize : s->SizeOfRawData;
        if (vsize == 0) continue;
        uint64_t dst = base + s->VirtualAddress;
        if (dst + vsize > user_max) {
            pe_fail(out, "section map OOB"); return false;
        }
        uint64_t off = 0;
        while (off < vsize) {
            uint32_t chunk = (uint32_t)((vsize - off) > sizeof(scratch)
                                     ? sizeof(scratch) : (vsize - off));
            memset(scratch, 0, chunk);
            if (s->SizeOfRawData > off) {
                uint32_t raw_left = s->SizeOfRawData - (uint32_t)off;
                uint32_t copy = chunk < raw_left ? chunk : raw_left;
                if (s->PointerToRawData + off + copy > len) {
                    pe_fail(out, "section read OOB"); return false;
                }
                memcpy(scratch, data + s->PointerToRawData + off, copy);
            }
            uintptr_t pa = vmm_resolve(pd, (uintptr_t)(dst + off));
            if (!pa) { pe_fail(out, "resolve failed"); return false; }
            uint32_t page_off = (uint32_t)((dst + off) & (VMM_PAGE_SIZE - 1u));
            uint32_t page_left = VMM_PAGE_SIZE - page_off;
            uint32_t wr = chunk < page_left ? chunk : page_left;
            memcpy((void *)(pa + page_off), scratch, wr);
            off += wr;
            if (wr < chunk) {
                pa = vmm_resolve(pd, (uintptr_t)(dst + off));
                if (!pa) { pe_fail(out, "resolve failed"); return false; }
                memcpy((void *)pa, scratch + wr, chunk - wr);
                off += chunk - wr;
            }
        }
        if (s->Characteristics & 0x20000000u) { /* IMAGE_SCN_MEM_EXECUTE */
            uint64_t sec_page = dst & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
            uint64_t sec_end = (dst + vsize + VMM_PAGE_SIZE - 1u) &
                               ~(uint64_t)(VMM_PAGE_SIZE - 1u);
            for (uint64_t p = sec_page; p < sec_end; p += VMM_PAGE_SIZE)
                (void)vmm_protect_page(pd, (uintptr_t)p, VMM_FLAG_USER);
        }
    }
    out->ok = true;
    debug_printf("[pe] mapped %u sections at 0x%x entry_rva=0x%x machine=0x%x\n",
                 out->section_count, (uint32_t)base, out->entry_rva,
                 out->machine);
    return true;
}
