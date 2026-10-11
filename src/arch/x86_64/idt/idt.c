#include "idt.h"
#include "kernel/kprintf/kprintf.h"
#include "drivers/serial/serial.h"
#include "kernel/panic/panic.h"
#include "kernel/uaccess/uaccess.h"
#include "kernel/multitask/task.h"
#include "arch/x86_64/pic/pic.h"
#include "arch/x86_64/pit/pit.h"

static inline uint64_t read_cr2(void) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

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

void idt_install(uint8_t vector, void *stub, uint8_t flags) {
    idt_set_descriptor(vector, stub, flags, 0);
}

void idt_reload(void) {
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    idt_load(&idtr);
}

void irq_install(uint8_t irq, irq_handler_t handler) {
    if (irq >= IRQ_COUNT) return;
    irq_handlers[irq] = handler;
}

void irq_dispatch(struct isr_frame* frame) {
    uint8_t vector = (uint8_t)frame->interrupt_number;
    if (vector < IDT_FIRST_IRQ || vector >= IDT_FIRST_IRQ + IRQ_COUNT) return;

    uint8_t irq = (uint8_t)(vector - IDT_FIRST_IRQ);

    if (irq >= 8) pic_send_eoi(8);
    pic_send_eoi(0);

    if (irq_handlers[irq]) irq_handlers[irq](frame);
    else spurious_irq_count++;
}

static int exception_signal(uint64_t vector) {
    switch (vector) {
        case 0:  return 8;
        case 6:  return 4;
        case 8:
        case 13:
        case 14: return 11;
        default: return 4;
    }
}

void exception_handler(struct isr_frame* frame) {


    if ((frame->cs & 3) == 0 && usermode_recover_copy(frame)) return;

    if ((frame->cs & 3) == 3) {
        struct task *t = task_current();
        const char *name = t && t->name ? t->name : "ring3";
        uint64_t vector = frame->interrupt_number;
        int signal = exception_signal(vector);

        if (!t || !t->expect_fault) {
            kprintf(PRINT_BOTH, "\ntask %s killed by CPU exception, vector 0x%llx\n", name, (unsigned long long)vector);
            kprintf(PRINT_BOTH, "  faulting rip=0x%llx err=0x%llx cr2=0x%llx signal=%d\n", (unsigned long long)frame->rip, (unsigned long long)frame->error_code, (unsigned long long)read_cr2(), signal);
            kprintf(PRINT_BOTH, "  rax=0x%llx rdi=0x%llx rsi=0x%llx rbx=0x%llx rbp=0x%llx\n", (unsigned long long)frame->rax, (unsigned long long)frame->rdi, (unsigned long long)frame->rsi, (unsigned long long)frame->rbx, (unsigned long long)frame->rbp);
        }

        task_exit_code(128 + signal);
    }

    panic("CPU Exception", frame);
}
