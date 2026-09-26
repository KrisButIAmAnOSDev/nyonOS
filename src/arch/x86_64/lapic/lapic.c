//file should be name la pace,iykyk
#include "lapic.h"

#define LAPIC_LVT0_OFFSET 0x350

void lapic_unmask_ext_int(uint64_t lapic_virt_base) {
    *((volatile uint32_t*)(lapic_virt_base + LAPIC_LVT0_OFFSET)) = 0x00000700;
}

