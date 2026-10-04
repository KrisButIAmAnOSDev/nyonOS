#include "task.h"
#include "kernel/kprintf/kprintf.h"
#include "drivers/serial/serial.h"
#include "kernel/sync/sync.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/gdt/tss.h"
#include "arch/x86_64/syscall/syscall.h"
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
static volatile uint64_t wake_seq = 0;
static uint32_t next_pid = 1;
static bool in_scheduler = false;
static bool sched_enabled = false;
static uint64_t switch_count = 0;
static spinlock_t sched_lock = SPINLOCK_INIT;

static void task_kstack_update(void) {
    tss_set_rsp0(tasks[current_slot].stack_top);
    syscall_kstack_top = tasks[current_slot].stack_top;
}

static void task_aspace_update(void) {
    struct page_table *pml4 = tasks[current_slot].pml4;
    vmm_switch_address_space(pml4 ? pml4 : kernel_pml4);
}

static void task_release(struct task *t) {
    fd_table_clear(t);

    if (t->pml4) {
        if (t->user_stack_base) vmm_unmap_from(t->pml4, t->user_stack_base, TASK_USER_STACK_PAGES);
        if (t->user_code_base) vmm_unmap_from(t->pml4, t->user_code_base, t->user_code_pages);
        vmm_destroy_address_space(t->pml4);
        t->pml4 = NULL;
    }

    if (t->user_stack_phys) pmm_free(t->user_stack_phys, TASK_USER_STACK_PAGES);
    if (t->user_code_phys) pmm_free(t->user_code_phys, t->user_code_pages);
    if (t->stack_base) pmm_free(t->stack_base - vmm_hhdm_offset, TASK_STACK_PAGES);

    t->user_stack_phys = 0;
    t->user_stack_base = 0;
    t->user_stack_top = 0;
    t->user_code_phys = 0;
    t->user_code_base = 0;
    t->user_code_pages = 0;
    t->stack_base = 0;
    t->stack_top = 0;
    t->frame = NULL;
    t->name = NULL;
    t->zombie = false;
    t->blocked = false;
    t->exit_code = 0;
}

static void task_reap_zombies(void) {
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].zombie || i == current_slot) continue;
        task_release(&tasks[i]);
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
        tasks[i].user_code_phys = 0;
        tasks[i].user_code_base = 0;
        tasks[i].user_code_pages = 0;
        tasks[i].pml4 = NULL;
        tasks[i].exit_code = 0;
        tasks[i].blocked = false;
        tasks[i].name = NULL;
        fd_table_init(&tasks[i]);
    }

    uint64_t boot_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(boot_rsp));
    tasks[0].stack_base = 0;
    tasks[0].stack_top = boot_rsp;
    tasks[0].in_use = true;
    tasks[0].name = "kmain";
    tasks[0].pid = 0;
    tasks[0].debug_log = true;
    fd_install_stdio(&tasks[0]);
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
        kprintf(PRINT_SERIAL, "task: out of memory for stack (mem leak?)\n");
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
    t->debug_log = false;
    t->pid = next_pid++;
    t->name = name;

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned ");
    kprintf(PRINT_SERIAL, "%s", name);
    kprintchar('\n', PRINT_SERIAL);
    return t;
}

struct task *task_spawn_ring3(const char *name, paddr_t code_phys, size_t code_pages, vaddr_t code_virt, vaddr_t entry_rip, paddr_t shared_phys, vaddr_t shared_virt) {
    lock_acquire(LOCK_SCHED, &sched_lock);

    task_reap_zombies();

