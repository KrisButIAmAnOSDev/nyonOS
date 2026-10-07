#ifndef VMSPACE_H
#define VMSPACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/mm/vmm/vmm.h"

#define VM_MAX_REGIONS 64
#define VM_PROT_READ   1
#define VM_PROT_WRITE  2
#define VM_PROT_EXEC   4
#define VM_STACK_GUARD_PAGES 16
#define VM_STACK_PAGES       4
#define VM_STACK_TOP_VIRT    0x0000000040000000ULL

#define VM_BACKING_ANON 0
#define VM_BACKING_NONE 1

struct vm_region {
    vaddr_t start;
    vaddr_t end;
    uint8_t prot;
    uint8_t maxprot;
    uint8_t backing;
    bool mapped;
};

struct vmspace {
    struct page_table *pml4;
    uint32_t refcount;
    struct vm_region regions[VM_MAX_REGIONS];
    size_t region_count;
    vaddr_t stack_base;
    vaddr_t stack_top;
    vaddr_t stack_bottom;
    bool has_stack;
};

struct vmspace *vm_create(void);
void vm_destroy(struct vmspace *vs);
struct vmspace *vm_ref(struct vmspace *vs);

int vm_region_add(struct vmspace *vs, vaddr_t start, vaddr_t end, uint8_t prot, uint8_t backing);
struct vm_region *vm_region_find(struct vmspace *vs, vaddr_t addr);

bool vm_map_anon(struct vmspace *vs, vaddr_t vaddr, size_t pages, uint8_t prot);
bool vm_map_anon_zeroed(struct vmspace *vs, vaddr_t vaddr, size_t pages, uint8_t prot, bool zero);
bool vm_commit(struct vmspace *vs, vaddr_t vaddr, paddr_t phys, size_t pages, uint8_t prot, bool owned);

bool vm_setup_stack(struct vmspace *vs);
bool vm_stack_push_strings(struct vmspace *vs, const char **strs, size_t count,
                           uint64_t *argc_out, uint64_t *stack_out, uint64_t *argv_out, uint64_t *auxv_out);

void vm_free_range(struct vmspace *vs, vaddr_t start, vaddr_t end);

uint64_t vm_flags_for(uint8_t prot);

#endif
