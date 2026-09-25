#include "arch/x86_64/pic/pic.h"
#include "io/serial/serial.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

void pic_remap(uint8_t offset1, uint8_t offset2) {
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, 0x00);
    outb(PIC2_DATA, 0x00);

    serial_print("PIC remapped to ");
    char buf[4];
    buf[0] = '0' + (offset1 / 100);
    buf[1] = '0' + ((offset1 / 10) % 10);
    buf[2] = '0' + (offset1 % 10);
    buf[3] = 0;
    serial_print(buf);
    serial_print("-");
    buf[0] = '0' + (offset2 / 100);
    buf[1] = '0' + ((offset2 / 10) % 10);
    buf[2] = '0' + (offset2 % 10);
    buf[3] = 0;
    serial_print(buf);
    serial_print("\n");
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t irq_num = irq % 8;
    uint8_t mask = inb(port) | (1 << irq_num);
    outb(port, mask);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t irq_num = irq % 8;
    uint8_t mask = inb(port) & ~(1 << irq_num);
    outb(port, mask);
}

void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    serial_print("PIC disabled\n");
}