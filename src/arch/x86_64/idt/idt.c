#include "idt.h"
#include "io/serial/serial.h"
#include "kernel/panic.h"

struct idt_entry idt[IDT_SIZE];
struct idtr idtr;

extern void* isr_stub_table[];
extern void idt_load(void* idtr_ptr);
extern void* get_stub_table(void);

volatile uint64_t debug_interrupt_count = 0;
volatile uint64_t debug_last_vector = 0xFFFFFFFFFFFFFFFF;

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags, uint8_t ist) {
    idt[vector].isr_low = (uint64_t)isr & 0xFFFF;
    idt[vector].kernel_cs = GDT_KERNEL_CODE;
    idt[vector].ist = ist;
    idt[vector].attributes = flags;
    idt[vector].isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
    idt[vector].isr_high = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

void debug_track_interrupt(uint64_t vector) {
    debug_interrupt_count++;
    serial_print("INT vector=");
    char buf[4];
    buf[0] = '0' + (vector / 100);
    buf[1] = '0' + ((vector / 10) % 10);
    buf[2] = '0' + (vector % 10);
    buf[3] = 0;
    serial_print(buf);
    serial_print("\n");
}

void idt_init(void) {
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;

    void** stub_table = (void**)get_stub_table();

    for (int i = 0; i < 48; i++) {
        uint8_t ist = 0;
        uint8_t flags = 0x8E;

        if (i == 8) {
            ist = 1;
        } else if (i == 2) {
            ist = 2;
        } else if (i == 1) {
            ist = 3;
            flags = 0xEE;
        } else if (i == 0 || i == 6 || i == 12 || i == 13 || i == 14) {
            flags = 0xEE;
        }

        idt_set_descriptor(i, stub_table[i], flags, ist);
    }

    extern void* isr_stub_unexpected;
    for (int i = 48; i < 256; i++) {
        idt_set_descriptor(i, isr_stub_unexpected, 0x8E, 0);
    }

    // TEMP DEBUG: confirm stub_table[32] holds a real address
    serial_print("stub[32]=0x");
    uint64_t addr = (uint64_t)stub_table[32];
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (addr >> i) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        serial_putchar(c);
    }
    serial_print("\n");

    // TEMP DEBUG: confirm idt[32]'s actual programmed bytes
    serial_print("idt[32]: ");
    for (int i = 0; i < 16; i++) {
        unsigned char byte = ((unsigned char*)&idt[32])[i];
        char hex[3];
        hex[0] = "0123456789abcdef"[(byte >> 4) & 0xf];
        hex[1] = "0123456789abcdef"[byte & 0xf];
        hex[2] = 0;
        serial_print(hex);
        serial_print(" ");
    }
    serial_print("\n");

    idt_load(&idtr);

    __asm__ volatile("sti");

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    serial_print("RFLAGS after sti: 0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (rflags >> i) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        serial_putchar(c);
    }
    serial_print("\n");

    serial_print("IDT loaded!\n");
}

void exception_handler(struct isr_frame* frame) {
    panic("CPU Exception", frame); //sad nyon 
}
