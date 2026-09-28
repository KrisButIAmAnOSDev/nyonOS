#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/idt/idt.h"
#include "kernel/multitask/task.h"
#include "arch/x86_64/pic/pic.h"
#include "io/serial/serial.h"

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

void pit_handler(struct isr_frame* frame) {
    pit_ticks++;
    pic_send_eoi(0);
    if (pit_ticks % TASK_QUANTUM_TICKS == 0) {
        task_schedule(frame);
    }
}

void pit_init(uint32_t frequency_hz) {
    pit_frequency = frequency_hz;
    uint32_t divisor = 1193182 / frequency_hz;

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    pic_clear_mask(0);

    serial_print("PIT initialized at ");
    char buf[16];
    int i = 0;
    uint32_t freq = frequency_hz;
    if (freq == 0) buf[i++] = '0';
    else {
        char rev[16];
        int j = 0;
        while (freq > 0) {
            rev[j++] = '0' + (freq % 10);
            freq /= 10;
        }
        while (j--) buf[i++] = rev[j];
    }
    buf[i++] = 'H'; buf[i++] = 'z'; buf[i] = 0;
    serial_print(buf);
    serial_print("\n");
}

void pit_sleep(uint64_t ms) {
    uint64_t ticks_per_ms = pit_frequency / 1000;
    if (ticks_per_ms == 0) ticks_per_ms = 1;
    uint64_t target = pit_ticks + ms * ticks_per_ms;
    while (pit_ticks < target) __asm__ volatile("hlt");
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}
