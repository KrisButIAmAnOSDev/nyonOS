#include "tss.h"
#include "io/serial/serial.h"
#include "kernel/mm/pmm/pmm.h"

static struct tss tss;

static uint64_t alloc_stack_top(uint64_t hhdm_offset) {
    paddr_t p;
    if (!pmm_alloc(&p, 1)) {
        serial_print("TSS: out of memory allocating stack\n");
        for (;;) __asm__ volatile("hlt");
    }
    return hhdm_offset + p + PAGE_SIZE;
}

void tss_init(uint64_t hhdm_offset) {
    uint8_t *raw = (uint8_t *)&tss;
    for (size_t i = 0; i < sizeof(tss); i++) raw[i] = 0;

    tss.iomap_base = sizeof(struct tss);

    for (int i = 0; i < TSS_IST_COUNT; i++) {
        tss.ist[i] = alloc_stack_top(hhdm_offset);
    }

    tss_set_rsp0(alloc_stack_top(hhdm_offset));
}

void tss_set_rsp0(uint64_t rsp) {
    tss.rsp[0] = rsp;
}

uint64_t tss_addr(void) {
    return (uint64_t)&tss;
}

uint64_t tss_get_ist(int index) {
    if (index < 0 || index >= TSS_IST_COUNT) return 0;
    return tss.ist[index];
}
