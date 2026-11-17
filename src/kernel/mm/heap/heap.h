#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

void heap_init(void);
size_t heap_used_bytes(void);
size_t heap_used_blocks(void);
size_t heap_free_blocks(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);

#endif
