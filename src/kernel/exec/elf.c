#include "elf.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/vmm/vmspace.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/uaccess/uaccess.h"
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

int elf_validate_hdr(const uint8_t *hdr, size_t hdr_len, size_t file_size,
                     size_t *out_phoff, size_t *out_phentsize, size_t *out_phnum,
                     bool *out_pie, uint64_t *out_entry) {
    (void)out_phentsize;
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

    *out_phoff = phoff;
    *out_phentsize = phentsize;
    *out_phnum = phnum;
    if (out_pie) *out_pie = (type == ELF_ET_DYN);
    (void)out_phentsize;
    return 0;
}

int elf_validate_phdrs(const elf64_phdr *ph, size_t phnum, size_t file_size,
                       bool is_pie, uint64_t entry, bool *out_ok) {
    if (out_ok) *out_ok = false;

    for (size_t i = 0; i < phnum; i++) {

        if (ph[i].type == ELF_PT_INTERP) return ELF_EINTERP;

        if (ph[i].type != ELF_PT_LOAD) continue;

        if (ph[i].filesz > ph[i].memsz) return ELF_EMEMSZ;

        if (ph[i].memsz == 0) continue;

        if (ph[i].filesz && ph[i].offset > file_size) return ELF_EOFFSET;
        if (ph[i].filesz && file_size - ph[i].offset < ph[i].filesz) return ELF_EOFFSET;

        if (ph[i].vaddr > UINT64_MAX - ph[i].memsz) return ELF_EADDR;
        if (ph[i].vaddr + ph[i].memsz > USER_MAX) return ELF_EADDR;
        if (is_pie && ph[i].vaddr + VM_LOAD_SPAN + ph[i].memsz > USER_MAX) return ELF_EADDR;

        if (ph[i].align && (PAGE_SIZE % ph[i].align) == 0) {
            if ((ph[i].vaddr & (ph[i].align - 1)) != (ph[i].offset & (ph[i].align - 1))) {
                return ELF_EALIGN;
            }
        }

        if ((ph[i].flags & ELF_PF_W) && (ph[i].flags & ELF_PF_X)) return ELF_EWX;
        if ((ph[i].flags & ELF_PF_W) && !(ph[i].flags & ELF_PF_R)) return ELF_EWX;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        total += (ph[i].memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    }
    if (total * PAGE_SIZE > ELF_MAX_IMAGE) return ELF_ETOOLARGE;

    bool entry_ok = false;
    for (size_t i = 0; i < phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        if (!(ph[i].flags & ELF_PF_X)) continue;
        if (entry >= ph[i].vaddr && entry < ph[i].vaddr + ph[i].memsz) { entry_ok = true; break; }
    }
    if (!entry_ok) return ELF_EENTRY;

    if (out_ok) *out_ok = true;
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
    return elf_load_shared(file, out, err, argv, argc, NULL);
}

int elf_load_shared(const fs_node *file, struct elf_image *out, const char **err,
                    const char **argv, size_t argc, const struct elf_shared *shared) {
    if (err) *err = NULL;
    if (!file || !out) return ELF_EOFFSET;
    if (file->is_dir) return ELF_EOFFSET;

    uint8_t ehdr[sizeof(elf64_header)];
    uint32_t ehdr_len = file->size < sizeof(ehdr) ? file->size : (uint32_t)sizeof(ehdr);
    if (ehdr_len < sizeof(ehdr)) { if (err) *err = elf_error_text(ELF_EBADMAGIC); return ELF_EBADMAGIC; }
    if (!fs_node_read(file, 0, ehdr, ehdr_len)) { if (err) *err = "short read"; return ELF_EOFFSET; }

    size_t phoff = 0, phentsize = 0, phnum = 0;
    bool is_pie = false;
    uint64_t entry = 0;
    int r = elf_validate_hdr(ehdr, ehdr_len, file->size, &phoff, &phentsize, &phnum, &is_pie, &entry);
    if (r) { if (err) *err = elf_error_text(r); return r; }

    elf64_phdr *ph = (elf64_phdr *)kmalloc(phnum * sizeof(elf64_phdr));
    if (!ph) { if (err) *err = "out of memory"; return ELF_ETOOLARGE; }

    for (size_t i = 0; i < phnum; i++) {
        uint8_t raw[sizeof(elf64_phdr)];
        if (!fs_node_read(file, (uint32_t)(phoff + i * phentsize), raw, sizeof(raw))) {
            kfree(ph);
            if (err) *err = "program header read failed";
            return ELF_EOFFSET;
        }
        phdr_read(raw, 0, &ph[i]);
    }

    bool ok = false;
    r = elf_validate_phdrs(ph, phnum, file->size, is_pie, entry, &ok);
    if (r || !ok) { kfree(ph); if (err) *err = elf_error_text(r); return r ? r : ELF_EBADMAGIC; }

    if (is_pie) {
        for (size_t i = 0; i < phnum; i++) {
            if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
            uint64_t hi = ph[i].vaddr + ph[i].memsz;
            if (ph[i].vaddr >= VM_LOAD_SPAN || hi > VM_LOAD_SPAN) { kfree((void *)ph); if (err) *err = "pie segment too big"; return ELF_EADDR; }
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
        if (load_bias >= USER_MAX - VM_LOAD_SPAN) {
            kfree((void *)ph);
            if (err) *err = elf_error_text(ELF_EADDR);
            return ELF_EADDR;
        }
    }

    struct vmspace *vs = vm_create();
    if (!vs) { kfree((void *)ph); if (err) *err = "out of memory"; return ELF_ETOOLARGE; }

    vaddr_t lo = UINT64_MAX, hi = 0;
    for (size_t i = 0; i < phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        if (ph[i].vaddr > USER_MAX - load_bias - ph[i].memsz) {
            vm_destroy(vs); kfree((void *)ph);
            if (err) *err = elf_error_text(ELF_EADDR);
            return ELF_EADDR;
        }
        vaddr_t s = ((ph[i].vaddr + load_bias) & ~(PAGE_SIZE - 1));
        vaddr_t e = ((ph[i].vaddr + load_bias + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    if (lo == UINT64_MAX) { vm_destroy(vs); kfree((void *)ph); if (err) *err = "no loadable segment"; return ELF_EADDR; }

    size_t npages = (size_t)((hi - lo) / PAGE_SIZE);
    if (npages == 0 || npages > (ELF_MAX_IMAGE / PAGE_SIZE)) {
        vm_destroy(vs); kfree((void *)ph);
        if (err) *err = "image too large";
        return ELF_ETOOLARGE;
    }

    uint8_t *prot = (uint8_t *)kmalloc(npages);
    if (!prot) { vm_destroy(vs); kfree((void *)ph); if (err) *err = "out of memory"; return ELF_ETOOLARGE; }
    for (size_t i = 0; i < npages; i++) prot[i] = 0;

    for (size_t i = 0; i < phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;
        uint8_t p = seg_prot(ph[i].flags);
        vaddr_t s = (ph[i].vaddr + load_bias) & ~(PAGE_SIZE - 1);
        vaddr_t e = (ph[i].vaddr + load_bias + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (vaddr_t va = s; va < e; va += PAGE_SIZE) {
            uint8_t *slot = &prot[(va - lo) / PAGE_SIZE];
            uint8_t merged = (uint8_t)(*slot | p);
            if ((merged & VM_PROT_WRITE) && (merged & VM_PROT_EXEC)) {
                kfree(prot); vm_destroy(vs); kfree((void *)ph);
                if (err) *err = elf_error_text(ELF_EWX);
                return ELF_EWX;
            }
            *slot = merged;
        }
    }

    int result = 0;
    uint64_t argc_out = 0, argv_out = 0, auxv_out = 0;

    for (size_t i = 0; i < phnum && result == 0; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) continue;

        uint64_t vaddr_seg = ph[i].vaddr + load_bias;
        vaddr_t start = vaddr_seg & ~(PAGE_SIZE - 1);
        vaddr_t vend = (vaddr_seg + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        vaddr_t fstart = vaddr_seg;
        vaddr_t fend = (vaddr_seg + ph[i].filesz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (vaddr_t va = start; va < vend; va += PAGE_SIZE) {
            size_t pi = (size_t)((va - lo) / PAGE_SIZE);
            uint8_t p = prot[pi];
            if (!p) continue;

            uint64_t existing = vmm_query_in(vs->pml4, va);

            paddr_t phys;
            uint8_t *page;

            if (existing & PAGE_PRESENT) {
                if (!(existing & PAGE_WRITE)) continue;
                phys = existing & 0x000FFFFFFFFFF000ULL;
                page = (uint8_t *)(vmm_hhdm_offset + phys);
            } else {
                if (!pmm_alloc(&phys, 1)) {
                    if (err) *err = "out of memory";
                    result = ELF_ETOOLARGE;
                    break;
                }
                page = (uint8_t *)(vmm_hhdm_offset + phys);
                for (size_t k = 0; k < PAGE_SIZE; k++) page[k] = 0;
            }

            vaddr_t read_from = va > fstart ? va : fstart;
            vaddr_t read_to = fend < vend ? fend : vend;
            if (read_to > read_from) {
                uint64_t skip = read_from - vaddr_seg;
                uint32_t want = (uint32_t)(ph[i].filesz - skip);
                if (want > PAGE_SIZE) want = PAGE_SIZE;
                if (want && !fs_node_read(file, (uint32_t)(ph[i].offset + skip), page + (read_from - va), want)) {
                    if (!(existing & PAGE_PRESENT)) pmm_free(phys, 1);
                    if (err) *err = "segment read failed";
                    result = ELF_EOFFSET;
                    break;
                }
            }

            if (!(existing & PAGE_PRESENT) && !vm_commit(vs, va, phys, 1, p, true)) {
                pmm_free(phys, 1);
                if (err) *err = "mapping failed";
                result = ELF_ETOOLARGE;
                break;
            }
        }
    }

    for (size_t i = 0; i < npages && result == 0; i++) {
        if (!prot[i]) continue;
        vaddr_t start = lo + i * PAGE_SIZE;
        vaddr_t end = start + PAGE_SIZE;
        if (i && prot[i - 1] == prot[i]) continue;
        if (vm_region_add(vs, start, end, prot[i], VM_BACKING_ANON) != 0) {
            kfree(prot);
            vm_destroy(vs);
            kfree((void *)ph);
            if (err) *err = "too many regions";
            return ELF_ETOOLARGE;
        }
    }

    kfree(prot);

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

    if (shared && shared->phys && shared->pages) {
        if (vm_region_add(vs, shared->virt, shared->virt + shared->pages * PAGE_SIZE,
                          VM_PROT_READ | VM_PROT_WRITE, VM_BACKING_NONE) != 0 ||
            !vm_commit(vs, shared->virt, shared->phys, shared->pages,
                       VM_PROT_READ | VM_PROT_WRITE, false)) {
            if (err) *err = "cannot map the shared page";
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
