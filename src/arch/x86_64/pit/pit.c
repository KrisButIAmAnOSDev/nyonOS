#include "arch/x86_64/pit/pit.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/idt/idt.h"
#include "kernel/multitask/task.h"
#include "kernel/sync/preempt.h"
#include "arch/x86_64/pic/pic.h"
#include "drivers/serial/serial.h"

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void pit_handler(struct isr_frame *frame) {
    pit_ticks++;

    if (pit_ticks % TASK_QUANTUM_TICKS != 0) return;
    if (preempt_count() != 0) return;

    task_schedule(frame);
}

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        kprintf(PRINT_SERIAL, "PIT: frequency 0 rejected\n");
        return;
    }

    uint32_t divisor = PIT_FREQUENCY / frequency_hz;

    if (divisor == 0 || divisor > 0xFFFF) {
        kprintf(PRINT_SERIAL, "PIT: %u Hz needs divisor %u, outside 1..65535, rejected\n", frequency_hz, divisor);
        return;
    }

    pit_frequency = PIT_FREQUENCY / divisor;

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    pic_clear_mask(0);

    kprintf(PRINT_SERIAL, "PIT initialized at %u Hz\n", pit_frequency);
}

void pit_sleep(uint64_t ms) {
    uint64_t target = pit_ticks + (ms * pit_frequency) / 1000;
    while (pit_ticks < target) __asm__ volatile("hlt");
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}
