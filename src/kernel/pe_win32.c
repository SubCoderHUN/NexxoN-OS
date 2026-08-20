/* ============================================================================
 * NexxoN OS - Win32 API stub layer for PE/Wine
 * ============================================================================ */
#include "pe_win32.h"
#include "string.h"
#include "debug.h"

typedef struct PACKED {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} pe_import_desc_t;

#define PE_STUB_RVA  0x11F0u   /* tail of .text — already RX via pe_map_user */

static int map_exec_pages(vmm_pd_t *pd, uint64_t addr, uint64_t len);

static int write_user(vmm_pd_t *pd, uint64_t dst, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uintptr_t pa = vmm_resolve(pd, (uintptr_t)dst);
        if (!pa) return -1;
        uint32_t chunk = VMM_PAGE_SIZE - (uint32_t)(dst & (VMM_PAGE_SIZE - 1u));
        if (chunk > n) chunk = n;
        memcpy((void *)(pa + (dst & (VMM_PAGE_SIZE - 1u))), s, chunk);
        dst += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

static int read_user(vmm_pd_t *pd, uint64_t src, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        uintptr_t pa = vmm_resolve(pd, (uintptr_t)src);
        if (!pa) return -1;
        uint32_t chunk = VMM_PAGE_SIZE - (uint32_t)(src & (VMM_PAGE_SIZE - 1u));
        if (chunk > n) chunk = n;
        memcpy(d, (void *)(pa + (src & (VMM_PAGE_SIZE - 1u))), chunk);
        src += chunk;
        d += chunk;
        n -= chunk;
    }
    return 0;
}

uint64_t pe_win32_map_stubs(vmm_pd_t *pd, uint64_t image_base,
                            uint32_t size_of_image) {
    if (PE_STUB_RVA >= size_of_image) return 0;
    uint64_t stub_va = image_base + PE_STUB_RVA;
    uint64_t page = stub_va & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    uintptr_t old_flags = 0;
    if (vmm_query_page(pd, (uintptr_t)page, NULL, &old_flags) != 0 ||
        !(old_flags & VMM_FLAG_USER)) {
        if (vmm_protect_page(pd, (uintptr_t)page, VMM_FLAG_USER) != 0)
            return 0;
    }

    static const uint8_t stub[] = {
        0x48, 0xC7, 0xC0, 0xCC, 0x01, 0x00, 0x00,   /* mov rax, 460 */
        0x48, 0x89, 0xCF,                             /* mov rdi, rcx */
        0x0F, 0x05,                                   /* syscall      */
        0xF4,                                         /* hlt          */
    };
    if (write_user(pd, stub_va, stub, sizeof(stub)) != 0)
        return 0;
    debug_printf("[pe/win32] ExitProcess stub at 0x%x\n", (uint32_t)stub_va);
    return stub_va;
}

static uint32_t pe_rva_to_off(const uint8_t *data, uint32_t len, uint32_t rva) {
    uint32_t lfanew = *(const uint32_t *)(data + 0x3C);
    if (lfanew + 4 + 20 + 2 > len) return 0;
    const uint16_t nsec = *(const uint16_t *)(data + lfanew + 4 + 2);
    const uint16_t opt_sz = *(const uint16_t *)(data + lfanew + 4 + 16);
    uint32_t sec_off = lfanew + 4 + 20 + opt_sz;
    if (sec_off + nsec * 40u > len) return 0;
    for (uint16_t i = 0; i < nsec; i++) {
        const uint8_t *s = data + sec_off + i * 40u;
        uint32_t va = *(const uint32_t *)(s + 12);
        uint32_t raw = *(const uint32_t *)(s + 20);
        uint32_t raw_sz = *(const uint32_t *)(s + 16);
        uint32_t virt_sz = *(const uint32_t *)(s + 8);
        if (!virt_sz) virt_sz = raw_sz;
        if (rva >= va && rva < va + virt_sz)
            return raw + (rva - va);
    }
    return 0;
}

