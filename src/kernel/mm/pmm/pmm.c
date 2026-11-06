#include "pmm.h"
#include "kernel/kprintf/kprintf.h"
#include "drivers/serial/serial.h"
#include "kernel/sync/sync.h"

#include "kernel/lib/string.h"
#include "arch/x86_64/cpu/cpu.h"

#define MAX_REGIONS 128
#define PMM_MAX_ORDER 14
#define PMM_RESERVED 0xFFu
#define PMM_STATE_FREE(order) ((uint8_t)(order))
#define PMM_STATE_USED(order) ((uint8_t)(0x80u | (order)))

struct pmm_free_block {
    struct pmm_free_block *next;
    struct pmm_free_block *prev;
};

static spinlock_t pmm_lock = SPINLOCK_INIT;

static uint8_t *pmm_state = NULL;
static uint8_t *pmm_share = NULL;
static struct pmm_free_block *pmm_free_lists[PMM_MAX_ORDER + 1];
static size_t total_pages = 0;
static size_t free_pages = 0;

static uint64_t pmm_hhdm_offset = 0;

struct pmm_region pmm_regions[MAX_REGIONS];
size_t pmm_region_count = 0;

static inline size_t page_index(paddr_t addr) { return (size_t)(addr / PAGE_SIZE); }
static inline paddr_t page_addr(size_t page) { return (paddr_t)page * PAGE_SIZE; }

static struct pmm_free_block *block_at(size_t page) {
    return (struct pmm_free_block *)(pmm_hhdm_offset + page_addr(page));
}

static void free_list_add(unsigned order, struct pmm_free_block *block) {
    block->prev = NULL;
    block->next = pmm_free_lists[order];
    if (block->next) block->next->prev = block;
    pmm_free_lists[order] = block;
}

static void free_list_remove(unsigned order, struct pmm_free_block *block) {
    if (block->prev) block->prev->next = block->next;
    else pmm_free_lists[order] = block->next;
    if (block->next) block->next->prev = block->prev;
    block->next = NULL;
    block->prev = NULL;
}

static unsigned order_for_pages(size_t pages) {
    if (pages <= 1) return 0;
    return (unsigned)(64 - __builtin_clzll((unsigned long long)(pages - 1)));
}

static void state_fill(size_t page, size_t pages, uint8_t value) {
    memset(pmm_state + page, value, pages);
}

#define PMM_CACHE_SIZE 32
#define PMM_BUFFERED 0x7Eu

struct pmm_cache {
    paddr_t pages[PMM_CACHE_SIZE];
    uint32_t count;
};

static struct pmm_cache pmm_caches[MAX_CPUS];

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

void pmm_share_ref(paddr_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;
    size_t page = addr / PAGE_SIZE;
    if (page >= total_pages) return;
    lock_acquire(LOCK_PMM, &pmm_lock);
    if (pmm_share[page] != 0xFF) pmm_share[page]++;
    lock_release(LOCK_PMM, &pmm_lock);
}

bool pmm_unshare_ref(paddr_t addr) {
    if (addr & (PAGE_SIZE - 1)) return false;
    size_t page = addr / PAGE_SIZE;
    if (page >= total_pages) return false;
    lock_acquire(LOCK_PMM, &pmm_lock);
    if (pmm_share[page] == 0) {
        lock_release(LOCK_PMM, &pmm_lock);
        return false;
    }
    bool last = (--pmm_share[page] == 0);
    lock_release(LOCK_PMM, &pmm_lock);
    return last;
}

static size_t buddy_alloc(unsigned order) {
    unsigned found = order;
    while (found <= PMM_MAX_ORDER && !pmm_free_lists[found]) found++;
    if (found > PMM_MAX_ORDER) return SIZE_MAX;

    struct pmm_free_block *block = pmm_free_lists[found];
    free_list_remove(found, block);
    size_t page = page_index((paddr_t)((uint64_t)block - pmm_hhdm_offset));

    while (found > order) {
        found--;
        size_t buddy = page + ((size_t)1 << found);
        state_fill(buddy, (size_t)1 << found, PMM_STATE_FREE(found));
        free_list_add(found, block_at(buddy));
    }

    state_fill(page, (size_t)1 << order, PMM_STATE_USED(order));
    return page;
}

