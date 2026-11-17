#include "kernel/mm/pagecache/pcache.h"

#include "kernel/kprintf/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/sync/preempt.h"
#include "kernel/sync/sync.h"

static struct pcache_file pcache_files[PCACHE_MAX_FILES];
static struct pcache_map pcache_maps[PCACHE_MAX_MAPS];
static spinlock_t pcache_lock = SPINLOCK_INIT;
static uint64_t pcache_clock;

static void pcache_release_arrays(paddr_t *phys, uint8_t *dirty, uint32_t npages) {
    if (!phys) {
        if (dirty) kfree(dirty);
        return;
    }
    for (uint32_t i = 0; i < npages; i++)
        if (phys[i]) pmm_free(phys[i], 1);
    kfree(phys);
    if (dirty) kfree(dirty);
}

void pcache_init(void) {
    memset(pcache_files, 0, sizeof pcache_files);
    memset(pcache_maps, 0, sizeof pcache_maps);
}


static bool pcache_load(struct pcache_file *c) {
    if (c->loaded) return true;

    uint32_t npages = (uint32_t)((c->size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!npages) { c->loaded = true; return true; }

    c->phys = kmalloc(sizeof(paddr_t) * npages);
    c->dirty = kmalloc(npages);
    if (!c->phys || !c->dirty) {
        if (c->phys) kfree(c->phys);
        if (c->dirty) kfree(c->dirty);
        c->phys = NULL;
        c->dirty = NULL;
        return false;
    }
    memset(c->phys, 0, sizeof(paddr_t) * npages);
    memset(c->dirty, 0, npages);
    c->npages = npages;

    for (uint32_t i = 0; i < npages; i++) {
        paddr_t p;
        { pmm_set_tag(PMM_TAG_CACHE); bool __g = pmm_alloc(&p, 1); pmm_set_tag(PMM_TAG_NONE); if (!__g) {
            for (uint32_t j = 0; j < i; j++) pmm_free(c->phys[j], 1);
            kfree(c->phys);
            kfree(c->dirty);
            c->phys = NULL;
            c->dirty = NULL;
            c->npages = 0;
            return false;
        }}
        c->phys[i] = p;
        uint8_t *dst = (uint8_t *)(vmm_hhdm_offset + p);
        memset(dst, 0, PAGE_SIZE);
        uint64_t off = (uint64_t)i * PAGE_SIZE;
        uint64_t left = c->size > off ? c->size - off : 0;
        if (left) {
            uint32_t chunk = left > PAGE_SIZE ? PAGE_SIZE : (uint32_t)left;
            if (!fs_node_read(&c->node, (uint32_t)off, dst, chunk)) {
                for (uint32_t j = 0; j <= i; j++) pmm_free(c->phys[j], 1);
                kfree(c->phys);
                kfree(c->dirty);
                c->phys = NULL;
                c->dirty = NULL;
                c->npages = 0;
                return false;
            }
        }
    }

    c->loaded = true;
    return true;
}

struct pcache_file *pcache_acquire(fs_node_t *node) {
    if (!node || node->is_dir || !node->ops || !node->ops->read) return NULL;
    uint64_t key = node->cache_key;
    if (!key) return NULL;

    lock_acquire(LOCK_PCACHE, &pcache_lock);

    struct pcache_file *victim = NULL;
    for (uint32_t i = 0; i < PCACHE_MAX_FILES; i++) {
        if (pcache_files[i].used && pcache_files[i].key == key) {
            struct pcache_file *c = &pcache_files[i];
            c->users++;
            c->stamp = ++pcache_clock;
            lock_release(LOCK_PCACHE, &pcache_lock);
            if (!pcache_load(c)) {
                pcache_release(c);
                return NULL;
            }
            return c;
        }
        if (!pcache_files[i].used) { victim = &pcache_files[i]; break; }
        if (!victim || pcache_files[i].stamp < victim->stamp) {
            if (!pcache_files[i].users && !pcache_files[i].busy) victim = &pcache_files[i];
        }
    }

    if (!victim) {
        lock_release(LOCK_PCACHE, &pcache_lock);
        return NULL;
    }

    if (victim->used) {
        lock_release(LOCK_PCACHE, &pcache_lock);
        pcache_writeback(victim);
        lock_acquire(LOCK_PCACHE, &pcache_lock);
        if (victim->users || victim->busy) {
            lock_release(LOCK_PCACHE, &pcache_lock);
            return NULL;
        }
        paddr_t *vphys = victim->phys;
        uint8_t *vdirty = victim->dirty;
        uint32_t vnpages = victim->npages;
        victim->phys = NULL;
        victim->dirty = NULL;
        victim->npages = 0;
        lock_release(LOCK_PCACHE, &pcache_lock);
        pcache_release_arrays(vphys, vdirty, vnpages);
        lock_acquire(LOCK_PCACHE, &pcache_lock);
    }

    victim->used = true;
    victim->key = key;
    victim->size = node->size;
    victim->node = *node;
    victim->users = 1;
    victim->stamp = ++pcache_clock;
    victim->loaded = false;

    lock_release(LOCK_PCACHE, &pcache_lock);

    if (!pcache_load(victim)) {
        pcache_release(victim);
        return NULL;
    }
    return victim;
}

bool pcache_writeback(struct pcache_file *c) {
    if (!c || !c->used) return false;

    for (uint32_t i = 0; i < c->npages; i++) {
        lock_acquire(LOCK_PCACHE, &pcache_lock);
        if (!c->dirty || !c->dirty[i] || !c->phys || !c->phys[i]) {
            lock_release(LOCK_PCACHE, &pcache_lock);
            continue;
        }
        c->busy++;
        uint64_t off = (uint64_t)i * PAGE_SIZE;
        uint64_t left = c->size > off ? c->size - off : 0;
        uint32_t chunk = left > PAGE_SIZE ? PAGE_SIZE : (uint32_t)left;
        uint8_t *src = (uint8_t *)(vmm_hhdm_offset + c->phys[i]);
        lock_release(LOCK_PCACHE, &pcache_lock);

        if (chunk) fs_node_write(&c->node, (uint32_t)off, src, chunk);

        lock_acquire(LOCK_PCACHE, &pcache_lock);
        if (c->dirty) c->dirty[i] = 0;
        c->busy--;
        lock_release(LOCK_PCACHE, &pcache_lock);
    }
    return true;
}

void pcache_release(struct pcache_file *c) {
    if (!c) return;
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    if (c->users) c->users--;
    bool last = (c->users == 0 && c->busy == 0);
    lock_release(LOCK_PCACHE, &pcache_lock);
    if (last) pcache_writeback(c);
}

void pcache_write_through(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    if (!node || !buf || !len) return;
    uint64_t key = node->cache_key;
    if (!key) return;

    lock_acquire(LOCK_PCACHE, &pcache_lock);
    for (uint32_t i = 0; i < PCACHE_MAX_FILES; i++) {
        struct pcache_file *c = &pcache_files[i];
        if (!c->used || c->key != key || !c->loaded || !c->phys || !c->dirty) continue;
        uint64_t pos = offset;
        const uint8_t *src = (const uint8_t *)buf;
        uint32_t left = len;
        while (left) {
            uint32_t idx = (uint32_t)(pos / PAGE_SIZE);
            uint32_t boff = (uint32_t)(pos % PAGE_SIZE);
            if (idx >= c->npages) break;
            uint32_t chunk = PAGE_SIZE - boff;
            if (chunk > left) chunk = left;
            uint8_t *dst = (uint8_t *)(vmm_hhdm_offset + c->phys[idx]);
            memcpy(dst + boff, src, chunk);
            c->dirty[idx] = 1;
            src += chunk;
            pos += chunk;
            left -= chunk;
        }
        break;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
}

void pcache_mark_dirty(struct pcache_file *c, uint32_t first, uint32_t npages) {
    if (!c || !c->dirty) return;
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t idx = first + i;
        if (idx < c->npages) c->dirty[idx] = 1;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
}

bool pcache_map_info(vaddr_t vaddr, struct pcache_map *out) {
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    bool found = false;
    for (uint32_t i = 0; i < PCACHE_MAX_MAPS; i++) {
        if (!pcache_maps[i].used || pcache_maps[i].vaddr != vaddr) continue;
        if (out) *out = pcache_maps[i];
        found = true;
        break;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
    return found;
}

bool pcache_sync_addr(vaddr_t vaddr, size_t pages) {
    (void)pages;
    struct pcache_map m;
    if (!pcache_map_info(vaddr, &m)) return false;
    if (m.shared && m.file) pcache_writeback(m.file);
    return true;
}

bool pcache_map_update(vaddr_t vaddr, vaddr_t new_vaddr, size_t oldpages, size_t newpages,
                       paddr_t *phys, uint8_t *own_flag) {
    (void)oldpages;
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    bool found = false;
    for (uint32_t i = 0; i < PCACHE_MAX_MAPS; i++) {
        struct pcache_map *m = &pcache_maps[i];
        if (!m->used || m->vaddr != vaddr) continue;
        paddr_t *old_phys = m->phys;
        uint8_t *old_flag = m->own_flag;
        m->phys = phys;
        m->own_flag = own_flag;
        m->vpages = newpages;
        m->vaddr = new_vaddr;
        found = true;
        lock_release(LOCK_PCACHE, &pcache_lock);
        if (old_phys) kfree(old_phys);
        if (old_flag) kfree(old_flag);
        return found;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
    return found;
}

void pcache_sync_all(void) {
    struct pcache_file *files[PCACHE_MAX_FILES];
    uint32_t n = 0;

    lock_acquire(LOCK_PCACHE, &pcache_lock);
    for (uint32_t i = 0; i < PCACHE_MAX_FILES && n < PCACHE_MAX_FILES; i++)
        if (pcache_files[i].used) files[n++] = &pcache_files[i];
    lock_release(LOCK_PCACHE, &pcache_lock);

    for (uint32_t i = 0; i < n; i++) pcache_writeback(files[i]);
}

bool pcache_map_add(struct pcache_file *c, uint64_t key, bool shared, uint32_t first_page,
                    uint32_t npages, vaddr_t vaddr, size_t vpages, paddr_t *phys,
                    uint8_t *own_flag) {
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    struct pcache_map *m = NULL;
    for (uint32_t i = 0; i < PCACHE_MAX_MAPS; i++) {
        if (!pcache_maps[i].used) { m = &pcache_maps[i]; break; }
    }
    if (m) {
        m->used = true;
        m->shared = shared;
        m->key = key;
        m->file = c;
        m->first_page = first_page;
        m->npages = npages;
        m->vaddr = vaddr;
        m->vpages = vpages;
        m->phys = phys;
        m->own_flag = own_flag;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
    return m != NULL;
}

void pcache_map_remove(vaddr_t vaddr, size_t pages) {
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    struct pcache_map *m = NULL;
    for (uint32_t i = 0; i < PCACHE_MAX_MAPS; i++) {
        if (pcache_maps[i].used && pcache_maps[i].vaddr == vaddr &&
            pcache_maps[i].vpages == pages) {
            m = &pcache_maps[i];
            break;
        }
    }
    if (!m) {
        lock_release(LOCK_PCACHE, &pcache_lock);
        return;
    }

    struct pcache_file *c = m->file;
    bool shared = m->shared;
    paddr_t *phys = m->phys;
    uint8_t *own_flag = m->own_flag;
    size_t vpages = m->vpages;
    memset(m, 0, sizeof(*m));
    lock_release(LOCK_PCACHE, &pcache_lock);

    if (shared && c) pcache_writeback(c);
    if (phys && own_flag) {
        for (size_t i = 0; i < vpages; i++)
            if (own_flag[i] && phys[i]) pmm_free(phys[i], 1);
        kfree(phys);
        kfree(own_flag);
    }
    if (c) pcache_release(c);
}

size_t pcache_pages_used(void) {
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    size_t pages = 0;
    uint32_t files = 0;
    for (uint32_t i = 0; i < PCACHE_MAX_FILES; i++) {
        if (!pcache_files[i].used) continue;
        files++;
        pages += pcache_files[i].npages;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
    (void)files;
    return pages;
}

size_t pcache_files_cached(void) {
    lock_acquire(LOCK_PCACHE, &pcache_lock);
    size_t files = 0;
    for (uint32_t i = 0; i < PCACHE_MAX_FILES; i++) {
        if (pcache_files[i].used) files++;
    }
    lock_release(LOCK_PCACHE, &pcache_lock);
    return files;
}

