#include "pmm.h"
#include "io/serial/serial.h"

#define MAX_REGIONS 128

static uint64_t *pmm_bitmap = NULL;
static size_t bitmap_pages = 0;
static size_t total_pages = 0;
static size_t free_pages = 0;

static uint64_t pmm_hhdm_offset = 0;

struct pmm_region pmm_regions[MAX_REGIONS];
size_t pmm_region_count = 0;

static void bitmap_set(size_t bit) {
    pmm_bitmap[bit / 64] |= (1ULL << (bit % 64));
}

static void bitmap_clear(size_t bit) {
    pmm_bitmap[bit / 64] &= ~(1ULL << (bit % 64));
}

static bool bitmap_test(size_t bit) {
    return (pmm_bitmap[bit / 64] >> (bit % 64)) & 1ULL;
}

static size_t bitmap_find_free(size_t count) {
    size_t consecutive = 0;
    size_t start = 0;
    
    for (size_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start = i;
            consecutive++;
            if (consecutive == count) return start;
        } else {
            consecutive = 0;
        }
    }
    return SIZE_MAX;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    pmm_hhdm_offset = hhdm_offset;
    paddr_t max_addr = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            paddr_t end = entry->base + entry->length;
            if (end > max_addr) max_addr = end;
        }
    }
    
    total_pages = (max_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap_pages = (total_pages + 63) / 64;
    size_t bitmap_size = bitmap_pages * 8;
    
    paddr_t bitmap_phys = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap_phys = entry->base;
            entry->base += bitmap_size;
            entry->length -= bitmap_size;
            break;
        }
    }
    
    if (!bitmap_phys) {
        serial_print("PMM: No space for bitmap!\n");
        for (;;) __asm__ volatile("hlt");
    }
    
    pmm_bitmap = (uint64_t *)(pmm_hhdm_offset + bitmap_phys);
    
    for (size_t i = 0; i < bitmap_pages; i++) {
        pmm_bitmap[i] = ~0ULL;
    }
    
    pmm_region_count = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= PAGE_SIZE) {
            paddr_t base = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            size_t pages = (entry->base + entry->length - base) / PAGE_SIZE;
            
            if (pages > 0 && pmm_region_count < MAX_REGIONS) {
                pmm_regions[pmm_region_count++] = (struct pmm_region){
                    .base = base, .pages = pages, .used = false
                };
                
                size_t start_page = base / PAGE_SIZE;
                for (size_t j = 0; j < pages; j++) {
                    bitmap_clear(start_page + j);
                }
                free_pages += pages;
            }
        }
    }
    
    serial_print("PMM: Init done. Total: ");
    char buf[24];
    size_t t = total_pages;
    size_t idx = 0;
    if (t == 0) buf[idx++] = '0';
    else {
        char rev[24]; size_t j = 0;
        while (t) { rev[j++] = '0' + (t % 10); t /= 10; }
        while (j > 0) { buf[idx++] = rev[--j]; }
    }
    buf[idx] = 0;
    serial_print(buf);
    serial_print(" pages (");
    t = free_pages; idx = 0;
    if (t == 0) buf[idx++] = '0';
    else {
        char rev[24]; size_t j = 0;
        while (t) { rev[j++] = '0' + (t % 10); t /= 10; }
        while (j > 0) { buf[idx++] = rev[--j]; }
    }
    buf[idx] = 0;
    serial_print(buf);
    serial_print(" free)\n");
}

paddr_t pmm_alloc(size_t pages) {
    if (pages == 0) return 0;
    if (pages > free_pages) return 0;
    
    size_t start = bitmap_find_free(pages);
    if (start == SIZE_MAX) return 0;
    
    for (size_t i = 0; i < pages; i++) {
        bitmap_set(start + i);
    }
    free_pages -= pages;
    return start * PAGE_SIZE;
}

void pmm_free(paddr_t addr, size_t pages) {
    if (pages == 0) return;
    size_t start = addr / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        if (bitmap_test(start + i)) {
            bitmap_clear(start + i);
            free_pages++;
        }
    }
}

size_t pmm_total_pages(void) { return total_pages; }
size_t pmm_free_pages(void) { return free_pages; }
size_t pmm_used_pages(void) { return total_pages - free_pages; }
