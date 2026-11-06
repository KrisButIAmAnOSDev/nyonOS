#include "vmm_va.h"

void vmm_va_init(struct vmm_va *va, vaddr_t base, size_t pages) {
    va->base = base;
    va->total_pages = pages;
    va->free_count = pages ? 1 : 0;
    va->free[0].start = base;
    va->free[0].pages = pages;
}

bool vmm_va_alloc(struct vmm_va *va, size_t pages, vaddr_t *out) {
    if (!va || !out || pages == 0) return false;

    for (size_t i = 0; i < va->free_count; i++) {
        if (va->free[i].pages < pages) continue;

        *out = va->free[i].start;
        va->free[i].start += pages * PAGE_SIZE;
        va->free[i].pages -= pages;

        if (va->free[i].pages == 0) {
            for (size_t j = i + 1; j < va->free_count; j++)
                va->free[j - 1] = va->free[j];
            va->free_count--;
        }
        return true;
    }
    return false;
}

void vmm_va_free(struct vmm_va *va, vaddr_t addr, size_t pages) {
    if (!va || pages == 0) return;
    if (va->free_count >= VMM_VA_MAX_NODES) return;

    size_t i = 0;
    while (i < va->free_count && va->free[i].start < addr) i++;

    for (size_t j = va->free_count; j > i; j--)
        va->free[j] = va->free[j - 1];
    va->free[i].start = addr;
    va->free[i].pages = pages;
    va->free_count++;

    if (i + 1 < va->free_count &&
        va->free[i].start + va->free[i].pages * PAGE_SIZE == va->free[i + 1].start) {
        va->free[i].pages += va->free[i + 1].pages;
        for (size_t j = i + 2; j < va->free_count; j++)
            va->free[j - 1] = va->free[j];
        va->free_count--;
    }

    if (i > 0 &&
        va->free[i - 1].start + va->free[i - 1].pages * PAGE_SIZE == va->free[i].start) {
        va->free[i - 1].pages += va->free[i].pages;
        for (size_t j = i + 1; j < va->free_count; j++)
            va->free[j - 1] = va->free[j];
        va->free_count--;
    }
}
