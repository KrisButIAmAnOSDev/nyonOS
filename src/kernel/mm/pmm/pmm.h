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

#define PMM_TAG_NONE    0
#define PMM_TAG_HEAP    1
#define PMM_TAG_CACHE   2
#define PMM_TAG_PTABLES 3
#define PMM_TAG_COW     4
#define PMM_TAG_ELF     5
#define PMM_TAG_STACK   6
#define PMM_TAG_SLAB    7
#define PMM_TAG_USER    8
#define PMM_TAG_MAX     9

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);
void pmm_set_tag(uint8_t tag);
size_t pmm_pages_by_tag(uint8_t tag);
bool pmm_alloc(paddr_t *out, size_t pages);
void pmm_free(paddr_t addr, size_t pages);
size_t pmm_total_pages(void);
void pmm_share_ref(paddr_t addr);
bool pmm_unshare_ref(paddr_t addr);
bool pmm_share_ready(void);
size_t pmm_free_pages(void);
size_t pmm_reserved_pages(void);
size_t pmm_usable_pages(void);
size_t pmm_cache_pages_used(void);

#endif
