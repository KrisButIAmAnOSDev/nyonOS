#ifndef VMM_VA_H
#define VMM_VA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/mm/vmm/vmm.h"

#define VMM_VA_MAX_NODES 128

struct vmm_va_node {
    vaddr_t start;
    size_t pages;
};

struct vmm_va {
    vaddr_t base;
    size_t total_pages;
    struct vmm_va_node free[VMM_VA_MAX_NODES];
    size_t free_count;
};

void vmm_va_init(struct vmm_va *va, vaddr_t base, size_t pages);
bool vmm_va_alloc(struct vmm_va *va, size_t pages, vaddr_t *out);
void vmm_va_free(struct vmm_va *va, vaddr_t addr, size_t pages);

#endif
