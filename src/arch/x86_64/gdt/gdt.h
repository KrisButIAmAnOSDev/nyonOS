#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

#define GDT_USER_CODE_RPL3 (GDT_USER_CODE | 3)
#define GDT_USER_DATA_RPL3 (GDT_USER_DATA | 3)

#define GDT_ENTRIES 7

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(uint64_t hhdm_offset);
void gdt_dump(void);

#endif