static void buddy_free(size_t page, unsigned order) {
    while (order < PMM_MAX_ORDER) {
        size_t buddy = page ^ ((size_t)1 << order);
        if (buddy + ((size_t)1 << order) > total_pages) break;
        if (pmm_state[buddy] != PMM_STATE_FREE(order)) break;
        free_list_remove(order, block_at(buddy));
        if (buddy < page) page = buddy;
        order++;
    }

    state_fill(page, (size_t)1 << order, PMM_STATE_FREE(order));
    free_list_add(order, block_at(page));
}

static paddr_t memmap_carve(struct limine_memmap_response *memmap, size_t size) {
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length < size) continue;
        paddr_t base = entry->base;
        entry->base += size;
        entry->length -= size;
        return base;
    }
    return 0;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    pmm_hhdm_offset = hhdm_offset;

    paddr_t max_addr = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        paddr_t end = entry->base + entry->length;
        if (end > max_addr) max_addr = end;
    }

    total_pages = (size_t)((max_addr + PAGE_SIZE - 1) / PAGE_SIZE);

    paddr_t share_phys = memmap_carve(memmap, total_pages);
    paddr_t state_phys = memmap_carve(memmap, total_pages);
    if (!share_phys || !state_phys) {
        kprintf(PRINT_SERIAL, "PMM: no space for metadata\n");
        for (;;) __asm__ volatile("hlt");
    }

    pmm_share = (uint8_t *)(pmm_hhdm_offset + share_phys);
    pmm_state = (uint8_t *)(pmm_hhdm_offset + state_phys);
    memset(pmm_share, 0, total_pages);
    memset(pmm_state, PMM_RESERVED, total_pages);
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) pmm_free_lists[o] = NULL;

    free_pages = 0;
    pmm_region_count = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length < PAGE_SIZE) continue;

        paddr_t base = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (base >= entry->base + entry->length) continue;
        size_t pages = (size_t)((entry->base + entry->length - base) / PAGE_SIZE);

        if (pmm_region_count < MAX_REGIONS)
            pmm_regions[pmm_region_count++] = (struct pmm_region){ .base = base, .pages = pages };

        size_t start_page = page_index(base);
        for (size_t j = 0; j < pages; j++) buddy_free(start_page + j, 0);
        free_pages += pages;
    }

    kprintf(PRINT_SERIAL, "PMM: Init done. Total: %zu pages (%zu free)\n",
            (size_t)total_pages, (size_t)free_pages);
}

static bool pmm_alloc_cached(paddr_t *out) {
    uint64_t flags = irq_save();
    struct pmm_cache *cache = &pmm_caches[cpu_id()];

    if (cache->count) {
        size_t page = cache->pages[--cache->count];
        pmm_state[page] = PMM_STATE_USED(0);
        __atomic_sub_fetch(&free_pages, 1, __ATOMIC_RELAXED);
        irq_restore(flags);
        *out = page_addr(page);
        return true;
    }

    lock_acquire(LOCK_PMM, &pmm_lock);
    while (cache->count < PMM_CACHE_SIZE) {
        size_t page = buddy_alloc(0);
        if (page == SIZE_MAX) break;
        pmm_state[page] = PMM_BUFFERED;
        cache->pages[cache->count++] = page;
    }
    bool ok = cache->count > 0;
    size_t page = 0;
    if (ok) {
        page = cache->pages[--cache->count];
        pmm_state[page] = PMM_STATE_USED(0);
        __atomic_sub_fetch(&free_pages, 1, __ATOMIC_RELAXED);
    }
    lock_release(LOCK_PMM, &pmm_lock);
    irq_restore(flags);

    if (!ok) return false;
    *out = page_addr(page);
    return true;
}

