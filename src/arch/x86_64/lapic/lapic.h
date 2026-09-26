#ifndef LAPIC_H
#define LAPIC_H
#include <stdint.h>
void lapic_unmask_ext_int(uint64_t lapic_virt_base);
#endif
