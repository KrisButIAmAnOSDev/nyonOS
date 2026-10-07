#include "elf.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/vmm/vmspace.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/usermode.h"
#include "kernel/crypto/crypto.h"
#include "kernel/fs/fs.h"

#define VM_LOAD_BASE   0x0000000040000000ULL
#define VM_LOAD_SPAN   0x0000000100000000ULL
#define VM_LOAD_LIMIT  0x0000000100000000ULL
#define VM_STACK_TOP   0x0000000040000000ULL

static void phdr_read(const uint8_t *base, size_t i, elf64_phdr *out) {
    const uint8_t *p = base + i * sizeof(elf64_phdr);
    out->type    = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    out->flags   = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    out->offset  = 0;
    for (int k = 0; k < 8; k++) out->offset |= (uint64_t)p[8 + k] << (8 * k);
    out->vaddr = 0;
    for (int k = 0; k < 8; k++) out->vaddr |= (uint64_t)p[16 + k] << (8 * k);
    out->paddr = 0;
    for (int k = 0; k < 8; k++) out->paddr |= (uint64_t)p[24 + k] << (8 * k);
    out->filesz = 0;
    for (int k = 0; k < 8; k++) out->filesz |= (uint64_t)p[32 + k] << (8 * k);
    out->memsz = 0;
    for (int k = 0; k < 8; k++) out->memsz |= (uint64_t)p[40 + k] << (8 * k);
    out->align = 0;
    for (int k = 0; k < 8; k++) out->align |= (uint64_t)p[48 + k] << (8 * k);
}

