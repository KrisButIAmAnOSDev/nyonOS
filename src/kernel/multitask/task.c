#include "task.h"
#include "io/kprintf/kprintf.h"
#include "io/serial/serial.h"
#include "kernel/sync/sync.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/pit/pit.h"

#define FRAME_SIZE 136
#define QW_RIP 17
#define QW_CS 18
#define QW_RFLAGS 19
#define QW_RSP 20
#define QW_SS 21

_Static_assert(offsetof(struct isr_frame, interrupt_number) == 120, "frame layout");
_Static_assert(offsetof(struct isr_frame, error_code) == 128, "frame layout");
_Static_assert(offsetof(struct isr_frame, rip) == 136, "frame layout");
_Static_assert(offsetof(struct isr_frame, cs) == 144, "frame layout");
_Static_assert(offsetof(struct isr_frame, rflags) == 152, "frame layout");

static struct task tasks[TASK_MAX];
static size_t task_count = 0;
static size_t current_slot = 0;
static bool in_scheduler = false;
static bool sched_enabled = false;
static uint64_t switch_count = 0;
static spinlock_t sched_lock = SPINLOCK_INIT;



static void task_resume(struct isr_frame *frame) {
    task_resume_asm(frame);
    for (;;) __asm__ volatile("hlt");
}

void task_init(void) {
    task_count = 0;
    current_slot = 0;
    in_scheduler = false;
    switch_count = 0;

    for (size_t i = 0; i < TASK_MAX; i++) {
        tasks[i].frame = NULL;
        tasks[i].stack_base = 0;
        tasks[i].stack_top = 0;
        tasks[i].in_use = false;
        tasks[i].name = NULL;
    }

    tasks[0].in_use = true;
    tasks[0].name = "kmain";
    sched_enabled = true;
}

struct task *task_spawn(const char *name, void (*entry)(void)) {
    if (!entry) return NULL;
    if (task_count + 1 >= TASK_MAX) return NULL;

    lock_acquire(LOCK_SCHED, &sched_lock);

    paddr_t phys;
    if (!pmm_alloc(&phys, TASK_STACK_PAGES)) {
        lock_release(LOCK_SCHED, &sched_lock);
        kprintf(PRINT_SERIAL, "task: out of memory for stack\n");
        return NULL;
    }

    struct task *t = &tasks[task_count + 1];
    t->stack_base = vmm_hhdm_offset + phys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;

    vaddr_t frame_addr = (t->stack_top - 512) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;

    for (int i = 0; i < FRAME_SIZE / 8; i++) q[i] = 0;

    q[QW_RIP] = (uint64_t)entry;
    q[QW_CS] = 0x08;
    q[QW_RFLAGS] = 0x202;
    q[QW_RSP] = t->stack_top - 512;
    q[QW_SS] = 0x10;

    t->frame = f;
    t->in_use = true;
    t->name = name;
    task_count++;

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned ");
    kprintf(PRINT_SERIAL, "%s", name);
    kprintchar('\n', PRINT_SERIAL);
    return t;
}

static struct task *pick_next(void) {
    size_t total = task_count + 1;
    for (size_t i = 1; i <= total; i++) {
        size_t idx = (current_slot + i) % total;
        if (tasks[idx].in_use) return &tasks[idx];
    }
    return NULL;
}

void task_schedule(struct isr_frame *frame) {
    if (!sched_enabled) return;
    if (in_scheduler) return;

    if (sync_lock_depth() != 0) {
        panic_assert("context switch attempted while this task still holds a lock");
    }

    in_scheduler = true;

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);

    tasks[current_slot].frame = frame;

    struct task *next = pick_next();
    if (next && next != &tasks[current_slot]) {
        current_slot = (size_t)(next - tasks);
        switch_count++;
    }

    if (!tasks[current_slot].frame) {
        kprintf(PRINT_SERIAL, "task: no saved frame, cannot resume\n");
        for (;;) __asm__ volatile("hlt");
    }

    lock_release(LOCK_SCHED, &sched_lock);

    in_scheduler = false;
    task_resume(tasks[current_slot].frame);
}

void task_exit(void) {
    struct task *self = &tasks[current_slot];

    lock_acquire(LOCK_SCHED, &sched_lock);
    self->in_use = false;
    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: ");
    kprintf(PRINT_SERIAL, "%s", self->name);
    kprintf(PRINT_SERIAL, " exited\n");

    for (;;) {
        struct task *next = pick_next();
        if (!next) {
            __asm__ volatile("cli" ::: "memory");
            for (;;) __asm__ volatile("hlt");
        }
        current_slot = (size_t)(next - tasks);
        switch_count++;
        task_resume(tasks[current_slot].frame);
    }
}

struct task *task_current(void) {
    return &tasks[current_slot];
}

uint64_t task_switch_count(void) {
    return switch_count;
}
