#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/idt/idt.h"
#include "kernel/fd/fd.h"

#define TASK_MAX 16
#define TASK_STACK_PAGES 4
#define TASK_QUANTUM_TICKS 20
#define TASK_FRAME_GUARD 512
#define TASK_RFLAGS_IF 0x202
#define TASK_USER_STACK_PAGES 1
#define TASK_USER_STACK_VIRT 0x0000000040000000ULL
#define TASK_USER_CODE_VIRT  0x0000000000400000ULL

struct task {
    struct isr_frame *frame;
    vaddr_t stack_base;
    vaddr_t stack_top;
    paddr_t user_stack_phys;
    vaddr_t user_stack_base;
    vaddr_t user_stack_top;
    paddr_t user_code_phys;
    vaddr_t user_code_base;
    size_t user_code_pages;
    struct page_table *pml4;
    uint64_t exit_code;
    bool in_use;
    bool zombie;
    bool blocked;
    bool expect_fault;
    const char *name;
    struct fd_table fds;
};

void task_resume_asm(struct isr_frame *frame);
void task_init(void);
struct task *task_spawn(const char *name, void (*entry)(void));
struct task *task_spawn_ring3(const char *name, paddr_t code_phys, size_t code_pages, vaddr_t code_virt, vaddr_t entry_rip, paddr_t shared_phys, vaddr_t shared_virt);
void task_schedule(struct isr_frame *frame);
void task_exit(void);
void task_exit_code(uint64_t code);
void task_block_current(uint64_t seen_seq);
uint64_t task_wake_seq(void);
void task_unblock_all(void);
struct task *task_current(void);
struct page_table *task_current_pml4(void);
uint64_t task_switch_count(void);

#endif