bool pe_win32_bind_imports(const uint8_t *data, uint32_t len, vmm_pd_t *pd,
                           uint64_t image_base, uint64_t stub_base,
                           pe_image_t *img) {
    if (!img) return false;
    uint32_t imp_rva = img->import_rva;
    uint32_t imp_sz  = img->import_sz;
    if (!imp_rva || !imp_sz)
        return true;

    uint32_t imp_off = pe_rva_to_off(data, len, imp_rva);
    if (!imp_off) {
        debug_printf("[pe/win32] bind: import RVA 0x%x not in sections\n",
                     imp_rva);
        return false;
    }

    uint32_t off = 0;
    while (off + sizeof(pe_import_desc_t) <= imp_sz &&
           imp_off + off + sizeof(pe_import_desc_t) <= len) {
        const pe_import_desc_t *d =
            (const pe_import_desc_t *)(data + imp_off + off);
        if (!d->Name && !d->FirstThunk) break;

        if (d->FirstThunk) {
            uint32_t iat_rva = d->FirstThunk;
            uint32_t iat_off = pe_rva_to_off(data, len, iat_rva);
            if (!iat_off) return false;
            for (uint32_t i = 0; i < 64; i++) {
                if (iat_off + i * 8 + 8 > len) break;
                uint64_t entry = *(const uint64_t *)(data + iat_off + i * 8);
                if (!entry) break;
                uint64_t slot_va = image_base + iat_rva + i * 8;
                if (write_user(pd, slot_va, &stub_base, 8) != 0)
                    return false;
                uint64_t check = 0;
                if (read_user(pd, slot_va, &check, 8) != 0 || check != stub_base)
                    return false;
                debug_printf("[pe/win32] bound IAT 0x%x -> 0x%x\n",
                             (uint32_t)slot_va, (uint32_t)check);
            }
        }
        off += sizeof(pe_import_desc_t);
    }
    return true;
}

bool pe_win32_inject_entry(vmm_pd_t *pd, uint64_t image_base,
                           uint32_t entry_rva, uint64_t exit_stub) {
    (void)exit_stub;
    uint8_t code[32];
    uint32_t n = 0;
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xEC; code[n++] = 0x28;
    code[n++] = 0x48; code[n++] = 0x31; code[n++] = 0xFF;             /* xor rdi, rdi */
    code[n++] = 0x48; code[n++] = 0xC7; code[n++] = 0xC0;             /* mov rax, 460 */
    code[n++] = 0xCC; code[n++] = 0x01; code[n++] = 0x00; code[n++] = 0x00;
    code[n++] = 0x0F; code[n++] = 0x05;                               /* syscall */
    code[n++] = 0xF4;                                                 /* hlt */
    uint64_t page = (image_base + entry_rva) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    if (vmm_protect_page(pd, (uintptr_t)page, VMM_FLAG_USER) != 0)
        return false;
    return write_user(pd, image_base + entry_rva, code, n) == 0;
}

bool pe_win32_prepare_run(vmm_pd_t *pd, uint64_t image_base,
                          uint32_t size_of_image, uint32_t entry_rva,
                          uint64_t *entry_out, uint64_t *rsp_out) {
    (void)size_of_image;
    if (map_exec_pages(pd, PE_STACK_TOP - PE_STACK_BYTES, PE_STACK_BYTES) != 0)
        return false;
    uint64_t rsp = PE_STACK_TOP - 0x100;
    rsp &= ~0xFULL;
    *entry_out = image_base + entry_rva;
    *rsp_out = rsp;
    return true;
}

static int map_exec_pages(vmm_pd_t *pd, uint64_t addr, uint64_t len) {
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
                         VMM_FLAG_USER | VMM_FLAG_RW | VMM_FLAG_NX) != 0) {
            vmm_frame_free(pa);
            return -1;
        }
    }
    return 0;
}