int elf_validate(const uint8_t *hdr, size_t hdr_len, size_t file_size, const elf64_phdr **out_ph, size_t *out_n, bool *out_pie, uint64_t *out_entry) {
    if (!hdr || hdr_len < sizeof(elf64_header)) return ELF_EBADMAGIC;

    if (hdr[0] != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') return ELF_EBADMAGIC;
    if (hdr[4] != 2) return ELF_EBADCLASS;
    if (hdr[5] != 1) return ELF_EBADDATA;
    if (hdr[6] != 1) return ELF_EBADVERS;

    uint16_t type = (uint16_t)(hdr[16] | (hdr[17] << 8));
    uint16_t machine = (uint16_t)(hdr[18] | (hdr[19] << 8));
    uint64_t entry = 0;
    for (int k = 0; k < 8; k++) entry |= (uint64_t)hdr[24 + k] << (8 * k);
    *out_entry = entry;
    uint64_t phoff = 0;
    for (int k = 0; k < 8; k++) phoff |= (uint64_t)hdr[32 + k] << (8 * k);
    uint16_t ehsize = (uint16_t)(hdr[52] | (hdr[53] << 8));
    uint16_t phentsize = (uint16_t)(hdr[54] | (hdr[55] << 8));
    uint16_t phnum = (uint16_t)(hdr[56] | (hdr[57] << 8));

    if (machine != 62) return ELF_EBADMACHINE;
    if (type != ELF_ET_EXEC && type != ELF_ET_DYN) return ELF_EBADTYPE;
    if (ehsize < sizeof(elf64_header)) return ELF_EPHDRSIZE;
    if (phentsize < sizeof(elf64_phdr)) return ELF_EPHDRSIZE;
    if (phnum == 0) return ELF_EPHNUM;
    if (phnum > ELF_MAX_PHNUM) return ELF_EPHNUM;

    if (phoff > file_size || file_size - phoff < (size_t)phnum * phentsize) return ELF_EOFFSET;

    bool is_pie = type == ELF_ET_DYN;

    elf64_phdr *ph = (elf64_phdr *)kmalloc((size_t)phnum * sizeof(elf64_phdr));
    if (!ph) return ELF_ETOOLARGE;

    for (size_t i = 0; i < phnum; i++) {
        phdr_read(hdr + phoff + i * phentsize, 0, &ph[i]);

        if (ph[i].type == ELF_PT_INTERP) { kfree(ph); return ELF_EINTERP; }

        if (ph[i].type != ELF_PT_LOAD) continue;

        if (ph[i].filesz > ph[i].memsz) { kfree(ph); return ELF_EMEMSZ; }

        if (ph[i].memsz == 0) continue;

        if (ph[i].filesz && ph[i].offset > file_size) { kfree(ph); return ELF_EOFFSET; }
        if (ph[i].filesz && file_size - ph[i].offset < ph[i].filesz) { kfree(ph); return ELF_EOFFSET; }

        if (ph[i].vaddr > UINT64_MAX - ph[i].memsz) { kfree(ph); return ELF_EADDR; }
        if (ph[i].vaddr + ph[i].memsz > USER_MAX) { kfree(ph); return ELF_EADDR; }
        if (is_pie && ph[i].vaddr + VM_LOAD_SPAN + ph[i].memsz > USER_MAX) { kfree(ph); return ELF_EADDR; }

        if (ph[i].align && (PAGE_SIZE % ph[i].align) == 0) {
            if ((ph[i].vaddr & (ph[i].align - 1)) != (ph[i].offset & (ph[i].align - 1))) {
                kfree(ph); return ELF_EALIGN;
            }
        }

        if ((ph[i].flags & ELF_PF_W) && (ph[i].flags & ELF_PF_X)) { kfree(ph); return ELF_EWX; }
        if ((ph[i].flags & ELF_PF_W) && !(ph[i].flags & ELF_PF_R)) { kfree(ph); return ELF_EWX; }
    }

    uint64_t total = 0;
    for (size_t i = 0; i < phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        total += (ph[i].memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    }
    if (total * PAGE_SIZE > ELF_MAX_IMAGE) { kfree(ph); return ELF_ETOOLARGE; }

    bool entry_ok = false;
    for (size_t i = 0; i < phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        if (!(ph[i].flags & ELF_PF_X)) continue;
        if (entry >= ph[i].vaddr && entry < ph[i].vaddr + ph[i].memsz) { entry_ok = true; break; }
    }
    if (!entry_ok) { kfree(ph); return ELF_EENTRY; }

    if (out_ph) *out_ph = ph;
    else kfree(ph);
    if (out_n) *out_n = phnum;
    if (out_pie) *out_pie = is_pie;
    return 0;
}

const char *elf_error_text(int err) {
    switch (err) {
        case 0: return "ok";
        case ELF_EBADMAGIC: return "not an elf file";
        case ELF_EBADCLASS: return "not 64-bit";
        case ELF_EBADDATA: return "not little-endian";
        case ELF_EBADVERS: return "bad elf version";
        case ELF_EBADMACHINE: return "not for x86-64";
        case ELF_EBADTYPE: return "bad elf type";
        case ELF_EPHDRSIZE: return "bad header size";
        case ELF_EPHNUM: return "bad program header count";
        case ELF_EOFFSET: return "program headers outside the file";
        case ELF_EMEMSZ: return "segment bigger in file than in memory";
        case ELF_EALIGN: return "segment offset and address disagree on alignment";
        case ELF_EADDR: return "segment address outside the user range";
        case ELF_EOVERLAP: return "segments overlap";
        case ELF_EWX: return "segment is writable and executable";
        case ELF_EENTRY: return "entry point is not in an executable segment";
        case ELF_ETOOLARGE: return "image too large";
        case ELF_EINTERP: return "dynamic binaries are not supported yet";
        default: return "bad elf";
    }
}

static uint8_t seg_prot(uint32_t flags) {
    uint8_t p = 0;
    if (flags & ELF_PF_R) p |= VM_PROT_READ;
    if (flags & ELF_PF_W) p |= VM_PROT_WRITE;
    if (flags & ELF_PF_X) p |= VM_PROT_EXEC;
    if (!p) p = VM_PROT_READ;
    return p;
}

int elf_load_argv(const fs_node *file, struct elf_image *out, const char **err,
                  const char **argv, size_t argc) {
    if (err) *err = NULL;
    if (!file || !out) return ELF_EOFFSET;
    if (file->is_dir) return ELF_EOFFSET;

    static uint8_t hdrbuf[1024];
    uint32_t hdr_len = sizeof(hdrbuf);
    if (file->size < hdr_len) hdr_len = file->size;
    if (hdr_len < sizeof(elf64_header)) { if (err) *err = elf_error_text(ELF_EBADMAGIC); return ELF_EBADMAGIC; }
    if (!fs_node_read(file, 0, hdrbuf, hdr_len)) { if (err) *err = "short read"; return ELF_EOFFSET; }

    const elf64_phdr *ph = NULL;
    size_t phnum = 0;
    bool is_pie = false;
    uint64_t entry = 0;
    int r = elf_validate(hdrbuf, hdr_len, file->size, &ph, &phnum, &is_pie, &entry);
    if (r) { if (err) *err = elf_error_text(r); return r; }

    if (is_pie) {
        for (size_t i = 0; i < phnum; i++) {
            if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
            uint64_t hi = ph[i].vaddr + ph[i].memsz;
            if (ph[i].vaddr >= VM_LOAD_SPAN || hi > VM_LOAD_SPAN) { kfree((void *)ph); if (err) *err = "pie segment too big"; return ELF_EADDR; }
            if (ph[i].vaddr >= USER_MAX || hi > USER_MAX) { kfree((void *)ph); if (err) *err = elf_error_text(ELF_EADDR); return ELF_EADDR; }
            if (ph[i].vaddr + VM_LOAD_SPAN + hi > USER_MAX) { kfree((void *)ph); if (err) *err = elf_error_text(ELF_EADDR); return ELF_EADDR; }
        }
    } else {
        for (size_t i = 0; i < phnum; i++) {
            if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
            if (ph[i].vaddr < VM_LOAD_BASE) { kfree((void *)ph); if (err) *err = elf_error_text(ELF_EADDR); return ELF_EADDR; }
            if (ph[i].vaddr + ph[i].memsz > VM_LOAD_LIMIT) { kfree((void *)ph); if (err) *err = elf_error_text(ELF_EADDR); return ELF_EADDR; }
        }
    }

    uint64_t load_bias = 0;
    if (is_pie) {
        uint64_t r = crypto_random_u64();
        load_bias = VM_LOAD_BASE + (r % 0x400) * PAGE_SIZE;
    }

    struct vmspace *vs = vm_create();
    if (!vs) { kfree((void *)ph); if (err) *err = "out of memory"; return ELF_ETOOLARGE; }

    size_t loads = 0;
    for (size_t i = 0; i < phnum; i++) if (ph[i].type == ELF_PT_LOAD && ph[i].memsz) loads++;

    int result = 0;
    uint64_t argc_out = 0, argv_out = 0, auxv_out = 0;
    (void)loads;

    for (size_t i = 0; i < phnum && result == 0; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;

        uint8_t prot = seg_prot(ph[i].flags);
        vaddr_t start = (ph[i].vaddr + load_bias) & ~(PAGE_SIZE - 1);
        vaddr_t vend = (ph[i].vaddr + load_bias + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size_t pages = (size_t)((vend - start) / PAGE_SIZE);

        vaddr_t file_end = (ph[i].vaddr + load_bias + ph[i].filesz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        if (vm_region_add(vs, start, vend, prot, VM_BACKING_ANON) != 0) {
            if (err) *err = elf_error_text(ELF_EOVERLAP);
            result = ELF_EOVERLAP;
            break;
        }

        for (size_t p = 0; p < pages; p++) {
            vaddr_t va = start + p * PAGE_SIZE;
            paddr_t phys;
            if (!pmm_alloc(&phys, 1)) { if (err) *err = "out of memory"; result = ELF_ETOOLARGE; break; }

            uint8_t *page = (uint8_t *)(vmm_hhdm_offset + phys);
            for (size_t k = 0; k < PAGE_SIZE; k++) page[k] = 0;

            vaddr_t file_page_end = file_end;
            if (va < file_page_end) {
                vaddr_t skip = (va > (ph[i].vaddr + load_bias)) ? va - (ph[i].vaddr + load_bias) : 0;
                uint32_t want = (uint32_t)(ph[i].filesz - skip);
                if (want > PAGE_SIZE) want = PAGE_SIZE;
                uint32_t got = want ? want : 0;
                if (got && !fs_node_read(file, (uint32_t)(ph[i].offset + skip), page, got)) {
                    pmm_free(phys, 1);
                    if (err) *err = "segment read failed";
                    result = ELF_EOFFSET;
                    break;
                }
            }

            if (!vm_commit(vs, va, phys, 1, prot, true)) {
                pmm_free(phys, 1);
                if (err) *err = "mapping failed";
                result = ELF_ETOOLARGE;
                break;
            }
        }
    }

    if (is_pie) {
        for (size_t i = 0; i < phnum; i++) {
            if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
            if (!(ph[i].flags & ELF_PF_W)) continue;
            if (ph[i].vaddr > 0x2000) continue;
            if (ph[i].vaddr + ph[i].memsz <= 0x2000) continue;

            uint64_t off = 0x2000 - ph[i].vaddr;
            if (off + 8 > ph[i].filesz) continue;

            vaddr_t va = 0x2000 + load_bias;
            uint64_t e = vmm_query_in(vs->pml4, va & ~(PAGE_SIZE - 1));
            if (!(e & PAGE_PRESENT)) continue;

            uint64_t *slot = (uint64_t *)(vmm_hhdm_offset + (e & 0x000FFFFFFFFFF000ULL) + (va & (PAGE_SIZE - 1)));
            *slot = load_bias;
            break;
        }
    }

    if (!vs->has_stack && !vm_setup_stack(vs)) {
        if (err) *err = "no stack";
        kfree((void *)ph);
        vm_destroy(vs);
        return ELF_ETOOLARGE;
    }

    uint64_t sp = vs->stack_top - 16;
    if (argv && argc) {
        if (!vm_stack_push_strings(vs, argv, argc, &argc_out, &sp, &argv_out, &auxv_out)) {
            if (err) *err = "cannot build the initial stack";
            kfree((void *)ph);
            vm_destroy(vs);
            return ELF_ETOOLARGE;
        }
    }

    out->entry = entry + load_bias;
    out->is_pie = is_pie;
    out->stack_top = sp;
    out->argc = argc_out;
    out->argv = argv_out;
    out->auxv = auxv_out;

    kfree((void *)ph);

    if (result) {
        vm_destroy(vs);
        return result;
    }

    out->vs = vs;
    return 0;
}

int elf_load(const fs_node *file, struct elf_image *out, const char **err) {
    return elf_load_argv(file, out, err, NULL, 0);
}

void elf_destroy(struct elf_image *img) {
    if (!img || !img->vs) return;
    vm_destroy(img->vs);
    img->vs = NULL;
}
