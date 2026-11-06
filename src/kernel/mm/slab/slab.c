#include "kernel/lib/string.h"
#include "slab.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "arch/x86_64/cpu/cpu.h"
#include "drivers/serial/serial.h"

__attribute__((noinline, used)) static void slab_layout_pad(void) {
    __asm__ volatile(
        ".rept 1024\n\t"
        "nop\n\t"
        ".endr\n\t"
    );
}

#define SLAB_MAG_SIZE 32
#define SLAB_MAX_EMPTY 8

struct slab {
    struct slab *next;
    struct slab *prev;
    struct kmem_cache *cache;
    void *freelist;
    uint32_t inuse;
    uint32_t total;
};

struct kmem_cache {
    const char *name;
    size_t obj_size;
    size_t obj_offset;
    uint32_t objs_per_slab;
    size_t pages_per_slab;
    size_t slab_bytes;
    spinlock_t lock;
    struct slab *partial;
    struct slab *empty;
    size_t empty_count;
    void *mag[MAX_CPUS][SLAB_MAG_SIZE];
    uint32_t mag_count[MAX_CPUS];
    struct kmem_cache *registry_next;
};

static struct kmem_cache *cache_registry = NULL;

static void list_push(struct slab **head, struct slab *s) {
    s->prev = NULL;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

static void list_remove(struct slab **head, struct slab *s) {
    if (s->prev) s->prev->next = s->next;
    else if (*head == s) *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = NULL;
    s->prev = NULL;
}

static struct slab *slab_of(struct kmem_cache *c, void *obj) {
    uintptr_t pa = (uintptr_t)obj - (uintptr_t)vmm_hhdm_offset;
    pa &= ~(uintptr_t)(c->slab_bytes - 1);
    return (struct slab *)(vmm_hhdm_offset + pa);
}

static struct slab *slab_create(struct kmem_cache *c) {
    paddr_t phys;
    if (!pmm_alloc(&phys, c->pages_per_slab)) return NULL;

    void *base = (void *)(vmm_hhdm_offset + phys);
    struct slab *s = (struct slab *)base;
    s->cache = c;
    s->total = c->objs_per_slab;
    s->inuse = 0;
    s->next = NULL;
    s->prev = NULL;

    uint8_t *p = (uint8_t *)base + c->obj_offset;
    void *fl = NULL;
    for (uint32_t i = 0; i < c->objs_per_slab; i++) {
        void *o = p + (size_t)i * c->obj_size;
        *(void **)o = fl;
        fl = o;
    }
    s->freelist = fl;
    return s;
}

static void slab_destroy(struct kmem_cache *c, struct slab *s) {
    paddr_t phys = (paddr_t)((uintptr_t)s - (uintptr_t)vmm_hhdm_offset);
    pmm_free(phys, c->pages_per_slab);
}

static bool slab_refill(struct kmem_cache *c, uint32_t cpu) {
    spin_lock(&c->lock);

    struct slab *s = c->partial;
    if (s) {
        list_remove(&c->partial, s);
    } else if (c->empty) {
        s = c->empty;
        list_remove(&c->empty, s);
        c->empty_count--;
    } else {
        s = slab_create(c);
        if (!s) {
            spin_unlock(&c->lock);
            return false;
        }
    }

    uint32_t moved = 0;
    while (moved < SLAB_MAG_SIZE && s->freelist) {
        void *o = s->freelist;
        s->freelist = *(void **)o;
        s->inuse++;
        c->mag[cpu][c->mag_count[cpu]++] = o;
        moved++;
    }

    if (s->inuse < s->total) list_push(&c->partial, s);

    spin_unlock(&c->lock);
    return moved > 0;
}

static void slab_put(struct kmem_cache *c, void *obj) {
    struct slab *s = slab_of(c, obj);
    bool was_full = (s->inuse == s->total);

    *(void **)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;

    if (was_full) list_push(&c->partial, s);

    if (s->inuse == 0) {
        if (!was_full) list_remove(&c->partial, s);
        if (c->empty_count < SLAB_MAX_EMPTY) {
            list_push(&c->empty, s);
            c->empty_count++;
        } else {
            slab_destroy(c, s);
        }
    }
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align) {
    if (size == 0) return NULL;

    struct kmem_cache *c = (struct kmem_cache *)kmalloc(sizeof(struct kmem_cache));
    if (!c) return NULL;
    memset(c, 0, sizeof(struct kmem_cache));

    c->name = name;

    size_t asz = align;
    if (asz < sizeof(void *)) asz = sizeof(void *);
    if (asz < 16) asz = 16;

    size_t obj = (size + asz - 1) & ~(asz - 1);
    if (obj < sizeof(void *)) obj = sizeof(void *);
    c->obj_size = obj;

    size_t overhead = (sizeof(struct slab) + asz - 1) & ~(asz - 1);

    size_t pages = 1;
    for (;;) {
        size_t bytes = pages * PAGE_SIZE;
        if (bytes > overhead) {
            size_t cnt = (bytes - overhead) / obj;
            if (cnt >= 8) break;
            if (cnt >= 1 && pages >= 64) break;
        }
        if (pages >= 65536) break;
        pages <<= 1;
    }

    c->pages_per_slab = pages;
    c->slab_bytes = pages * PAGE_SIZE;
    c->obj_offset = overhead;
    c->objs_per_slab = (uint32_t)((c->slab_bytes - overhead) / obj);
    if (c->objs_per_slab == 0) {
        kfree(c);
        return NULL;
    }

    c->partial = NULL;
    c->empty = NULL;
    c->empty_count = 0;
    for (int i = 0; i < MAX_CPUS; i++) c->mag_count[i] = 0;

    c->registry_next = cache_registry;
    cache_registry = c;
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *c) {
    if (!c) return NULL;

    preempt_disable();
    uint32_t cpu = cpu_id();

    if (c->mag_count[cpu]) {
        void *o = c->mag[cpu][--c->mag_count[cpu]];
        preempt_enable();
        return o;
    }

    if (!slab_refill(c, cpu)) {
        preempt_enable();
        return NULL;
    }

    void *o = c->mag[cpu][--c->mag_count[cpu]];
    preempt_enable();
    return o;
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
    if (!c || !obj) return;

    preempt_disable();
    uint32_t cpu = cpu_id();

    if (c->mag_count[cpu] < SLAB_MAG_SIZE) {
        c->mag[cpu][c->mag_count[cpu]++] = obj;
        preempt_enable();
        return;
    }

    spin_lock(&c->lock);
    uint32_t flush = SLAB_MAG_SIZE / 2;
    while (flush--) slab_put(c, c->mag[cpu][--c->mag_count[cpu]]);
    spin_unlock(&c->lock);

    c->mag[cpu][c->mag_count[cpu]++] = obj;
    preempt_enable();
}

size_t kmem_cache_obj_size(struct kmem_cache *c) {
    return c ? c->obj_size : 0;
}

size_t kmem_cache_pages_per_slab(struct kmem_cache *c) {
    return c ? c->pages_per_slab : 0;
}

void slab_init(void) {
    cache_registry = NULL;
    kprintf(PRINT_SERIAL, "Slab: initialized\n");
}
