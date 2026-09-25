#include "idt.h"
#include "io/serial/serial.h"
#include "kernel/panic.h"

struct idt_entry idt[IDT_SIZE];
struct idtr idtr;

extern void* isr_stub_table[];
extern void idt_load(void* idtr_ptr);
extern void* get_stub_table(void);

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idt[vector].isr_low = (uint64_t)isr & 0xFFFF;
    idt[vector].kernel_cs = 0x28;
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

    for (int i = 0; i < 48; i++) {
        idt_set_descriptor(i, stub_table[i], 0x8E);
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
