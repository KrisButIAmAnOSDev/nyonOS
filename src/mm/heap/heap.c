#include "heap.h"
#include "../vmm/vmm.h"
#include "../pmm/pmm.h"
#include "io/serial/serial.h"

#define HEAP_VIRT_BASE 0xFFFFFFFFD0000000ULL
#define HEAP_MAX_PERCENT 50
#define HEAP_MAX_SEGMENTS 64
#define HEAP_MAGIC 0x6E796F6E
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))

struct block_header {
    size_t size;
    bool free;
    uint32_t magic;
    struct block_header *next;
    struct block_header *prev;
};

struct heap_segment {
    vaddr_t vaddr;
    paddr_t phys;
    size_t pages;
};

static struct block_header *heap_head = NULL;
static struct heap_segment segments[HEAP_MAX_SEGMENTS];
static size_t segment_count = 0;
static uint64_t heap_committed = 0;
static uint64_t heap_max_bytes = 0;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "cc", "memory");
}

static struct block_header *heap_tail(void) {
    struct block_header *b = heap_head;
    if (!b) return NULL;
    while (b->next) b = b->next;
    return b;
}

static void coalesce(struct block_header *b) {
    while (b->next && b->next->free) {
        b->size += sizeof(struct block_header) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size += sizeof(struct block_header) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

static void heap_trim(void) {
    while (segment_count > 0) {
        struct heap_segment *seg = &segments[segment_count - 1];
        struct block_header *tail = heap_tail();
        if (!tail || !tail->free) return;

        uint8_t *bstart = (uint8_t *)tail;
        uint8_t *bend = bstart + sizeof(struct block_header) + tail->size;
        vaddr_t seg_end = seg->vaddr + seg->pages * PAGE_SIZE;

        if (bstart != (uint8_t *)seg->vaddr) return;
        if (bend != (uint8_t *)seg_end) return;

        if (tail->prev) {
            tail->prev->next = NULL;
        } else {
            heap_head = NULL;
        }

        vmm_unmap(seg->vaddr, seg->pages);
        pmm_free(seg->phys, seg->pages);

        heap_committed -= seg->pages * PAGE_SIZE;
        segment_count--;
    }
}

static bool heap_grow(size_t min_extra) {
    if (min_extra == 0) return false;

    size_t needed = ALIGN_UP(min_extra, PAGE_SIZE);
    size_t pages = needed / PAGE_SIZE;
    if (pages == 0) return false;

    if (heap_committed + needed > heap_max_bytes) return false;
    if (segment_count >= HEAP_MAX_SEGMENTS) return false;

    paddr_t phys = pmm_alloc(pages);
    if (!phys) return false;

    vaddr_t vaddr = HEAP_VIRT_BASE + heap_committed;
    if (!vmm_map(vaddr, phys, pages, VMM_DEFAULT_FLAGS)) {
        pmm_free(phys, pages);
        return false;
    }

    segments[segment_count].vaddr = vaddr;
    segments[segment_count].phys = phys;
    segments[segment_count].pages = pages;
    segment_count++;

    struct block_header *tail = heap_tail();

    struct block_header *new_block = (struct block_header *)vaddr;
    new_block->size = needed - sizeof(struct block_header);
    new_block->free = true;
    new_block->magic = HEAP_MAGIC;
    new_block->next = NULL;
    new_block->prev = tail;

    if (tail) {
        tail->next = new_block;
    } else {
        heap_head = new_block;
    }

    heap_committed += needed;
    return true;
}

void heap_init(void) {
    heap_head = NULL;
    segment_count = 0;
    heap_committed = 0;

    uint64_t total_bytes = (uint64_t)pmm_total_pages() * PAGE_SIZE;
    heap_max_bytes = (total_bytes * HEAP_MAX_PERCENT) / 100;

    if (!heap_grow(PAGE_SIZE * 4)) {
        serial_print("Heap: initial grow failed!\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_print("Heap: initialized\n");
}

static void split_block(struct block_header *b, size_t size) {
    size_t remaining = b->size - size;
    if (remaining <= sizeof(struct block_header)) return;

    uint8_t *data = (uint8_t *)(b + 1);
    struct block_header *new_block = (struct block_header *)(data + size);
    new_block->size = remaining - sizeof(struct block_header);
    new_block->free = true;
    new_block->magic = HEAP_MAGIC;
    new_block->next = b->next;
    new_block->prev = b;

    if (b->next) b->next->prev = new_block;
    b->next = new_block;
    b->size = size;
}

static void *kmalloc_locked(size_t size) {
    struct block_header *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = false;
            return (void *)(b + 1);
        }
        b = b->next;
    }

    if (!heap_grow(size + sizeof(struct block_header))) return NULL;

    b = heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = false;
            return (void *)(b + 1);
        }
        b = b->next;
    }
    return NULL;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (size > SIZE_MAX - 16 - sizeof(struct block_header)) return NULL;

    size = ALIGN_UP(size, 16);

    uint64_t flags = irq_save();
    void *result = kmalloc_locked(size);
    irq_restore(flags);

    return result;
}

static void kfree_locked(void *ptr) {
    uint64_t addr = (uint64_t)ptr;

    if (addr < HEAP_VIRT_BASE + sizeof(struct block_header) ||
        addr >= HEAP_VIRT_BASE + heap_committed) {
        serial_print("kfree: pointer outside heap\n");
        return;
    }

    struct block_header *b = (struct block_header *)ptr - 1;
    if (b->magic != HEAP_MAGIC) {
        serial_print("kfree: not a block boundary\n");
        return;
    }
    if (b->free) {
        serial_print("kfree: double free\n");
        return;
    }

    b->free = true;
    coalesce(b);
    heap_trim();
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t flags = irq_save();
    kfree_locked(ptr);
    irq_restore(flags);
}

uint64_t heap_committed_bytes(void) {
    return heap_committed;
}
