#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/idt/idt.h"

#define TASK_MAX 4
#define TASK_STACK_PAGES 2
#define TASK_QUANTUM_TICKS 20

struct task {
    struct isr_frame *frame;
    vaddr_t stack_base;
    vaddr_t stack_top;
    bool in_use;
    const char *name;
};

void task_resume_asm(struct isr_frame *frame);
void task_init(void);struct task *task_spawn(const char *name, void (*entry)(void));
void task_schedule(struct isr_frame *frame);
void task_exit(void);
struct task *task_current(void);
uint64_t task_switch_count(void);

#endif