static void pmm_free_cached(size_t page) {
    uint64_t flags = irq_save();
    struct pmm_cache *cache = &pmm_caches[cpu_id()];

    if (cache->count < PMM_CACHE_SIZE) {
        pmm_state[page] = PMM_BUFFERED;
        cache->pages[cache->count++] = page;
        __atomic_add_fetch(&free_pages, 1, __ATOMIC_RELAXED);
        irq_restore(flags);
        return;
    }

    lock_acquire(LOCK_PMM, &pmm_lock);
    while (cache->count > PMM_CACHE_SIZE / 2) {
        size_t p = cache->pages[--cache->count];
        buddy_free(p, 0);
    }
    pmm_state[page] = PMM_BUFFERED;
    cache->pages[cache->count++] = page;
    __atomic_add_fetch(&free_pages, 1, __ATOMIC_RELAXED);
    lock_release(LOCK_PMM, &pmm_lock);
    irq_restore(flags);
}

bool pmm_alloc(paddr_t *out, size_t pages) {
    if (!out || pages == 0) return false;

    unsigned order = order_for_pages(pages);
    if (order > PMM_MAX_ORDER) return false;

    if (order == 0) return pmm_alloc_cached(out);

    lock_acquire(LOCK_PMM, &pmm_lock);

    if (pages > __atomic_load_n(&free_pages, __ATOMIC_RELAXED)) {
        lock_release(LOCK_PMM, &pmm_lock);
        return false;
    }

    size_t page = buddy_alloc(order);
    if (page != SIZE_MAX)
        __atomic_sub_fetch(&free_pages, (size_t)1 << order, __ATOMIC_RELAXED);
    lock_release(LOCK_PMM, &pmm_lock);

    if (page == SIZE_MAX) return false;
    *out = page_addr(page);
    return true;
}

static bool pmm_page_managed(size_t page) {
    for (size_t i = 0; i < pmm_region_count; i++) {
        size_t base = pmm_regions[i].base / PAGE_SIZE;
        if (page >= base && page < base + pmm_regions[i].pages) return true;
    }
    return false;
}

void pmm_free(paddr_t addr, size_t pages) {
    if (pages == 0) return;

    if (addr & (PAGE_SIZE - 1)) {
        kprintf(PRINT_SERIAL, "PMM: free of unaligned address\n");
        return;
    }

    size_t start = page_index(addr);
    if (start >= total_pages || pages > total_pages - start) {
        kprintf(PRINT_SERIAL, "PMM: free out of range\n");
        return;
    }

    if (!pmm_page_managed(start)) {
        kprintf(PRINT_SERIAL, "PMM: free of page outside managed regions\n");
        return;
    }

    uint64_t flags = irq_save();
    uint8_t state = pmm_state[start];
    irq_restore(flags);
    if (!(state & 0x80u)) return;

    unsigned order = state & 0x7Fu;
    if (order > PMM_MAX_ORDER) {
        kprintf(PRINT_SERIAL, "PMM: free of corrupt block state\n");
        return;
    }

    if (order == 0) {
        pmm_free_cached(start);
        return;
    }

    size_t base = start & ~(((size_t)1 << order) - 1);

    lock_acquire(LOCK_PMM, &pmm_lock);
    if (pmm_state[base] == PMM_STATE_USED(order)) {
        buddy_free(base, order);
        __atomic_add_fetch(&free_pages, (size_t)1 << order, __ATOMIC_RELAXED);
    }
    lock_release(LOCK_PMM, &pmm_lock);
}

size_t pmm_total_pages(void) { return total_pages; }

bool pmm_share_ready(void) { return pmm_share != NULL; }
size_t pmm_free_pages(void) { return __atomic_load_n(&free_pages, __ATOMIC_RELAXED); }
