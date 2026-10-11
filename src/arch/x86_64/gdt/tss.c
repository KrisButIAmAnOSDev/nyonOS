#include "tss.h"
#include "kernel/kprintf/kprintf.h"
#include "drivers/serial/serial.h"
#include "kernel/mm/pmm/pmm.h"
#include "arch/x86_64/cpu/cpu.h"

static struct tss tss[MAX_CPUS];

static uint64_t alloc_stack_top(uint64_t hhdm_offset) {
    paddr_t p;
    if (!pmm_alloc(&p, 1)) {
        kprintf(PRINT_SERIAL, "TSS: out of memory allocating stack\n");
        for (;;) __asm__ volatile("hlt");
    }
    return hhdm_offset + p + PAGE_SIZE;
}

void tss_init(uint64_t hhdm_offset, uint32_t id) {
    if (id >= MAX_CPUS) return;

    struct tss *t = &tss[id];

    uint8_t *raw = (uint8_t *)t;
    for (size_t i = 0; i < sizeof(*t); i++) raw[i] = 0;

    t->iomap_base = sizeof(struct tss);

    for (int i = 0; i < TSS_IST_COUNT; i++) {
        t->ist[i] = alloc_stack_top(hhdm_offset);
    }

    tss_set_rsp0(id, alloc_stack_top(hhdm_offset));
}

void tss_set_rsp0(uint32_t id, uint64_t rsp) {
    if (id >= MAX_CPUS) return;
    tss[id].rsp[0] = rsp;
}

uint64_t tss_addr(uint32_t id) {
    if (id >= MAX_CPUS) id = 0;
    return (uint64_t)&tss[id];
}