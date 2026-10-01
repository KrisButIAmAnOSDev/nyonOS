#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot/limine/limine.h"

#define PAGE_SIZE 4096

typedef uint64_t paddr_t;

struct pmm_region {
    paddr_t base;
    size_t pages;
};

extern struct pmm_region pmm_regions[];
extern size_t pmm_region_count;

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);
bool pmm_alloc(paddr_t *out, size_t pages);
void pmm_free(paddr_t addr, size_t pages);
size_t pmm_total_pages(void);
size_t pmm_free_pages(void);

#endif
