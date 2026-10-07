#ifndef PIT_H
#define PIT_H

#include <stdint.h>
#include "arch/x86_64/idt/idt.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43

#define PIT_FREQUENCY 1193182

void pit_init(uint32_t frequency_hz);
void pit_sleep(uint64_t ms);
uint64_t pit_get_ticks(void);
uint64_t pit_ms_to_ticks(uint64_t ms);
void pit_handler(struct isr_frame *frame);

#endif