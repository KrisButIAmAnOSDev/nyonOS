#include "task.h"
#include "io/kprintf/kprintf.h"
#include "io/serial/serial.h"
#include "kernel/sync/sync.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/gdt/tss.h"
#include "kernel/usermode.h"

#define QW_RIP 17
#define QW_CS 18
#define QW_RFLAGS 19
#define QW_RSP 20
#define QW_SS 21

extern void task_return_stub(void);

_Static_assert(offsetof(struct isr_frame, interrupt_number) == 120, "frame layout");
_Static_assert(offsetof(struct isr_frame, error_code) == 128, "frame layout");
_Static_assert(offsetof(struct isr_frame, rip) == 136, "frame layout");
_Static_assert(offsetof(struct isr_frame, cs) == 144, "frame layout");
_Static_assert(offsetof(struct isr_frame, rflags) == 152, "frame layout");
_Static_assert(offsetof(struct isr_frame, rsp) == 160, "frame layout");
_Static_assert(offsetof(struct isr_frame, ss) == 168, "frame layout");
_Static_assert(sizeof(struct isr_frame) == 176, "frame layout");

static struct task tasks[TASK_MAX];
static size_t current_slot = 0;
static bool in_scheduler = false;
static bool sched_enabled = false;
static uint64_t switch_count = 0;
static spinlock_t sched_lock = SPINLOCK_INIT;

static void task_kstack_update(void) {
    tss_set_rsp0(tasks[current_slot].stack_top);
}

static void task_reap_zombies(void) {
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].zombie || i == current_slot) continue;

        pmm_free(tasks[i].stack_base - vmm_hhdm_offset, TASK_STACK_PAGES);
        if (tasks[i].user_stack_base) {
            vmm_unmap(tasks[i].user_stack_base, TASK_USER_STACK_PAGES);
            pmm_free(tasks[i].user_stack_phys, TASK_USER_STACK_PAGES);
        }
        tasks[i].user_stack_phys = 0;
        tasks[i].user_stack_base = 0;
        tasks[i].user_stack_top = 0;
        tasks[i].stack_base = 0;
        tasks[i].stack_top = 0;
        tasks[i].frame = NULL;
        tasks[i].name = NULL;
        tasks[i].zombie = false;
    }
}

static void task_resume(struct isr_frame *frame) {
    task_resume_asm(frame);
    for (;;) __asm__ volatile("hlt");
}

void task_init(void) {
    current_slot = 0;
    in_scheduler = false;
    switch_count = 0;

    for (size_t i = 0; i < TASK_MAX; i++) {
        tasks[i].frame = NULL;
        tasks[i].stack_base = 0;
        tasks[i].stack_top = 0;
        tasks[i].in_use = false;
        tasks[i].zombie = false;
        tasks[i].user_stack_phys = 0;
        tasks[i].user_stack_base = 0;
        tasks[i].user_stack_top = 0;
        tasks[i].name = NULL;
    }

    uint64_t boot_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(boot_rsp));
    tasks[0].stack_base = 0;
    tasks[0].stack_top = boot_rsp;
    tasks[0].in_use = true;
    tasks[0].name = "kmain";
    task_kstack_update();
    sched_enabled = true;
}

struct task *task_spawn(const char *name, void (*entry)(void)) {
    if (!entry) return NULL;

    lock_acquire(LOCK_SCHED, &sched_lock);

    task_reap_zombies();

    size_t slot = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].in_use && !tasks[i].zombie) {
            slot = i;
            break;
        }
    }

    if (!slot) {
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    paddr_t phys;
    if (!pmm_alloc(&phys, TASK_STACK_PAGES)) {
        lock_release(LOCK_SCHED, &sched_lock);
        kprintf(PRINT_SERIAL, "task: out of memory for stack\n");
        return NULL;
    }

    struct task *t = &tasks[slot];
    t->stack_base = vmm_hhdm_offset + phys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;

    vaddr_t frame_addr = (t->stack_top - TASK_FRAME_GUARD) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;

    for (size_t i = 0; i < sizeof(struct isr_frame) / 8; i++) q[i] = 0;

    *(uint64_t *)(frame_addr - 8) = (uint64_t)task_return_stub;

    q[QW_RIP] = (uint64_t)entry;
    q[QW_CS] = GDT_KERNEL_CODE;
    q[QW_RFLAGS] = TASK_RFLAGS_IF;
    q[QW_RSP] = frame_addr - 8;
    q[QW_SS] = GDT_KERNEL_DATA;

    t->frame = f;
    t->in_use = true;
    t->zombie = false;
    t->name = name;
    tss_set_rsp0(t->stack_top);

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned ");
    kprintf(PRINT_SERIAL, "%s", name);
    kprintchar('\n', PRINT_SERIAL);
    return t;
}

