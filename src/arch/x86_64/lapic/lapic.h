#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define LAPIC_DEFAULT_PHYS   0xFEE00000ULL

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_VER        0x030
#define LAPIC_REG_TPR        0x080
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SPURIOUS   0x0F0
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_LVT_THERMAL 0x330
#define LAPIC_REG_LVT_PERF    0x340
#define LAPIC_REG_LVT_LINT0   0x350
#define LAPIC_REG_LVT_LINT1   0x360
#define LAPIC_REG_LVT_ERROR   0x370
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0
#define LAPIC_REG_ERR        0x280

#define LAPIC_LVT_MASKED     0x00010000u

#define LAPIC_RESCHED_VECTOR 0xFD
#define LAPIC_TIMER_PERIOD 200000u
#define LAPIC_TIMER_DIV    0x0B

#define LAPIC_ICR_DELIVS     (1u << 12)
#define LAPIC_ICR_INIT       0x00004500u
#define LAPIC_ICR_SIPI       0x00004600u

void lapic_unmask_ext_int(uint64_t lapic_virt_base);
void lapic_mask_all_lvt(uint64_t base);
void lapic_timer_set(uint64_t base, uint32_t count, uint32_t div_id);
void lapic_write_lvt_timer(uint64_t base, uint32_t value);
void lapic_spurious_enable(uint64_t base, uint8_t vector);
uint64_t lapic_id_read(uint64_t base);
void lapic_eoi(uint64_t base);
void lapic_ipi_wait_clear(uint64_t base);
uint32_t lapic_ipi_send(uint64_t base, uint32_t vector, uint32_t target_lapic_id, bool init);

#endif