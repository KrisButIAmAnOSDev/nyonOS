#include "kernel/lib/string.h"
#include "heap.h"
#include "kernel/mm/vmm/vmm_va.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/sync/sync.h"
#include "drivers/serial/serial.h"

#define HEAP_VIRT_BASE 0xFFFFFFFFD0000000ULL
#define HEAP_MAX_PERCENT 50
#define HEAP_MAX_SEGMENTS 256
#define HEAP_GROW_MIN_PAGES 32
#define HEAP_MAGIC 0x6E796F6E
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))

typedef enum {
    KFREE_OK = 0,
    KFREE_OUTSIDE,
    KFREE_NOT_BOUNDARY,
    KFREE_DOUBLE,
} kfree_result_t;

static spinlock_t heap_lock = SPINLOCK_INIT;

struct block_header {
    size_t size;
    bool free;
    uint32_t magic;
    uint32_t seg;
    struct block_header *next;
    struct block_header *prev;
    struct block_header *fnext;
    struct block_header *fprev;
    uint64_t align_pad;
};

_Static_assert(sizeof(struct block_header) % 16 == 0, "heap header must keep 16-byte alignment");

struct heap_segment {
    vaddr_t vaddr;
    paddr_t phys;
    size_t pages;
};

static struct block_header *heap_head = NULL;
static struct block_header *heap_tail_block = NULL;
static struct heap_segment segments[HEAP_MAX_SEGMENTS];
static size_t segment_count = 0;
static uint64_t heap_max_bytes = 0;
static struct vmm_va heap_va;

#define HEAP_BINS 64
static struct block_header *heap_bins[HEAP_BINS];

static unsigned bin_for(size_t size) {
    if (size < 16) size = 16;
    unsigned b = (unsigned)(63 - __builtin_clzll((unsigned long long)size));
    return b < HEAP_BINS ? b : HEAP_BINS - 1;
}

static void bin_insert(struct block_header *b) {
    unsigned i = bin_for(b->size);
    b->fprev = NULL;
    b->fnext = heap_bins[i];
    if (b->fnext) b->fnext->fprev = b;
    heap_bins[i] = b;
}

static void bin_remove(struct block_header *b) {
    unsigned i = bin_for(b->size);
    if (b->fprev) b->fprev->fnext = b->fnext;
    else if (heap_bins[i] == b) heap_bins[i] = b->fnext;
    if (b->fnext) b->fnext->fprev = b->fprev;
    b->fnext = NULL;
    b->fprev = NULL;
}

static struct block_header *coalesce(struct block_header *b) {
    while (b->next && b->next->free && b->next->seg == b->seg) {
        struct block_header *n = b->next;
        bin_remove(n);
        b->size += sizeof(struct block_header) + n->size;
        b->next = n->next;
        if (b->next) b->next->prev = b;
        if (n == heap_tail_block) heap_tail_block = b;
    }
    while (b->prev && b->prev->free && b->prev->seg == b->seg) {
        struct block_header *p = b->prev;
        bin_remove(p);
        p->size += sizeof(struct block_header) + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
        if (b == heap_tail_block) heap_tail_block = p;
        b = p;
    }
    return b;
}

static void heap_trim(void) {
    while (segment_count > 0) {
        struct heap_segment *seg = &segments[segment_count - 1];
        struct block_header *tail = heap_tail_block;
        if (!tail || !tail->free) return;

        vaddr_t seg_end = seg->vaddr + seg->pages * PAGE_SIZE;

        uint8_t *bstart = (uint8_t *)tail;
        uint8_t *bend = bstart + sizeof(struct block_header) + tail->size;

        if (bend != (uint8_t *)seg_end) return;
        if (bstart != (uint8_t *)seg->vaddr) return;
        if (tail->seg != (uint32_t)(segment_count - 1)) return;

        bin_remove(tail);

        if (tail->prev) {
            tail->prev->next = NULL;
            heap_tail_block = tail->prev;
        } else {
            heap_head = NULL;
            heap_tail_block = NULL;
        }

        vmm_unmap(seg->vaddr, seg->pages);
        pmm_free(seg->phys, seg->pages);

        vmm_va_free(&heap_va, seg->vaddr, seg->pages);
        segment_count--;
    }
}

static bool heap_grow(size_t min_extra) {
    if (min_extra == 0) return false;

    size_t needed = ALIGN_UP(min_extra, PAGE_SIZE);

    size_t chunk = needed;
    if (chunk < HEAP_GROW_MIN_PAGES * PAGE_SIZE) chunk = HEAP_GROW_MIN_PAGES * PAGE_SIZE;

    size_t pages = chunk / PAGE_SIZE;
    if (pages == 0) return false;

    if (segment_count >= HEAP_MAX_SEGMENTS) return false;

    vaddr_t vaddr;
    if (!vmm_va_alloc(&heap_va, pages, &vaddr)) return false;

    paddr_t phys;
    if (!pmm_alloc(&phys, pages)) {
        vmm_va_free(&heap_va, vaddr, pages);
        return false;
    }

    if (!vmm_map(vaddr, phys, pages, VMM_DEFAULT_FLAGS)) {
        pmm_free(phys, pages);
        vmm_va_free(&heap_va, vaddr, pages);
        return false;
    }

    segments[segment_count].vaddr = vaddr;
    segments[segment_count].phys = phys;
    segments[segment_count].pages = pages;
    segment_count++;

    struct block_header *tail = heap_tail_block;

    struct block_header *new_block = (struct block_header *)vaddr;
    new_block->size = chunk - sizeof(struct block_header);
    new_block->free = true;
    new_block->magic = HEAP_MAGIC;
    new_block->seg = (uint32_t)(segment_count - 1);
    new_block->next = NULL;
    new_block->prev = tail;

    if (tail) {
        tail->next = new_block;
    } else {
        heap_head = new_block;
    }
    heap_tail_block = new_block;

    bin_insert(new_block);

    return true;
}

