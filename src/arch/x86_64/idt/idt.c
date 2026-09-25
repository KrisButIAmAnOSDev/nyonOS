#include "idt.h"
#include "io/serial/serial.h"

struct idt_entry idt[IDT_SIZE];
struct idtr idtr;

extern void* isr_stub_table[];
extern void idt_load(void* idtr_ptr);
extern void* get_stub_table(void);

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idt[vector].isr_low = (uint64_t)isr & 0xFFFF;
    idt[vector].kernel_cs = 0x08;
    idt[vector].ist = 0;
    idt[vector].attributes = flags;
    idt[vector].isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
    idt[vector].isr_high = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

void idt_init(void) {
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;

    void** stub_table = (void**)get_stub_table();

    for (int i = 0; i < 32; i++) {
        idt_set_descriptor(i, stub_table[i], 0x8E);
    }

    idt_load(&idtr);
    __asm__ volatile("sti");

    serial_print("IDT loaded!\n");

    // Debug: print idt[0] entry
    serial_print("idt[0]: ");
    for (int i = 0; i < 16; i++) {
        unsigned char byte = ((unsigned char*)idt)[i];
        char hex[3];
        hex[0] = "0123456789abcdef"[(byte >> 4) & 0xf];
        hex[1] = "0123456789abcdef"[byte & 0xf];
        hex[2] = 0;
        serial_print(hex);
        serial_print(" ");
    }
    serial_print("\n");
}

void exception_handler(struct isr_frame* frame) {
    serial_print("Exception: ");
    char num[4];
    num[0] = '0' + (frame->interrupt_number / 100);
    num[1] = '0' + ((frame->interrupt_number / 10) % 10);
    num[2] = '0' + (frame->interrupt_number % 10);
    num[3] = 0;
    serial_print(num);
    serial_print("\n");

    if (frame->interrupt_number == 0) {
        serial_print("Divide by zero!\n");
    }

    for (;;) __asm__ volatile("hlt");
}
