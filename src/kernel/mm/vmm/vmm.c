#include "vmm.h"
#include "io/kprintf/kprintf.h"
#include "kernel/mm/pmm/pmm.h"
#include "io/serial/serial.h"
#include "kernel/sync/sync.h"

static spinlock_t vmm_lock = SPINLOCK_INIT;

struct page_table *kernel_pml4 = NULL;
bool vmm_5level = false;
uint64_t vmm_hhdm_offset = 0;

static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void write_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3));
}

static inline uint64_t read_cr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void invlpg(vaddr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

#define CR4_LA57 (1ULL << 12)

static bool cpu_has_5level(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    return (ecx & (1 << 16)) != 0;
}

#define VMM_PT_ENTRIES 512
#define VMM_PT_INDEX_MASK 0x1FF
#define VMM_PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

static struct page_table *alloc_page_table(void) {
    paddr_t phys;
    if (!pmm_alloc(&phys, 1)) return NULL;
    struct page_table *pt = (struct page_table *)(vmm_hhdm_offset + phys);
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) pt->entries[i] = 0;
    return pt;
}

static void free_page_table(struct page_table *pt) {
    pmm_free((paddr_t)((uint64_t)pt - vmm_hhdm_offset), 1);
}

static int level_shift(int level) {
    if (vmm_5level) {
        static const int shifts[5] = {48, 39, 30, 21, 12};
        return shifts[5 - level];
    }
    static const int shifts[4] = {39, 30, 21, 12};
    return shifts[4 - level];
}

static bool table_is_empty(struct page_table *pt) {
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        if (pt->entries[i]) return false;
    }
    return true;
}

static uint64_t *walk_page_table(struct page_table *table, vaddr_t vaddr, int level, bool alloc, uint64_t flags) {
    if (level == 0) return NULL;

    int shift = level_shift(level);

    size_t index = (vaddr >> shift) & VMM_PT_INDEX_MASK;
    uint64_t entry = table->entries[index];

    if (level == 1) return &table->entries[index];

    struct page_table *next;
    if (entry & PAGE_PRESENT) {
        if (alloc && (flags & PAGE_USER) && !(entry & PAGE_USER)) {
            table->entries[index] = entry | PAGE_USER;
        }
        next = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
    } else if (alloc) {
        next = alloc_page_table();
        if (!next) return NULL;
        uint64_t iflags = PAGE_PRESENT | PAGE_WRITE;
        if (flags & PAGE_USER) iflags |= PAGE_USER;
        table->entries[index] = ((uint64_t)next - vmm_hhdm_offset) | iflags;
    } else {
        return NULL;
    }

    return walk_page_table(next, vaddr, level - 1, alloc, flags);
}

// Frees the child table at `level` and any ancestors that became empty.
// Never frees the root table, which is shared with the rest of the kernel.
static void reclaim_ancestors(struct page_table *table, vaddr_t vaddr, int level) {
    int shift = level_shift(level);
    size_t index = (vaddr >> shift) & VMM_PT_INDEX_MASK;
    uint64_t entry = table->entries[index];

    if (!(entry & PAGE_PRESENT)) return;

    struct page_table *child = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));

    if (level > 1) reclaim_ancestors(child, vaddr, level - 1);

    if (table == kernel_pml4) return;
    if (!table_is_empty(child)) return;

    table->entries[index] = 0;
    invlpg(vaddr);
    free_page_table(child);
}

void vmm_init(uint64_t hhdm_offset) {
    vmm_hhdm_offset = hhdm_offset;

    bool cpu_supports_la57 = cpu_has_5level();
    bool paging_is_5level = (read_cr4() & CR4_LA57) != 0;
    vmm_5level = cpu_supports_la57 && paging_is_5level;

    uint64_t cr3 = read_cr3();
    kernel_pml4 = (struct page_table *)(vmm_hhdm_offset + (cr3 & 0x000FFFFFFFFFF000ULL));

    kprintf(PRINT_SERIAL, "VMM: Initialized, ");
    kprintf(PRINT_SERIAL, "%s", vmm_5level ? "5-level paging\n" : "4-level paging\n");
    if (cpu_supports_la57 && !paging_is_5level) {
        kprintf(PRINT_SERIAL, "VMM: LA57 available but CR4.LA57 clear, using 4-level walk\n");
    }
}

// TODO: reclaim intermediate page tables that become empty after an unmap.
// free_page_table exists but is never called, so empty tables leak physical
// memory under map/unmap churn. Needed for real address-space teardown.

bool vmm_map(vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    if (pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1) || paddr & (PAGE_SIZE - 1)) return false;
    
    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t *entry = walk_page_table(kernel_pml4, vaddr + i * PAGE_SIZE, levels, true, flags);
        if (!entry) {
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
        if (*entry & PAGE_PRESENT) {
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;
        paddr_t p = paddr + i * PAGE_SIZE;

        uint64_t *entry = walk_page_table(kernel_pml4, v, levels, true, flags);
        *entry = p | flags | PAGE_PRESENT;
    }

    lock_release(LOCK_VMM, &vmm_lock);
    return true;
}

bool vmm_unmap(vaddr_t vaddr, size_t pages) {
    if (pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1)) return false;
    
    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t *entry = walk_page_table(kernel_pml4, vaddr + i * PAGE_SIZE, levels, false, 0);
        if (!entry || !(*entry & PAGE_PRESENT)) {
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;

        uint64_t *entry = walk_page_table(kernel_pml4, v, levels, false, 0);
        *entry = 0;
        invlpg(v);
    }

    for (size_t i = 0; i < pages; i++) {
        reclaim_ancestors(kernel_pml4, vaddr + i * PAGE_SIZE, levels);
    }

    lock_release(LOCK_VMM, &vmm_lock);
    return true;
}

paddr_t vmm_virt_to_phys(vaddr_t vaddr) {
    uint64_t *entry = walk_page_table(kernel_pml4, vaddr, vmm_5level ? 5 : 4, false, 0);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    return (*entry & 0x000FFFFFFFFFF000ULL) | (vaddr & (PAGE_SIZE - 1));
}

uint64_t vmm_query(vaddr_t vaddr) {
    uint64_t *entry = walk_page_table(kernel_pml4, vaddr, vmm_5level ? 5 : 4, false, 0);
    if (!entry) return 0;
    return *entry;
}

bool vmm_is_mapped(vaddr_t vaddr) {
    uint64_t *entry = walk_page_table(kernel_pml4, vaddr, vmm_5level ? 5 : 4, false, 0);
    return entry && (*entry & PAGE_PRESENT);
}

struct page_table *vmm_create_address_space(void) {
    struct page_table *new_pml4 = alloc_page_table();
    if (!new_pml4) return NULL;
    
    for (size_t i = 256; i < 512; i++) {
        new_pml4->entries[i] = kernel_pml4->entries[i];
    }
    
    return new_pml4;
}

void vmm_switch_address_space(struct page_table *pml4) {
    write_cr3((uint64_t)pml4 - vmm_hhdm_offset);
}

void vmm_invlpg(vaddr_t vaddr) {
    invlpg(vaddr);
}
