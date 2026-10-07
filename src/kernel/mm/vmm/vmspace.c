#include "vmspace.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/sync/preempt.h"
#include "kernel/usermode.h"
#include "kernel/mm/heap/heap.h"

uint64_t vm_flags_for(uint8_t prot) {
    uint64_t f = VMM_USER_EXEC_FLAGS;
    if (!(prot & VM_PROT_WRITE)) f &= ~PAGE_WRITE;
    return f;
}

static struct vm_region *region_slot(struct vmspace *vs) {
    if (vs->region_count >= VM_MAX_REGIONS) return NULL;
    return &vs->regions[vs->region_count++];
}

struct vmspace *vm_create(void) {
    struct vmspace *vs = (struct vmspace *)kmalloc(sizeof(struct vmspace));
    if (!vs) return NULL;

    vs->pml4 = vmm_create_address_space();
    if (!vs->pml4) { kfree(vs); return NULL; }

    vs->refcount = 1;
    vs->region_count = 0;
    vs->stack_base = 0;
    vs->stack_bottom = 0;
    vs->stack_top = 0;
    vs->has_stack = false;
    return vs;
}

struct vmspace *vm_ref(struct vmspace *vs) {
    if (!vs) return NULL;
    vs->refcount++;
    return vs;
}

void vm_free_range(struct vmspace *vs, vaddr_t start, vaddr_t end) {
    if (!vs || !vs->pml4) return;
    if (start >= end) return;

    vaddr_t a = start & ~(PAGE_SIZE - 1);
    vaddr_t b = end & ~(PAGE_SIZE - 1);
    if (b <= a) return;

    size_t pages = (size_t)((b - a) / PAGE_SIZE);

    for (size_t i = 0; i < pages; i++) {
        uint64_t entry = vmm_query_in(vs->pml4, a + i * PAGE_SIZE);
        if (entry & PAGE_PRESENT) pmm_free(entry & 0x000FFFFFFFFFF000ULL, 1);
    }

    vmm_unmap_keep(vs->pml4, a, pages);
}

void vm_destroy(struct vmspace *vs) {
    if (!vs) return;
    if (--vs->refcount > 0) return;

    if (vs->pml4) {
        vmm_destroy_address_space(vs->pml4);
        vs->pml4 = NULL;
    }

    kfree(vs);
}

int vm_region_add(struct vmspace *vs, vaddr_t start, vaddr_t end, uint8_t prot, uint8_t backing) {
    if (!vs || start >= end) return -1;
    if ((start & (PAGE_SIZE - 1)) || (end & (PAGE_SIZE - 1))) return -1;
    if (prot & ~(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) return -1;
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC)) return -1;
    if ((prot & VM_PROT_WRITE) && !(prot & VM_PROT_READ)) return -1;

    for (size_t i = 0; i < vs->region_count; i++) {
        struct vm_region *r = &vs->regions[i];
        if (start < r->end && r->start < end) return -1;
    }

    preempt_disable();
    struct vm_region *slot = region_slot(vs);
    if (!slot) { preempt_enable(); return -1; }

    slot->start = start;
    slot->end = end;
    slot->prot = prot;
    slot->maxprot = prot;
    slot->backing = backing;
    slot->mapped = true;
    preempt_enable();
    return 0;
}

struct vm_region *vm_region_find(struct vmspace *vs, vaddr_t addr) {
    if (!vs) return NULL;
    for (size_t i = 0; i < vs->region_count; i++) {
        struct vm_region *r = &vs->regions[i];
        if (addr >= r->start && addr < r->end) return r;
    }
    return NULL;
}

bool vm_commit(struct vmspace *vs, vaddr_t vaddr, paddr_t phys, size_t pages, uint8_t prot, bool owned) {
    if (!vs || !vs->pml4 || pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1) || phys & (PAGE_SIZE - 1)) return false;
    return vmm_map_frames(vs->pml4, vaddr, phys, pages, vm_flags_for(prot),
                          (prot & VM_PROT_EXEC) != 0, owned);
}

bool vm_map_anon(struct vmspace *vs, vaddr_t vaddr, size_t pages, uint8_t prot) {
    return vm_map_anon_zeroed(vs, vaddr, pages, prot, false);
}

