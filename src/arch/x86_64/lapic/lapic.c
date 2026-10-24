#include "lapic.h"

#define LAPIC_LVT0_OFFSET 0x350

static inline uint32_t lapic_read32(uint64_t base, uint32_t reg) {
    return *(volatile uint32_t *)(base + reg);
}

static inline void lapic_write32(uint64_t base, uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(base + reg) = v;
}

void lapic_unmask_ext_int(uint64_t lapic_virt_base) {
    lapic_write32(lapic_virt_base, LAPIC_LVT0_OFFSET, 0x00000700);
}

void lapic_mask_all_lvt(uint64_t base) {
    lapic_write32(base, LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write32(base, LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write32(base, LAPIC_REG_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write32(base, LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write32(base, LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write32(base, LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);
}

void lapic_spurious_enable(uint64_t base, uint8_t vector) {
    lapic_write32(base, LAPIC_REG_SPURIOUS, ((uint32_t)vector & 0xFFu) | 0x100u);
}

void lapic_write_lvt_timer(uint64_t base, uint32_t value) {
    lapic_write32(base, LAPIC_REG_LVT_TIMER, value);
}

void lapic_timer_set(uint64_t base, uint32_t count, uint32_t div_id) {
    lapic_write32(base, LAPIC_REG_TIMER_DIV, div_id);
    lapic_write32(base, LAPIC_REG_TIMER_INIT, count);
}

uint64_t lapic_id_read(uint64_t base) {
    return lapic_read32(base, LAPIC_REG_ID) >> 24;
}

void lapic_eoi(uint64_t base) {
    lapic_write32(base, LAPIC_REG_EOI, 0);
}

void lapic_ipi_wait_clear(uint64_t base) {
    while (lapic_read32(base, LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVS) {
        __asm__ volatile("pause" ::: "memory");
    }
}

uint32_t lapic_ipi_send(uint64_t base, uint32_t vector, uint32_t target_lapic_id, bool init) {
    lapic_write32(base, LAPIC_REG_ERR, 0);
    lapic_write32(base, LAPIC_REG_ICR_HIGH, target_lapic_id << 24);

    uint32_t cmd = init ? LAPIC_ICR_INIT : (LAPIC_ICR_SIPI | (vector & 0xFF));
    lapic_write32(base, LAPIC_REG_ICR_LOW, cmd);
    lapic_ipi_wait_clear(base);

    return lapic_read32(base, LAPIC_REG_ERR);
}