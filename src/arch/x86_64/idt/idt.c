#include "idt.h"
#include "io/kprintf/kprintf.h"
#include "io/serial/serial.h"
#include "kernel/panic/panic.h"
#include "arch/x86_64/pic/pic.h"
#include "arch/x86_64/pit/pit.h"

struct idt_entry idt[IDT_SIZE];
struct idtr idtr;

static irq_handler_t irq_handlers[IRQ_COUNT];
uint64_t spurious_irq_count = 0;

extern void* isr_stub_table[];
extern void idt_load(void* idtr_ptr);
extern void* get_stub_table(void);

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags, uint8_t ist) {
    idt[vector].isr_low = (uint64_t)isr & 0xFFFF;
    idt[vector].kernel_cs = GDT_KERNEL_CODE;
    idt[vector].ist = ist;
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

    idt_load(&idtr);

    __asm__ volatile("sti");

    kprintf(PRINT_SERIAL, "IDT loaded!\n");
}

void irq_install(uint8_t irq, irq_handler_t handler) {
    if (irq >= IRQ_COUNT) return;
    irq_handlers[irq] = handler;
}

void irq_dispatch(struct isr_frame* frame) {
    uint8_t vector = (uint8_t)frame->interrupt_number;
    if (vector < IDT_FIRST_IRQ || vector >= IDT_FIRST_IRQ + IRQ_COUNT) return;

    uint8_t irq = (uint8_t)(vector - IDT_FIRST_IRQ);

    current_isr_frame = frame;

    if (irq >= 8) pic_send_eoi(8);
    pic_send_eoi(0);

    if (irq_handlers[irq]) irq_handlers[irq]();
    else spurious_irq_count++;
}

void exception_handler(struct isr_frame* frame) {
    panic("CPU Exception", frame); //sad nyon 
}
