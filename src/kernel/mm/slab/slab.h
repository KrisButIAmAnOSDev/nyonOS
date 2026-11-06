#ifndef SLAB_H
#define SLAB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct kmem_cache;

void slab_init(void);

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);

size_t kmem_cache_obj_size(struct kmem_cache *cache);
size_t kmem_cache_pages_per_slab(struct kmem_cache *cache);

#endif
