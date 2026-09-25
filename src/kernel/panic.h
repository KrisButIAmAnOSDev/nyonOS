#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>
#include "arch/x86_64/idt/idt.h"

struct panic_context {
    struct isr_frame frame;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t rsp_at_panic;
};

void panic(const char* msg, struct isr_frame* frame);
void panic_dump_regs(struct panic_context* ctx);

#endif