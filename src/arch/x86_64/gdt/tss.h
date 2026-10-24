#ifndef TSS_H
#define TSS_H

#include <stdint.h>
#include <stddef.h>

#define TSS_IST_COUNT 7

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[TSS_IST_COUNT];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

void tss_init(uint64_t hhdm_offset, uint32_t id);
void tss_set_rsp0(uint32_t id, uint64_t rsp);
uint64_t tss_addr(uint32_t id);

#endif