void heap_init(void) {
    heap_head = NULL;
    heap_tail_block = NULL;
    segment_count = 0;
    memset(heap_bins, 0, sizeof heap_bins);

    uint64_t total_bytes = (uint64_t)pmm_total_pages() * PAGE_SIZE;
    heap_max_bytes = (total_bytes * HEAP_MAX_PERCENT) / 100;

    vmm_va_init(&heap_va, HEAP_VIRT_BASE, heap_max_bytes / PAGE_SIZE);

    if (!heap_grow(PAGE_SIZE * 4)) {
        kprintf(PRINT_SERIAL, "Heap: initial grow failed!\n");
        for (;;) __asm__ volatile("hlt");
    }
    kprintf(PRINT_SERIAL, "Heap: initialized\n");
}

static void split_block(struct block_header *b, size_t size) {
    if (b->size < size) return;
    size_t remaining = b->size - size;
    if (remaining < sizeof(struct block_header) + 16) return;

    uint8_t *data = (uint8_t *)(b + 1);
    struct block_header *new_block = (struct block_header *)(data + size);
    new_block->size = remaining - sizeof(struct block_header);
    new_block->free = true;
    new_block->magic = HEAP_MAGIC;
    new_block->seg = b->seg;
    new_block->next = b->next;
    new_block->prev = b;

    if (b->next) b->next->prev = new_block;
    b->next = new_block;
    b->size = size;
    if (b == heap_tail_block) heap_tail_block = new_block;

    bin_insert(new_block);
}

static void *kmalloc_locked(size_t size) {
    unsigned start = bin_for(size);

    for (;;) {
        for (unsigned i = start; i < HEAP_BINS; i++) {
            struct block_header *b = heap_bins[i];
            while (b && b->size < size) b = b->fnext;
            if (!b) continue;
            bin_remove(b);
            split_block(b, size);
            b->free = false;
            return (void *)(b + 1);
        }
        if (!heap_grow(size + sizeof(struct block_header))) return NULL;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (size > SIZE_MAX - 16 - sizeof(struct block_header)) return NULL;

    size = ALIGN_UP(size, 16);

    lock_acquire(LOCK_HEAP, &heap_lock);
    void *result = kmalloc_locked(size);
    lock_release(LOCK_HEAP, &heap_lock);

    return result;
}

static bool heap_contains(vaddr_t addr) {
    for (size_t i = 0; i < segment_count; i++) {
        vaddr_t s = segments[i].vaddr;
        vaddr_t e = s + segments[i].pages * PAGE_SIZE;
        if (addr >= s && addr < e) return true;
    }
    return false;
}

static kfree_result_t kfree_locked(void *ptr) {
    uint64_t addr = (uint64_t)ptr;

    if (addr < sizeof(struct block_header)) return KFREE_OUTSIDE;
    vaddr_t hdr = (vaddr_t)(addr - sizeof(struct block_header));
    if (!heap_contains(hdr)) return KFREE_OUTSIDE;

    struct block_header *b = (struct block_header *)hdr;
    if (b->magic != HEAP_MAGIC) {
        return KFREE_NOT_BOUNDARY;
    }
    if (b->free) {
        return KFREE_DOUBLE;
    }

    b->free = true;
    b = coalesce(b);
    bin_insert(b);
    heap_trim();
    return KFREE_OK;
}

void kfree(void *ptr) {
    if (!ptr) return;

    lock_acquire(LOCK_HEAP, &heap_lock);
    kfree_result_t r = kfree_locked(ptr);
    lock_release(LOCK_HEAP, &heap_lock);

    switch (r) {
        case KFREE_OUTSIDE:      kprintf(PRINT_SERIAL, "kfree: pointer outside heap\n"); break;
        case KFREE_NOT_BOUNDARY: kprintf(PRINT_SERIAL, "kfree: not a block boundary\n"); break;
        case KFREE_DOUBLE:       kprintf(PRINT_SERIAL, "kfree: double free\n"); break;
        case KFREE_OK: break;
    }
}

static size_t kalloc_size_locked(void *ptr) {
    struct block_header *b = (struct block_header *)ptr - 1;
    if (b->magic != HEAP_MAGIC || b->free) return 0;
    return b->size;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (!p) return NULL;
    memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return kzalloc(n * size);
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    if (size > SIZE_MAX - 16 - sizeof(struct block_header)) return NULL;

    size = ALIGN_UP(size, 16);

    lock_acquire(LOCK_HEAP, &heap_lock);
    size_t old = kalloc_size_locked(ptr);
    if (old == 0) {
        lock_release(LOCK_HEAP, &heap_lock);
        return NULL;
    }
    if (old >= size) {
        split_block((struct block_header *)ptr - 1, size);
        lock_release(LOCK_HEAP, &heap_lock);
        return ptr;
    }
    void *np = kmalloc_locked(size);
    if (!np) {
        lock_release(LOCK_HEAP, &heap_lock);
        return NULL;
    }
    memcpy(np, ptr, old);
    kfree_locked(ptr);
    lock_release(LOCK_HEAP, &heap_lock);
    return np;
}

