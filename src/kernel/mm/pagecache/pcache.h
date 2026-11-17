#ifndef PCACHE_H
#define PCACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel/vfs/vfs.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/heap/heap.h"

#define PCACHE_MAX_FILES 16
#define PCACHE_MAX_MAPS 32
#define PCACHE_MAX_BYTES (16u * 1024u * 1024u)

struct pcache_file {
    bool used;
    bool loaded;
    uint64_t key;
    uint64_t size;
    uint32_t npages;
    paddr_t *phys;
    uint8_t *dirty;
    uint32_t users;
    uint32_t busy;
    uint64_t stamp;
    fs_node_t node;
};

struct pcache_map {
    bool used;
    bool shared;
    uint64_t key;
    struct pcache_file *file;
    uint32_t first_page;
    uint32_t npages;
    vaddr_t vaddr;
    size_t vpages;
    paddr_t *phys;
    uint8_t *own_flag;
};

void pcache_init(void);
struct pcache_file *pcache_acquire(fs_node_t *node);
void pcache_release(struct pcache_file *c);
bool pcache_writeback(struct pcache_file *c);
void pcache_write_through(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len);
void pcache_mark_dirty(struct pcache_file *c, uint32_t first, uint32_t npages);
void pcache_sync_all(void);
size_t pcache_pages_used(void);
size_t pcache_files_cached(void);
bool pcache_map_add(struct pcache_file *c, uint64_t key, bool shared, uint32_t first_page,
                    uint32_t npages, vaddr_t vaddr, size_t vpages, paddr_t *phys,
                    uint8_t *own_flag);
bool pcache_sync_addr(vaddr_t vaddr, size_t pages);
bool pcache_map_update(vaddr_t vaddr, vaddr_t new_vaddr, size_t oldpages, size_t newpages,
                       paddr_t *phys, uint8_t *own_flag);
bool pcache_map_info(vaddr_t vaddr, struct pcache_map *out);
void pcache_map_remove(vaddr_t vaddr, size_t pages);

#endif
