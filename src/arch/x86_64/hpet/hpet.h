#ifndef HPET_H
#define HPET_H

#include <stdint.h>
#include <stdbool.h>

#define HPET_CAP        0x000
#define HPET_CONF       0x010
#define HPET_INT_STATUS 0x020
#define HPET_COUNTER    0x0F0
#define HPET_TIMER0_CONF 0x100
#define HPET_TIMER0_CMP  0x108

#define HPET_CONF_ENABLE    (1ULL << 0)
#define HPET_CONF_LEGACY    (1ULL << 1)

#define HPET_TN_ENABLE      (1ULL << 2)
#define HPET_TN_PERIODIC    (1ULL << 3)
#define HPET_TN_LEVEL       (1ULL << 1)

bool hpet_init(uint32_t target_hz);
uint64_t hpet_counter(void);
bool hpet_available(void);
uint64_t hpet_nanoseconds(void);
uint32_t hpet_frequency(void);

#endif
