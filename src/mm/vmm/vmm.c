#include "vmm.h"
#include "../pmm/pmm.h"
#include "../../io/serial/serial.h"

struct page_table *kernel_pml4 = NULL;
bool vmm_5level = false;

static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void write_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3));
}

static inline void invlpg(vaddr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static bool cpu_has_5level(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    return (ecx & (1 << 16)) != 0;
}

static struct page_table *alloc_page_table(void) {
    paddr_t phys = pmm_alloc(1);
    if (!phys) return NULL;
    struct page_table *pt = (struct page_table *)(VMM_KERNEL_BASE + phys);
    for (size_t i = 0; i < 512; i++) pt->entries[i] = 0;
    return pt;
}

static void free_page_table(struct page_table *pt) {
    pmm_free((paddr_t)((uint64_t)pt - VMM_KERNEL_BASE), 1);
}

static uint64_t *walk_page_table(struct page_table *table, vaddr_t vaddr, int level, bool alloc) {
    if (level == 0) return NULL;
    
    int shift;
    if (vmm_5level) {
        static const int shifts[5] = {48, 39, 30, 21, 12};
        shift = shifts[5 - level];
    } else {
        static const int shifts[4] = {39, 30, 21, 12};
        shift = shifts[4 - level];
    }
    
    size_t index = (vaddr >> shift) & 0x1FF;
    uint64_t entry = table->entries[index];
    
    if (level == 1) return &table->entries[index];
    
    struct page_table *next;
    if (entry & PAGE_PRESENT) {
        next = (struct page_table *)(VMM_KERNEL_BASE + (entry & 0x000FFFFFFFFFF000ULL));
    } else if (alloc) {
        next = alloc_page_table();
        if (!next) return NULL;
        table->entries[index] = ((uint64_t)next - VMM_KERNEL_BASE) | PAGE_PRESENT | PAGE_WRITE;
    } else {
        return NULL;
    }
    
    return walk_page_table(next, vaddr, level - 1, alloc);
}

void vmm_init(void) {
    vmm_5level = cpu_has_5level();
    
    uint64_t cr3 = read_cr3();
    kernel_pml4 = (struct page_table *)(VMM_KERNEL_BASE + (cr3 & 0x000FFFFFFFFFF000ULL));
    
    serial_print("VMM: Initialized, ");
    serial_print(vmm_5level ? "5-level paging\n" : "4-level paging\n");
}

bool vmm_map(vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    if (pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1) || paddr & (PAGE_SIZE - 1)) return false;
    
    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;
        paddr_t p = paddr + i * PAGE_SIZE;
        
        uint64_t *entry = walk_page_table(kernel_pml4, v, vmm_5level ? 5 : 4, true);
        if (!entry) return false;
        
        if (*entry & PAGE_PRESENT) return false;
        *entry = p | flags | PAGE_PRESENT;
    }
    
    return true;
}

bool vmm_unmap(vaddr_t vaddr, size_t pages) {
    if (pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1)) return false;
    
    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;
        
        uint64_t *entry = walk_page_table(kernel_pml4, v, vmm_5level ? 5 : 4, false);
        if (!entry || !(*entry & PAGE_PRESENT)) return false;
        
        *entry = 0;
        invlpg(v);
    }
    
    return true;
}

paddr_t vmm_virt_to_phys(vaddr_t vaddr) {
    uint64_t *entry = walk_page_table(kernel_pml4, vaddr, vmm_5level ? 5 : 4, false);
    if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    return (*entry & 0x000FFFFFFFFFF000ULL) | (vaddr & (PAGE_SIZE - 1));
}

bool vmm_is_mapped(vaddr_t vaddr) {
    uint64_t *entry = walk_page_table(kernel_pml4, vaddr, vmm_5level ? 5 : 4, false);
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
    write_cr3((uint64_t)pml4 - VMM_KERNEL_BASE);
}

void vmm_invlpg(vaddr_t vaddr) {
    invlpg(vaddr);
}