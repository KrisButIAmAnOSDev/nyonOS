#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../pmm/pmm.h"

#define PAGE_SIZE 4096
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITE      (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_PWT        (1ULL << 3)
#define PAGE_PCD        (1ULL << 4)
#define PAGE_ACCESSED   (1ULL << 5)
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_HUGE       (1ULL << 7)
#define PAGE_GLOBAL     (1ULL << 8)
#define PAGE_NX         (1ULL << 63)

extern uint64_t vmm_hhdm_offset;
#define VMM_HIGHER_HALF 0xFFFF800000000000

typedef uint64_t vaddr_t;
typedef uint64_t paddr_t;

struct page_table {
    uint64_t entries[512];
} __attribute__((aligned(4096)));

extern struct page_table *kernel_pml4;
extern bool vmm_5level;

void vmm_init(uint64_t hhdm_offset);
bool vmm_map(vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags);
bool vmm_unmap(vaddr_t vaddr, size_t pages);
paddr_t vmm_virt_to_phys(vaddr_t vaddr);
bool vmm_is_mapped(vaddr_t vaddr);
struct page_table *vmm_create_address_space(void);
void vmm_switch_address_space(struct page_table *pml4);
void vmm_invlpg(vaddr_t vaddr);

static inline bool vmm_is_5level(void) { return vmm_5level; }

#define VMM_DEFAULT_FLAGS (PAGE_PRESENT | PAGE_WRITE | PAGE_GLOBAL)
#define VMM_USER_FLAGS (PAGE_PRESENT | PAGE_WRITE | PAGE_USER)

#endif
