//file should be name la pace,iykyk
#include "lapic.h"

#define LAPIC_PHYS_BASE 0xFEE00000
#define LAPIC_LVT0_OFFSET 0x350

static inline void lapic_write(uint64_t virt_base, uint32_t reg_offset, uint32_t value) {
    *((volatile uint32_t*)(virt_base + reg_offset)) = value;
}

void lapic_unmask_ext_int(uint64_t hhdm_offset) {
    uint64_t lapic_virt_base = hhdm_offset + LAPIC_PHYS_BASE;
    lapic_write(lapic_virt_base, LAPIC_LVT0_OFFSET, 0x00000700);
}

