#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43

#define PIT_FREQUENCY 1193182

void pit_init(uint32_t frequency_hz);
void pit_sleep(uint64_t ms);

#endif