#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include "arch/x86_64/gdt/gdt.h"

#define IDT_SIZE 256

#define IDT_FIRST_EXC 0
#define IDT_FIRST_IRQ 32
#define IRQ_COUNT 16

#define IDT_ATTR_PRESENT 0x8E
#define IDT_ATTR_DPL3 0xEE

typedef void (*irq_handler_t)(void);
void irq_install(uint8_t irq, irq_handler_t handler);

struct idt_entry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t ist;
    uint8_t attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t interrupt_number;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
};

extern struct idt_entry idt[IDT_SIZE];
extern struct idtr idtr;
extern void* isr_stub_table[];

void idt_init(void);
void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags, uint8_t ist);
void exception_handler(struct isr_frame* frame);
void irq_dispatch(struct isr_frame* frame);

#endif