struct task *task_spawn_ring3(const char *name, vaddr_t user_rip) {
    lock_acquire(LOCK_SCHED, &sched_lock);

    size_t slot = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].in_use && !tasks[i].zombie) { slot = i; break; }
    }
    if (!slot) {
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    paddr_t kphys, uphys;
    if (!pmm_alloc(&kphys, TASK_STACK_PAGES)) {
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }
    if (!pmm_alloc(&uphys, TASK_USER_STACK_PAGES)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    vaddr_t ustack_virt = TASK_USER_STACK_VIRT;
    if (!vmm_map(ustack_virt, uphys, TASK_USER_STACK_PAGES, VMM_USER_FLAGS)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    struct task *t = &tasks[slot];
    t->stack_base = vmm_hhdm_offset + kphys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;
    t->user_stack_phys = uphys;
    t->user_stack_base = ustack_virt;
    t->user_stack_top = ustack_virt + TASK_USER_STACK_PAGES * PAGE_SIZE;
    t->zombie = false;

    vaddr_t frame_addr = (t->stack_top - TASK_FRAME_GUARD) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;
    for (size_t i = 0; i < sizeof(struct isr_frame) / 8; i++) q[i] = 0;

    q[QW_RIP] = user_rip;
    q[QW_CS] = GDT_USER_CODE_RPL3;
    q[QW_RFLAGS] = TASK_RFLAGS_IF;
    q[QW_RSP] = t->user_stack_top - 8;
    q[QW_SS] = GDT_USER_DATA_RPL3;

    t->frame = f;
    t->in_use = true;
    t->name = name;
    tss_set_rsp0(t->stack_top);

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned ");
    kprintf(PRINT_SERIAL, "%s", name);
    kprintf(PRINT_SERIAL, " in ring 3 (rip=0x%llx cs=0x%llx ss=0x%llx)\n", (unsigned long long)user_rip, (unsigned long long)GDT_USER_CODE_RPL3, (unsigned long long)GDT_USER_DATA_RPL3);
    return t;
}

static struct task *pick_next(void) {
    for (size_t i = 1; i <= TASK_MAX; i++) {
        size_t idx = (current_slot + i) % TASK_MAX;
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

    task_reap_zombies();

    struct task *next = pick_next();
    if (next && next != &tasks[current_slot]) {
        current_slot = (size_t)(next - tasks);
        switch_count++;
    }

    if (!tasks[current_slot].frame) {
        kprintf(PRINT_SERIAL, "task: no saved frame, cannot resume\n");
        for (;;) __asm__ volatile("hlt");
    }

    task_kstack_update();

    lock_release(LOCK_SCHED, &sched_lock);

    in_scheduler = false;
    task_resume(tasks[current_slot].frame);
}

void task_exit(void) {
    struct task *self = &tasks[current_slot];

    kprintf(PRINT_SERIAL, "task: ");
    kprintf(PRINT_SERIAL, "%s", self->name);
    kprintf(PRINT_SERIAL, " exited\n");

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);
    self->in_use = false;
    self->zombie = true;
    struct task *next = pick_next();
    if (next) {
        current_slot = (size_t)(next - tasks);
        switch_count++;
    }
    task_kstack_update();
    lock_release(LOCK_SCHED, &sched_lock);

    if (!next) {
        for (;;) __asm__ volatile("hlt");
    }

    task_resume(tasks[current_slot].frame);
}

struct task *task_current(void) {
    return &tasks[current_slot];
}

size_t task_current_index(void) {
    return current_slot;
}

uint64_t task_switch_count(void) {
    return switch_count;
}