    size_t slot = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].in_use && !tasks[i].zombie) { slot = i; break; }
    }
    if (!slot) {
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    struct page_table *pml4 = vmm_create_address_space();
    if (!pml4) {
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    paddr_t kphys, uphys;
    if (!pmm_alloc(&kphys, TASK_STACK_PAGES)) {
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }
    if (!pmm_alloc(&uphys, TASK_USER_STACK_PAGES)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    if (!vmm_map_user(pml4, code_virt, code_phys, code_pages, VMM_DEFAULT_FLAGS)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    if (shared_phys && !vmm_map_user(pml4, shared_virt, shared_phys, 1, VMM_DEFAULT_FLAGS)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    if (!vmm_map_user(pml4, TASK_USER_STACK_VIRT, uphys, TASK_USER_STACK_PAGES, VMM_DEFAULT_FLAGS)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    struct task *t = &tasks[slot];
    t->stack_base = vmm_hhdm_offset + kphys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;
    t->user_stack_phys = uphys;
    t->user_stack_base = TASK_USER_STACK_VIRT;
    t->user_stack_top = TASK_USER_STACK_VIRT + TASK_USER_STACK_PAGES * PAGE_SIZE;
    t->expect_fault = false;
    t->user_code_phys = code_phys;
    t->user_code_base = code_virt;
    t->user_code_pages = code_pages;
    t->pml4 = pml4;
    t->exit_code = 0;
    t->blocked = false;
    t->zombie = false;

    vaddr_t frame_addr = (t->stack_top - TASK_FRAME_GUARD) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;
    for (size_t i = 0; i < sizeof(struct isr_frame) / 8; i++) q[i] = 0;

    q[QW_RIP] = entry_rip;
    q[QW_CS] = GDT_USER_CODE_RPL3;
    q[QW_RFLAGS] = TASK_RFLAGS_IF;
    q[QW_RSP] = t->user_stack_top - 8;
    q[QW_SS] = GDT_USER_DATA_RPL3;

    t->frame = f;
    t->in_use = true;
    t->debug_log = false;
    t->pid = next_pid++;
    t->name = name;
    fd_table_init(t);
    fd_install_stdio(t);

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned %s in ring 3 (rip=0x%llx as=%p)\n", name, (unsigned long long)entry_rip, (void *)pml4);
    return t;
}

static struct task *pick_next(void) {
    for (size_t i = 1; i <= TASK_MAX; i++) {
        size_t idx = (current_slot + i) % TASK_MAX;
        if (tasks[idx].in_use && !tasks[idx].blocked) return &tasks[idx];
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
    task_aspace_update();

    lock_release(LOCK_SCHED, &sched_lock);

    in_scheduler = false;
    task_resume(tasks[current_slot].frame);
}

void task_exit_code(uint64_t code) {
    struct task *self = &tasks[current_slot];

    self->exit_code = code;

    if (!self->expect_fault)
        kprintf(PRINT_SERIAL, "task: %s exited with code %llu\n", self->name, (unsigned long long)code);

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);
    self->in_use = false;
    self->blocked = false;
    self->zombie = true;
    struct task *next = pick_next();
    if (next) {
        current_slot = (size_t)(next - tasks);
        switch_count++;
    }
    task_kstack_update();
    task_aspace_update();
    lock_release(LOCK_SCHED, &sched_lock);

    if (!next) {
        for (;;) __asm__ volatile("sti; hlt" ::: "memory");
    }

    task_resume(tasks[current_slot].frame);
}

void task_exit(void) {
    task_exit_code(0);
}

void task_block_current(uint64_t seen_seq) {
    if (__atomic_load_n(&wake_seq, __ATOMIC_ACQUIRE) != seen_seq) return;

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);
    tasks[current_slot].blocked = true;
    lock_release(LOCK_SCHED, &sched_lock);

    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
        __asm__ volatile("cli" ::: "memory");

        lock_acquire(LOCK_SCHED, &sched_lock);
        bool woken = !tasks[current_slot].blocked;
        lock_release(LOCK_SCHED, &sched_lock);

        if (woken) {
            __asm__ volatile("sti" ::: "memory");
            return;
        }
    }
}


uint64_t task_wake_seq(void) {
    return __atomic_load_n(&wake_seq, __ATOMIC_ACQUIRE);
}

void task_unblock_all(void) {
    // called from the keyboard ISR: taking LOCK_SCHED here would trip the
    // held_mask checker (or self-deadlock) if the interrupted code held a lock
    __atomic_add_fetch(&wake_seq, 1, __ATOMIC_RELEASE);

    // 0, not 1: slot 0 is kmain, and kmain is the task parked on input. Every
    // other loop over tasks[] starts at 1, so this one looks like a nyonpo (only comment in the whole os btw)
    for (size_t i = 0; i < TASK_MAX; i++) {
        if (tasks[i].in_use) __atomic_store_n(&tasks[i].blocked, false, __ATOMIC_RELEASE);
    }
}

struct task *task_current(void) {
    return &tasks[current_slot];
}

struct page_table *task_current_pml4(void) {
    return tasks[current_slot].pml4;
}

uint64_t task_switch_count(void) {
    return switch_count;
}
