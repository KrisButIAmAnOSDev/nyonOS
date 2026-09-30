#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/idt/idt.h"

#define TASK_MAX 16
#define TASK_STACK_PAGES 4
#define TASK_QUANTUM_TICKS 20
#define TASK_FRAME_GUARD 512
#define TASK_RFLAGS_IF 0x202
#define TASK_USER_STACK_PAGES 1
#define TASK_USER_STACK_VIRT 0x0000000040000000ULL

struct task {
    struct isr_frame *frame;
    vaddr_t stack_base;
    vaddr_t stack_top;
    paddr_t user_stack_phys;
    vaddr_t user_stack_base;
    vaddr_t user_stack_top;
    bool in_use;
    bool zombie;
    const char *name;
};

void task_resume_asm(struct isr_frame *frame);
void task_init(void);
struct task *task_spawn(const char *name, void (*entry)(void));
struct task *task_spawn_ring3(const char *name, vaddr_t user_rip);
void task_schedule(struct isr_frame *frame);
void task_exit(void);
struct task *task_current(void);
size_t task_current_index(void);
uint64_t task_switch_count(void);

#endif