bool vm_map_anon_zeroed(struct vmspace *vs, vaddr_t vaddr, size_t pages, uint8_t prot, bool zero) {
    if (!vs || !vs->pml4 || pages == 0) return false;

    paddr_t phys;
    if (!pmm_alloc(&phys, pages)) return false;

    if (zero) {
        uint8_t *dst = (uint8_t *)(vmm_hhdm_offset + phys);
        for (size_t i = 0; i < pages * PAGE_SIZE; i++) dst[i] = 0;
    }

    if (!vm_commit(vs, vaddr, phys, pages, prot, true)) {
        pmm_free(phys, pages);
        return false;
    }

    struct vm_region *r = vm_region_find(vs, vaddr);
    if (r) r->mapped = true;
    return true;
}

bool vm_setup_stack(struct vmspace *vs) {
    if (!vs) return false;

    vs->stack_top = VM_STACK_TOP_VIRT;
    vs->stack_base = vs->stack_top - (size_t)VM_STACK_PAGES * PAGE_SIZE;
    vs->stack_bottom = vs->stack_base - (vaddr_t)VM_STACK_GUARD_PAGES * PAGE_SIZE;

    if (vm_region_add(vs, vs->stack_bottom, vs->stack_top, VM_PROT_READ | VM_PROT_WRITE, VM_BACKING_ANON) != 0)
        return false;

    if (!vm_map_anon_zeroed(vs, vs->stack_base, VM_STACK_PAGES, VM_PROT_READ | VM_PROT_WRITE, true))
        return false;

    vs->has_stack = true;
    return true;
}

static uint8_t *stack_page(struct vmspace *vs, uint64_t *vbase_out) {
    uint64_t entry = vmm_query_in(vs->pml4, (vs->stack_top - 1) & ~(PAGE_SIZE - 1));
    if (!(entry & PAGE_PRESENT)) return NULL;
    uint8_t *base = (uint8_t *)(vmm_hhdm_offset + (entry & 0x000FFFFFFFFFF000ULL));
    *vbase_out = (vs->stack_top - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return base;
}

bool vm_stack_push_strings(struct vmspace *vs, const char **strs, size_t count,
                           uint64_t *argc_out, uint64_t *stack_out, uint64_t *argv_out, uint64_t *auxv_out) {
    if (!vs || !vs->pml4 || !vs->has_stack) return false;
    if (count > 32) return false;

    uint64_t vbase = 0;
    uint8_t *base = stack_page(vs, &vbase);
    if (!base) return false;

    size_t room = (size_t)(vs->stack_top - vs->stack_base);
    if (room < 1024) return false;

    uint8_t *top = base + (vs->stack_top - vbase);
    uint8_t *floor = base + (vs->stack_base - vbase);
    if (floor < base) floor = base;

    uint64_t argv_ptr[32];

    for (size_t i = 0; i < count; i++) {
        size_t sl = 0;
        while (strs[i][sl]) sl++;

        if ((size_t)(top - floor) < sl + 16) return false;

        uint8_t *dst = top - sl - 1;
        for (size_t k = 0; k <= sl; k++) dst[k] = (uint8_t)strs[i][k];

        argv_ptr[i] = vbase + (uint64_t)(dst - base);
        top = (uint8_t *)((uintptr_t)dst & ~0x7ULL);
    }

    top = (uint8_t *)((uintptr_t)top & ~0xFULL);

    if ((size_t)(top - floor) < (count + 2) * 8) return false;

    uint64_t *sp = (uint64_t *)top - (count + 2);
    sp[0] = count;
    for (size_t i = 0; i < count; i++) sp[1 + i] = argv_ptr[i];
    sp[1 + count] = 0;

    uint64_t sp_virt = vbase + (uint64_t)((uint8_t *)sp - base);

    if (argc_out) *argc_out = count;
    if (stack_out) *stack_out = sp_virt;
    if (argv_out) *argv_out = sp_virt + 8;
    if (auxv_out) *auxv_out = sp_virt + (uint64_t)(count + 2) * 8;
    return true;
}
