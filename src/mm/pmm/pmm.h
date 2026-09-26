#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine/limine.h"

#define PAGE_SIZE 4096

typedef uint64_t paddr_t;

struct pmm_region {
    paddr_t base;
    size_t pages;
    bool used;
};

extern struct pmm_region pmm_regions[];
extern size_t pmm_region_count;

void pmm_init(struct limine_memmap_response *memmap);
paddr_t pmm_alloc(size_t pages);
void pmm_free(paddr_t addr, size_t pages);
size_t pmm_total_pages(void);
size_t pmm_free_pages(void);
size_t pmm_used_pages(void);

static inline paddr_t pmm_page_to_addr(size_t page) {
    return page * PAGE_SIZE;
}

static inline size_t pmm_addr_to_page(paddr_t addr) {
    return addr / PAGE_SIZE;
}

#endif