#include "task.h"
#include "kernel/kprintf/kprintf.h"
#include "drivers/serial/serial.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/heap/heap.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/gdt/tss.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/syscall/syscall_msr.h"
#include "kernel/uaccess/uaccess.h"
#include "kernel/mm/vmm/vmspace.h"
#include "arch/x86_64/cpu/cpu.h"
#include "arch/x86_64/lapic/lapic.h"

#define QW_RIP 17
#define QW_CS 18
#define QW_RFLAGS 19
#define QW_RAX 16
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
static uint32_t next_pid = 1;
static spinlock_t sched_lock = SPINLOCK_INIT;
static uint32_t idle_slot[MAX_CPUS];

static void task_wake_broadcast(void);

static inline size_t current_slot(void) { return cpu_self()->current_slot; }
static inline void set_current_slot(size_t v) { cpu_self()->current_slot = v; }
static inline volatile uint64_t *wake_seq_p(void) { return &cpu_self()->wake_seq; }
static inline bool in_scheduler(void) { return cpu_self()->in_scheduler; }
static inline void set_in_scheduler(bool v) { cpu_self()->in_scheduler = v; }
static inline bool sched_enabled(void) { return cpu_self()->sched_enabled; }
static inline void set_sched_enabled(bool v) { cpu_self()->sched_enabled = v; }
static inline uint64_t switch_count(void) { return cpu_self()->switch_count; }
static inline void bump_switch_count(void) { cpu_self()->switch_count++; }
static volatile uint64_t earliest_wake_v = UINT64_MAX;

static inline uint64_t earliest_wake(void) {
    return __atomic_load_n(&earliest_wake_v, __ATOMIC_ACQUIRE);
}
static inline void set_earliest_wake(uint64_t v) {
    __atomic_store_n(&earliest_wake_v, v, __ATOMIC_RELEASE);
}

static void task_wake_broadcast(void) {
    if (cpu_count() > 1) lapic_ipi_broadcast(cpu_self()->lapic_base, LAPIC_RESCHED_VECTOR);
}

void task_register_idle(uint32_t cpu, struct task *t) {
    if (!t || cpu >= MAX_CPUS) return;
    t->cpu = cpu;
    idle_slot[cpu] = (uint32_t)(t - tasks);
}

void task_adopt_idle(uint32_t cpu) {
    if (cpu >= MAX_CPUS || idle_slot[cpu] >= TASK_MAX) return;
    set_current_slot(idle_slot[cpu]);
    tasks[idle_slot[cpu]].running = true;
}

static void task_kstack_update(void) {
    size_t cs = current_slot();
    tss_set_rsp0((uint32_t)cpu_id(), tasks[cs].stack_top);
    *percpu_syscall_kstack_top() = tasks[cs].stack_top;
}

void task_aspace_update(void) {
    struct page_table *pml4 = tasks[current_slot()].pml4;
    vmm_switch_address_space(pml4 ? pml4 : kernel_pml4);
    msr_write(MSR_FS_BASE, tasks[current_slot()].fs_base);
}

#define PGROUP_NONE 0xFFFFFFFFu

struct pgroup {
    bool used;
    uint32_t pgid;
    uint32_t sid;
    uint32_t count;
    uint32_t head;
};

static struct pgroup pgroups[TASK_MAX];

static int pgroup_find(uint32_t pgid, uint32_t sid) {
    for (size_t i = 0; i < TASK_MAX; i++)
        if (pgroups[i].used && pgroups[i].pgid == pgid && pgroups[i].sid == sid)
            return (int)i;
    return -1;
}

static int pgroup_find_or_create(uint32_t pgid, uint32_t sid) {
    int i = pgroup_find(pgid, sid);
    if (i >= 0) return i;
    for (size_t j = 0; j < TASK_MAX; j++) {
        if (pgroups[j].used) continue;
        pgroups[j].used = true;
        pgroups[j].pgid = pgid;
        pgroups[j].sid = sid;
        pgroups[j].count = 0;
        pgroups[j].head = 0;
        return (int)j;
    }
    return -1;
}

static void pgroup_unlink(struct task *t) {
    if (!t || t->pg_idx == PGROUP_NONE || t->pg_idx >= TASK_MAX) {
        if (t) {
            t->pg_idx = PGROUP_NONE;
            t->pg_next = 0;
        }
        return;
    }
    struct pgroup *g = &pgroups[t->pg_idx];
    if (g->used) {
        uint32_t self1 = (uint32_t)(t - tasks) + 1;
        uint32_t *link = &g->head;
        while (*link) {
            if (*link == self1) {
                *link = t->pg_next;
                break;
            }
            struct task *n = &tasks[*link - 1];
            link = &n->pg_next;
        }
        if (g->count) g->count--;
        if (g->count == 0) {
            g->used = false;
            g->head = 0;
        }
    }
    t->pg_idx = PGROUP_NONE;
    t->pg_next = 0;
}

void task_pgrp_join(struct task *t, uint32_t pgid, uint32_t sid) {
    if (!t) return;
    pgroup_unlink(t);
    t->pgid = pgid;
    t->sid = sid;
    if (!pgid) {
        t->pg_idx = PGROUP_NONE;
        return;
    }
    int i = pgroup_find_or_create(pgid, sid);
    if (i < 0) {
        t->pg_idx = PGROUP_NONE;
        return;
    }
    struct pgroup *g = &pgroups[i];
    t->pg_idx = (uint32_t)i;
    t->pg_next = g->head;
    g->head = (uint32_t)(t - tasks) + 1;
    g->count++;
}

static void task_release(struct task *t) {
    fd_tty_task_gone(t->pid);
    fd_table_clear(t);
    pgroup_unlink(t);
    t->pgid = 0;
    t->sid = 0;
    t->cpu = TASK_CPU_ANY;
    t->running = false;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_blocked = 0;
    t->sig_in_handler = false;
    t->sig_frame_user = 0;
    t->sig_terminate = false;

    if (t->vs && t->vs->refcount > 1 && t->user_stack_phys &&
        t->user_stack_top > t->user_stack_base) {
        size_t pages = (size_t)(t->user_stack_top - t->user_stack_base) / PAGE_SIZE;
        vmm_unmap_from(t->vs->pml4, t->user_stack_base, pages);
        vm_region_remove(t->vs, t->user_stack_base, t->user_stack_top);
        pmm_free(t->user_stack_phys, pages);
        t->user_stack_phys = 0;
        t->user_stack_base = 0;
        t->user_stack_top = 0;
    }

    if (t->vs) {
        vm_destroy(t->vs);
        t->vs = NULL;
        t->pml4 = NULL;
    }

    if (t->pml4) {
        if (t->user_stack_base) vmm_unmap_from(t->pml4, t->user_stack_base, TASK_USER_STACK_PAGES);
        if (t->user_code_base) vmm_unmap_from(t->pml4, t->user_code_base, t->user_code_pages);
        vmm_destroy_address_space(t->pml4);
        t->pml4 = NULL;
    }

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
    t->vs = NULL;
    t->zombie = false;
    t->blocked = false;
    t->parent_pid = 0;
    t->child_chan = &t->child_chan;
    t->vfork_child = 0;
    t->vfork_chan = &t->vfork_chan;
    t->wait_chan = NULL;
    t->wake_tick = 0;
    t->wake_gen = 0;
    t->exit_code = 0;
    t->fs_base = 0;
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    t->copy_active = false;
    for (size_t i = 0; i < TASK_FXSAVE_BYTES; i++) t->fxsave[i] = 0;
}

static void task_reap_zombies(void) {
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].zombie || i == current_slot()) continue;
        if (tasks[i].parent_pid) continue;
        task_release(&tasks[i]);
    }
}

static void task_kstack_zero(paddr_t phys) {
    uint8_t *ks = (uint8_t *)(vmm_hhdm_offset + phys);
    for (size_t z = 0; z < TASK_STACK_PAGES * PAGE_SIZE; z++) ks[z] = 0;
}

static inline void task_fpu_save(void) {
    __asm__ volatile("fxsave %0" : "=m"(tasks[current_slot()].fxsave));
}

static inline void task_fpu_load(void) {
    __asm__ volatile("fxrstor %0" : : "m"(tasks[current_slot()].fxsave));
}

static void task_resume(struct isr_frame *frame) {
    task_fpu_load();
    task_resume_asm(frame);
    for (;;) __asm__ volatile("hlt");
}

void task_init(void) {
    set_current_slot(0);
    set_in_scheduler(false);
    cpu_self()->switch_count = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) idle_slot[i] = TASK_MAX;

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
        for (size_t j = 0; j < TASK_FXSAVE_BYTES; j++) tasks[i].fxsave[j] = 0;
        tasks[i].blocked = false;
        tasks[i].wait_chan = NULL;
        tasks[i].wake_tick = 0;
        tasks[i].wake_gen = 0;
        tasks[i].name = NULL;
        tasks[i].cpu = TASK_CPU_ANY;
        tasks[i].running = false;
        tasks[i].pgid = 0;
        tasks[i].sid = 0;
        tasks[i].pg_idx = PGROUP_NONE;
        tasks[i].pg_next = 0;
        tasks[i].cwd[0] = '/';
        tasks[i].cwd[1] = 0;
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
    tasks[0].running = true;
    fd_install_stdio(&tasks[0]);
    task_kstack_update();
    set_sched_enabled(true);
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
    t->blocked = false;
    t->vfork_child = 0;
    t->vfork_chan = &t->vfork_chan;
    t->wait_chan = NULL;
    t->wake_tick = 0;
    t->wake_gen = 0;
    t->debug_log = false;
    t->running = false;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_blocked = 0;
    t->sig_in_handler = false;
    t->sig_frame_user = 0;
    t->sig_terminate = false;
    t->cpu = TASK_CPU_ANY;
    t->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_RELAXED);
    task_pgrp_join(t, 0, 0);
    t->name = name;

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned ");
    kprintf(PRINT_SERIAL, "%s", name);
    kprintchar('\n', PRINT_SERIAL);
    return t;
}

static struct task *spawn_ring3(const char *name, paddr_t code_phys, size_t code_pages, vaddr_t code_virt, vaddr_t entry_rip, paddr_t shared_phys, vaddr_t shared_virt, bool shared_owned) {
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
    task_kstack_zero(kphys);
    if (!pmm_alloc(&uphys, TASK_USER_STACK_PAGES)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }


    if (!vmm_map_user_exec(pml4, code_virt, code_phys, code_pages, VMM_USER_EXEC_FLAGS)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    bool shared_ok = !shared_phys ||
        (shared_owned ? vmm_map_user(pml4, shared_virt, shared_phys, 1, VMM_USER_EXEC_FLAGS)
                      : vmm_map_user_lent(pml4, shared_virt, shared_phys, 1, VMM_USER_EXEC_FLAGS));
    if (!shared_ok) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    if (!vmm_map_user(pml4, TASK_USER_STACK_VIRT, uphys, TASK_USER_STACK_PAGES, VMM_USER_EXEC_FLAGS)) {
        pmm_free(kphys, TASK_STACK_PAGES);
        pmm_free(uphys, TASK_USER_STACK_PAGES);
        vmm_destroy_address_space(pml4);
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }

    struct task *t = &tasks[slot];
    t->parent_pid = 0;
    t->child_chan = &t->child_chan;
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
    t->vfork_child = 0;
    t->vfork_chan = &t->vfork_chan;
    t->wait_chan = NULL;
    t->wake_tick = 0;
    t->wake_gen = 0;
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
    t->running = false;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_blocked = 0;
    t->sig_in_handler = false;
    t->sig_frame_user = 0;
    t->sig_terminate = false;
    t->cpu = TASK_CPU_ANY;
    t->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_RELAXED);
    task_pgrp_join(t, 0, 0);
    t->name = name;
    fd_table_init(t);
    fd_install_stdio(t);

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned %s in ring 3 (rip=0x%llx as=%p)\n", name, (unsigned long long)entry_rip, (void *)pml4);
    return t;
}

struct task *task_spawn_ring3(const char *name, paddr_t code_phys, size_t code_pages, vaddr_t code_virt, vaddr_t entry_rip, paddr_t shared_phys, vaddr_t shared_virt) {
    return spawn_ring3(name, code_phys, code_pages, code_virt, entry_rip, shared_phys, shared_virt, true);
}

struct task *task_spawn_thread(struct task *parent, const char *name,
                               vaddr_t entry_rip, vaddr_t user_rsp, uint64_t fs_base) {
    if (!parent || !name) return NULL;
    struct vmspace *vs = parent->vs;
    if (!vs || !vs->pml4) return NULL;

    lock_acquire(LOCK_SCHED, &sched_lock);
    task_reap_zombies();

    size_t slot = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].in_use && !tasks[i].zombie) { slot = i; break; }
    }
    if (!slot) { lock_release(LOCK_SCHED, &sched_lock); return NULL; }

    paddr_t kphys;
    if (!pmm_alloc(&kphys, TASK_STACK_PAGES)) { lock_release(LOCK_SCHED, &sched_lock); return NULL; }
    task_kstack_zero(kphys);

    struct task *t = &tasks[slot];
    t->parent_pid = parent->pid;
    task_pgrp_join(t, parent->pgid, parent->sid);
    t->child_chan = &t->child_chan;
    t->stack_base = vmm_hhdm_offset + kphys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;
    t->user_stack_phys = 0;
    t->user_stack_base = 0;
    t->user_stack_top = user_rsp;
    t->user_code_phys = 0;
    t->user_code_base = 0;
    t->user_code_pages = 0;
    vm_ref(vs);
    t->vs = vs;
    t->pml4 = vs->pml4;
    t->fs_base = fs_base;
    t->exit_code = 0;
    t->blocked = false;
    t->vfork_child = 0;
    t->vfork_chan = &t->vfork_chan;
    t->wait_chan = NULL;
    t->wake_tick = 0;
    t->zombie = false;
    t->expect_fault = false;

    vaddr_t frame_addr = (t->stack_top - TASK_FRAME_GUARD) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;
    for (size_t i = 0; i < sizeof(struct isr_frame) / 8; i++) q[i] = 0;

    q[QW_RIP] = entry_rip;
    q[QW_CS] = GDT_USER_CODE_RPL3;
    q[QW_RFLAGS] = TASK_RFLAGS_IF;
    q[QW_RSP] = user_rsp;
    q[QW_SS] = GDT_USER_DATA_RPL3;

    t->frame = f;
    t->in_use = true;
    t->debug_log = false;
    t->running = false;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_blocked = 0;
    t->sig_in_handler = false;
    t->sig_frame_user = 0;
    t->sig_terminate = false;
    t->cpu = TASK_CPU_ANY;
    t->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_RELAXED);
    task_pgrp_join(t, 0, 0);
    t->name = name;

    for (size_t i = 0; i < FD_PATH_MAX; i++) t->cwd[i] = parent->cwd[i];

    fd_table_clone(t, parent);

    lock_release(LOCK_SCHED, &sched_lock);
    return t;
}

struct task *task_spawn_fork(struct task *parent, const char *name,
                             struct isr_frame *src,
                             uint64_t fs_base, bool share_vm, bool clone_fds,
                             uint32_t *tid_out) {
    uint64_t entry_rip = src ? (uint64_t)src->rip : 0;
    uint64_t user_rsp = src ? (uint64_t)src->rsp : 0;
    if (!parent || !name) return NULL;
    if (!share_vm && !parent->vs) return NULL;
    struct vmspace *vs = parent->vs;
    if (!vs || !vs->pml4) return NULL;

    lock_acquire(LOCK_SCHED, &sched_lock);
    task_reap_zombies();

    size_t slot = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].in_use && !tasks[i].zombie) { slot = i; break; }
    }
    if (!slot) { lock_release(LOCK_SCHED, &sched_lock); return NULL; }

    paddr_t kphys;
    if (!pmm_alloc(&kphys, TASK_STACK_PAGES)) {
        lock_release(LOCK_SCHED, &sched_lock);
        return NULL;
    }
    task_kstack_zero(kphys);

    if (share_vm) vs->refcount++;

    if (share_vm && vmm_cow_enabled()) vmm_cow_privatise(vs->pml4);


    struct page_table *pml4;
    if (share_vm) {
        pml4 = vs->pml4;
    } else {
        pml4 = vmm_create_address_space();
        if (!pml4) {
            pmm_free(kphys, TASK_STACK_PAGES);
            lock_release(LOCK_SCHED, &sched_lock);
            return NULL;
        }
        size_t copied = 0;
        bool ok = vmm_cow_enabled()
                      ? vmm_share_user_space(vs->pml4, pml4, &copied)
                      : vmm_clone_user_space(vs->pml4, pml4, &copied);
        if (ok && vmm_cow_enabled() && user_rsp) {
            vaddr_t sp_page = user_rsp & ~(vaddr_t)(PAGE_SIZE - 1);
            vmm_cow_resolve(vs->pml4, sp_page);
            vmm_cow_resolve(vs->pml4, sp_page - PAGE_SIZE);
        }
        if (!ok) {
            vmm_destroy_address_space(pml4);
            pmm_free(kphys, TASK_STACK_PAGES);
            lock_release(LOCK_SCHED, &sched_lock);
            return NULL;
        }
    }

    struct vmspace *child_vs = vs;
    if (!share_vm) {
        child_vs = (struct vmspace *)kmalloc(sizeof(struct vmspace));
        if (!child_vs) {
            vmm_destroy_address_space(pml4);
            pmm_free(kphys, TASK_STACK_PAGES);
            lock_release(LOCK_SCHED, &sched_lock);
            return NULL;
        }
        child_vs->pml4 = pml4;
        child_vs->refcount = 1;
        for (size_t i = 0; i < vs->region_count && i < VM_MAX_REGIONS; i++)
            child_vs->regions[i] = vs->regions[i];
        child_vs->region_count = vs->region_count;
        child_vs->stack_base = vs->stack_base;
        child_vs->stack_top = vs->stack_top;
        child_vs->stack_bottom = vs->stack_bottom;
        child_vs->has_stack = vs->has_stack;
    }

    struct task *t = &tasks[slot];
    t->parent_pid = parent->pid;
    task_pgrp_join(t, parent->pgid, parent->sid);
    t->child_chan = &t->child_chan;
    t->stack_base = vmm_hhdm_offset + kphys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;
    t->user_stack_phys = 0;
    t->user_stack_base = 0;
    t->user_stack_top = user_rsp;

    if (share_vm) {
        paddr_t cstack = 0;
        vaddr_t sbase = vs->has_stack ? vs->stack_base : parent->user_stack_base;
        size_t spages = vs->has_stack ? VM_STACK_PAGES : TASK_USER_STACK_PAGES;
        uint64_t spte = sbase ? vmm_query_in(vs->pml4, sbase) : 0;
        vaddr_t cbase = 0;
        bool mapped = false;

        if ((spte & PAGE_PRESENT) &&
            pmm_alloc(&cstack, spages) &&
            vm_find_free(vs, TASK_VFORK_STACK_LO, TASK_VFORK_STACK_HI,
                         spages * PAGE_SIZE, &cbase)) {
            uint8_t *dst = (uint8_t *)(vmm_hhdm_offset + cstack);
            for (size_t i = 0; i < spages; i++) {
                uint64_t page = (i == 0) ? spte
                                         : vmm_query_in(vs->pml4, sbase + i * PAGE_SIZE);
                if (!(page & PAGE_PRESENT)) continue;
                const uint8_t *src = (const uint8_t *)(vmm_hhdm_offset +
                                                       (page & 0x000FFFFFFFFFF000ULL));
                uint8_t *out = dst + i * PAGE_SIZE;
                for (size_t k = 0; k < PAGE_SIZE; k++) out[k] = src[k];
            }

            if (vmm_map_frames(vs->pml4, cbase, cstack, spages,
                               PAGE_PRESENT | PAGE_WRITE | PAGE_USER, false, true) &&
                vm_region_add(vs, cbase, cbase + spages * PAGE_SIZE,
                              VM_PROT_READ | VM_PROT_WRITE, VM_BACKING_ANON) == 0) {
                t->user_stack_phys = cstack;
                t->user_stack_base = cbase;
                t->user_stack_top = cbase + spages * PAGE_SIZE;
                user_rsp = cbase + (user_rsp - sbase);
                mapped = true;
            }
        }

        if (!mapped && cstack) pmm_free(cstack, spages);
    }

    t->user_code_phys = 0;
    t->user_code_base = 0;
    t->user_code_pages = 0;
    t->fs_base = fs_base;
    t->pml4 = pml4;
    t->vs = child_vs;
    t->exit_code = 0;
    t->blocked = false;
    t->vfork_child = 0;
    t->vfork_chan = &t->vfork_chan;
    t->wait_chan = NULL;
    t->wake_tick = 0;
    t->zombie = false;
    t->expect_fault = false;

    vaddr_t frame_addr = (t->stack_top - TASK_FRAME_GUARD) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;
    for (size_t i = 0; i < sizeof(struct isr_frame) / 8; i++) q[i] = 0;

    if (src) {
        const uint64_t *s = (const uint64_t *)src;
        for (size_t i = 0; i < 14; i++) q[i] = s[i];
    }

    q[QW_RIP] = entry_rip;
    q[QW_CS] = GDT_USER_CODE_RPL3;
    q[QW_RFLAGS] = TASK_RFLAGS_IF;
    q[QW_RSP] = user_rsp;
    q[QW_SS] = GDT_USER_DATA_RPL3;
    q[QW_RAX] = 0;

    t->frame = f;
    t->in_use = true;
    t->debug_log = false;
    t->running = false;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_blocked = 0;
    t->sig_in_handler = false;
    t->sig_frame_user = 0;
    t->sig_terminate = false;
    t->cpu = TASK_CPU_ANY;
    t->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_RELAXED);
    task_pgrp_join(t, 0, 0);
    t->name = name;

    if (clone_fds) {
        fd_table_clone(t, parent);
    } else {
        fd_table_init(t);
        fd_install_stdio(t);
    }

    if (tid_out) *tid_out = t->pid;

    for (size_t i = 0; i < FD_PATH_MAX; i++) t->cwd[i] = parent->cwd[i];

    lock_release(LOCK_SCHED, &sched_lock);
    return t;
}

static void task_sleep_registered(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    struct task *self = &tasks[current_slot()];

    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
        __asm__ volatile("cli" ::: "memory");
        if (!__atomic_load_n(&self->blocked, __ATOMIC_ACQUIRE)) break;
    }

    self->wait_chan = NULL;
    if (rflags & TASK_RFLAGS_IF) __asm__ volatile("sti" ::: "memory");
}

int task_wait_child(struct task *parent, int32_t want, int32_t *status_out, int32_t options) {
    bool wnohang = (options & ABI_WNOHANG) != 0;
    struct task *self = &tasks[current_slot()];

    for (;;) {
        lock_acquire(LOCK_SCHED, &sched_lock);
        task_reap_zombies();

        size_t slot = 0;
        int32_t code = 0;
        for (size_t i = 1; i < TASK_MAX; i++) {
            if (!tasks[i].zombie || tasks[i].parent_pid != parent->pid) continue;
            if (want > 0 && (int32_t)tasks[i].pid != want) continue;
            slot = i;
            code = (int32_t)tasks[i].exit_code;
            break;
        }

        if (slot) {
            int32_t pid = (int32_t)tasks[slot].pid;
            int32_t st;
            if (code < 128) {
                st = (code & 0xFF) << 8;
            } else {
                st = (code - 128) & 0x7F;
            }
            lock_release(LOCK_SCHED, &sched_lock);

            if (status_out && !copyout((void *)status_out, &st, sizeof(st))) {
                lock_acquire(LOCK_SCHED, &sched_lock);
                task_release(&tasks[slot]);
                lock_release(LOCK_SCHED, &sched_lock);
                return SYS_EFAULT;
            }

            lock_acquire(LOCK_SCHED, &sched_lock);
            task_release(&tasks[slot]);
            lock_release(LOCK_SCHED, &sched_lock);
            return pid;
        }

        if (wnohang) {
            lock_release(LOCK_SCHED, &sched_lock);
            return SYS_ECHILD;
        }

        self->wait_chan = &self->child_chan;
        self->wake_tick = 0;
        self->blocked = true;
        lock_release(LOCK_SCHED, &sched_lock);

        task_sleep_registered();
    }
}

struct task *task_spawn_vmspace(const char *name, struct vmspace *vs, vaddr_t entry_rip, vaddr_t user_rsp) {
    if (!name || !vs || !vs->pml4) return NULL;

    lock_acquire(LOCK_SCHED, &sched_lock);
    task_reap_zombies();

    size_t slot = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (!tasks[i].in_use && !tasks[i].zombie) { slot = i; break; }
    }
    if (!slot) { lock_release(LOCK_SCHED, &sched_lock); return NULL; }

    paddr_t kphys;
    if (!pmm_alloc(&kphys, TASK_STACK_PAGES)) { lock_release(LOCK_SCHED, &sched_lock); return NULL; }
    task_kstack_zero(kphys);

    struct task *t = &tasks[slot];
    t->parent_pid = 0;
    t->child_chan = &t->child_chan;
    t->stack_base = vmm_hhdm_offset + kphys;
    t->stack_top = t->stack_base + TASK_STACK_PAGES * PAGE_SIZE;
    t->user_stack_phys = 0;
    t->user_stack_base = 0;
    t->user_stack_top = user_rsp;
    t->user_code_phys = 0;
    t->user_code_base = 0;
    t->user_code_pages = 0;
    vm_ref(vs);
    t->vs = vs;
    t->pml4 = vs->pml4;
    t->exit_code = 0;
    t->blocked = false;
    t->vfork_child = 0;
    t->vfork_chan = &t->vfork_chan;
    t->wait_chan = NULL;
    t->wake_tick = 0;
    t->wake_gen = 0;
    t->zombie = false;
    t->expect_fault = false;

    vaddr_t frame_addr = (t->stack_top - TASK_FRAME_GUARD) & ~0xFULL;
    struct isr_frame *f = (struct isr_frame *)frame_addr;
    uint64_t *q = (uint64_t *)f;
    for (size_t i = 0; i < sizeof(struct isr_frame) / 8; i++) q[i] = 0;

    q[QW_RIP] = entry_rip;
    q[QW_CS] = GDT_USER_CODE_RPL3;
    q[QW_RFLAGS] = TASK_RFLAGS_IF;
    q[QW_RSP] = user_rsp;
    q[QW_SS] = GDT_USER_DATA_RPL3;

    t->frame = f;
    t->in_use = true;
    t->debug_log = false;
    t->running = false;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved_blocked = 0;
    t->sig_in_handler = false;
    t->sig_frame_user = 0;
    t->sig_terminate = false;
    t->cpu = TASK_CPU_ANY;
    t->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_RELAXED);
    task_pgrp_join(t, 0, 0);
    t->name = name;

    fd_table_init(t);
    fd_install_stdio(t);

    lock_release(LOCK_SCHED, &sched_lock);

    kprintf(PRINT_SERIAL, "task: spawned %s in ring 3 (rip=0x%llx as=%p)\n", name, (unsigned long long)entry_rip, (void *)vs->pml4);
    return t;
}

struct task *task_spawn_ring3_lent(const char *name, paddr_t code_phys, size_t code_pages, vaddr_t code_virt, vaddr_t entry_rip, paddr_t shared_phys, vaddr_t shared_virt) {
    return spawn_ring3(name, code_phys, code_pages, code_virt, entry_rip, shared_phys, shared_virt, false);
}

static void task_release_wait(struct task *t, size_t idx) {
    (void)idx;
    __atomic_add_fetch(&t->wake_gen, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&t->blocked, false, __ATOMIC_RELEASE);
    task_wake_broadcast();
}

static struct task *pick_next(uint32_t me) {
    for (size_t i = 1; i <= TASK_MAX; i++) {
        size_t idx = (current_slot() + i) % TASK_MAX;
        struct task *t = &tasks[idx];
        if (!t->in_use || t->blocked || t->stopped) continue;
        if (t->running) continue;
        if (t->cpu != TASK_CPU_ANY && t->cpu != me) continue;
        if (idle_slot[me] < TASK_MAX && idx == idle_slot[me]) continue;
        return t;
    }
    return NULL;
}

static void task_zombify_locked(struct task *t) {
    if (!t || !t->in_use || t->zombie) return;
    fd_tty_task_gone(t->pid);
    fd_table_clear(t);
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (tasks[i].parent_pid == t->pid) tasks[i].parent_pid = 0;
    }
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (tasks[i].in_use && tasks[i].pid == t->parent_pid &&
            tasks[i].wait_chan == tasks[i].child_chan) {
            __atomic_store_n(&tasks[i].blocked, false, __ATOMIC_RELEASE);
        }
    }
    pgroup_unlink(t);
    t->in_use = false;
    t->zombie = true;
    t->blocked = false;
    t->frame = NULL;
    t->running = false;
}

static void task_switch_to(struct isr_frame *frame, bool force_away) {
    if (!sched_enabled()) return;
    if (in_scheduler()) return;

    if (sync_lock_depth() != 0) {
        panic_assert("context switch attempted while this task still holds a lock");
    }

    set_in_scheduler(true);

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);

    task_fpu_save();

    tasks[current_slot()].frame = frame;

    task_reap_zombies();

    struct task *next = pick_next(cpu_id());
    if (next && force_away && next == &tasks[current_slot()]) {
        for (size_t i = 1; i <= TASK_MAX; i++) {
            size_t idx = (current_slot() + i) % TASK_MAX;
            struct task *t = &tasks[idx];
            if (t->in_use && !t->blocked && !t->running) {
                if (t->cpu != TASK_CPU_ANY && t->cpu != cpu_id()) continue;
                next = t;
                break;
            }
        }
    }
    while (next && next != &tasks[current_slot()] && next->sig_terminate) {
        task_zombify_locked(next);
        next = pick_next(cpu_id());
        if (!next) break;
        if (next->cpu != TASK_CPU_ANY && next->cpu != cpu_id()) continue;
    }
    if (!next) {
        uint32_t me = cpu_id();
        if (idle_slot[me] < TASK_MAX && tasks[idle_slot[me]].in_use &&
            !tasks[idle_slot[me]].running && idle_slot[me] != current_slot())
            next = &tasks[idle_slot[me]];
    }
    if (next && next != &tasks[current_slot()]) {
        tasks[current_slot()].running = false;
        next->running = true;
        bump_switch_count();
        set_current_slot((size_t)(next - tasks));
    }


    if (!tasks[current_slot()].frame) {
        kprintf(PRINT_SERIAL, "task: no saved frame, cannot resume\n");
        for (;;) __asm__ volatile("hlt");
    }

    task_kstack_update();
    task_aspace_update();

    if (tasks[current_slot()].frame)
        task_sig_check(&tasks[current_slot()], tasks[current_slot()].frame);

    lock_release(LOCK_SCHED, &sched_lock);

    set_in_scheduler(false);
    task_resume(tasks[current_slot()].frame);
}

void task_init_cpu(void) {
    struct percpu *self = cpu_self();
    self->current_slot = 0;
    self->in_scheduler = false;
    self->sched_enabled = true;
    self->switch_count = 0;
    self->earliest_wake = UINT64_MAX;
}

bool task_is_idle_current(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return false;
    return current_slot() == idle_slot[cpu];
}

void smp_resched_handler(struct isr_frame *frame) {
    cpu_self()->resched_count++;
    if (cpu_self()->lapic_base) lapic_eoi(cpu_self()->lapic_base);

    if (preempt_count() != 0) {
        if (cpu_self()->lapic_base)
            lapic_timer_set(cpu_self()->lapic_base, LAPIC_TIMER_PERIOD, LAPIC_TIMER_DIV);
        return;
    }

    task_schedule(frame);

    if (cpu_self()->lapic_base)
        lapic_timer_set(cpu_self()->lapic_base,
                        task_is_idle_current(cpu_id()) ? LAPIC_IDLE_PERIOD : LAPIC_TIMER_PERIOD,
                        LAPIC_TIMER_DIV);
}

void task_schedule(struct isr_frame *frame) {
    task_switch_to(frame, false);
}

void task_yield(struct isr_frame *frame) {
    task_switch_to(frame, true);
}

static uint64_t exit_park_stack_top;

void task_exit_code(uint64_t code) {
    struct task *self = &tasks[current_slot()];

    self->exit_code = code;
    fd_tty_task_gone(self->pid);
    fd_table_clear(self);

    if (!self->expect_fault)
        kprintf(PRINT_SERIAL, "task: %s exited with code %llu\n", self->name, (unsigned long long)code);

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);

    if (self->parent_pid) {
        for (size_t i = 1; i < TASK_MAX; i++) {
            if (tasks[i].in_use && tasks[i].pid == self->parent_pid &&
                tasks[i].wait_chan == tasks[i].child_chan) {
                __atomic_store_n(&tasks[i].vfork_child, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&tasks[i].blocked, false, __ATOMIC_RELEASE);
            }
        }
    }

    for (size_t i = 1; i < TASK_MAX; i++) {
        if (tasks[i].in_use && tasks[i].vfork_child == self->pid &&
            tasks[i].wait_chan == tasks[i].vfork_chan) {
            __atomic_store_n(&tasks[i].vfork_child, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&tasks[i].blocked, false, __ATOMIC_RELEASE);
        }
    }

    for (size_t i = 1; i < TASK_MAX; i++) {
        if (tasks[i].parent_pid == self->pid)
            tasks[i].parent_pid = 0;
    }

    task_fpu_save();

    pgroup_unlink(self);
    self->in_use = false;
    self->blocked = false;
    self->zombie = true;
    self->frame = NULL;

    struct task *next = pick_next(cpu_id());
    if (!next && idle_slot[cpu_id()] < TASK_MAX &&
        tasks[idle_slot[cpu_id()]].in_use && !tasks[idle_slot[cpu_id()]].running)
        next = &tasks[idle_slot[cpu_id()]];
    if (next) {
        next->running = true;
        set_current_slot((size_t)(next - tasks));
        bump_switch_count();
    }

    task_kstack_update();
    task_aspace_update();

    lock_release(LOCK_SCHED, &sched_lock);

    if (!next || !tasks[current_slot()].frame) {
        kprintf(PRINT_SERIAL, "task: no task left to run\n");
        if (!exit_park_stack_top) {
            paddr_t pp;
            if (pmm_alloc(&pp, 2))
                exit_park_stack_top = vmm_hhdm_offset + pp + 2 * PAGE_SIZE;
        }
        if (exit_park_stack_top) {
            tss_set_rsp0((uint32_t)cpu_id(), exit_park_stack_top);
            __asm__ volatile("mov %0, %%rsp; sti"
                             :: "r"(exit_park_stack_top - TASK_FRAME_GUARD) : "memory");
        }
        for (;;) __asm__ volatile("sti; hlt" ::: "memory");
    }

    task_resume(tasks[current_slot()].frame);
}

void task_exit(void) {
    task_exit_code(0);
}

void task_block_current(uint64_t seen_seq) {
    if (__atomic_load_n(wake_seq_p(), __ATOMIC_ACQUIRE) != seen_seq) return;

    __asm__ volatile("cli" ::: "memory");

    lock_acquire(LOCK_SCHED, &sched_lock);
    tasks[current_slot()].blocked = true;
    lock_release(LOCK_SCHED, &sched_lock);

    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
        __asm__ volatile("cli" ::: "memory");

        lock_acquire(LOCK_SCHED, &sched_lock);
        bool woken = !tasks[current_slot()].blocked;
        lock_release(LOCK_SCHED, &sched_lock);

        if (woken) {
            __asm__ volatile("sti" ::: "memory");
            return;
        }
    }
}


bool task_wait_vfork_child(uint32_t child) {
    struct task *self = &tasks[current_slot()];

    lock_acquire(LOCK_SCHED, &sched_lock);

    bool done = false;
    for (size_t i = 1; i < TASK_MAX; i++) {
        if (tasks[i].zombie && tasks[i].pid == child) { done = true; break; }
    }

    if (!done) {
        self->vfork_child = child;
        self->wait_chan = &self->vfork_chan;
        self->wake_tick = 0;
        self->blocked = true;
    }
    lock_release(LOCK_SCHED, &sched_lock);

    if (done) return true;

    task_sleep_registered();
    self->vfork_child = 0;
    return true;
}

bool task_wait(void *chan, uint64_t deadline) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    struct task *self = &tasks[current_slot()];

    uint64_t seen = __atomic_load_n(&self->wake_gen, __ATOMIC_ACQUIRE);

    lock_acquire(LOCK_SCHED, &sched_lock);
    self->wait_chan = chan;
    self->wake_tick = deadline;
    if (deadline && deadline < earliest_wake()) set_earliest_wake(deadline);
    self->blocked = true;
    lock_release(LOCK_SCHED, &sched_lock);

    if (__atomic_load_n(&self->wake_gen, __ATOMIC_ACQUIRE) != seen) {
        __atomic_store_n(&self->blocked, false, __ATOMIC_RELEASE);
    }

    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
        __asm__ volatile("cli" ::: "memory");
        if (!__atomic_load_n(&self->blocked, __ATOMIC_ACQUIRE)) break;
    }

    bool timed_out = deadline && pit_get_ticks() >= deadline;
    self->wait_chan = NULL;
    self->wake_tick = 0;

    if (rflags & TASK_RFLAGS_IF) __asm__ volatile("sti" ::: "memory");
    return timed_out;
}

void task_wake_chan(void *chan) {
    if (!chan) return;
    for (size_t i = 0; i < TASK_MAX; i++) {
        if (tasks[i].in_use && tasks[i].wait_chan == chan) task_release_wait(&tasks[i], i);
    }
}

int task_futex_wait(void *chan, uintptr_t uaddr, uint32_t expected, uint64_t deadline) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    struct task *self = &tasks[current_slot()];

    uint64_t seen = __atomic_load_n(&self->wake_gen, __ATOMIC_ACQUIRE);

    lock_acquire(LOCK_SCHED, &sched_lock);
    self->wait_chan = chan;
    self->wake_tick = deadline;
    if (deadline && deadline < earliest_wake()) set_earliest_wake(deadline);
    self->blocked = true;
    lock_release(LOCK_SCHED, &sched_lock);

    if (__atomic_load_n(&self->wake_gen, __ATOMIC_ACQUIRE) != seen) {
        __atomic_store_n(&self->blocked, false, __ATOMIC_RELEASE);
    }

    uint32_t cur;
    if (!copyin(&cur, (const void *)uaddr, sizeof(cur))) {
        lock_acquire(LOCK_SCHED, &sched_lock);
        self->blocked = false;
        self->wait_chan = NULL;
        lock_release(LOCK_SCHED, &sched_lock);
        if (rflags & TASK_RFLAGS_IF) __asm__ volatile("sti" ::: "memory");
        return -14;
    }
    if (cur != expected) {
        bool woken = !__atomic_load_n(&self->blocked, __ATOMIC_ACQUIRE);
        lock_acquire(LOCK_SCHED, &sched_lock);
        self->blocked = false;
        self->wait_chan = NULL;
        self->wake_tick = 0;
        lock_release(LOCK_SCHED, &sched_lock);
        if (rflags & TASK_RFLAGS_IF) __asm__ volatile("sti" ::: "memory");
        return woken ? 0 : -11;
    }

    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
        __asm__ volatile("cli" ::: "memory");
        if (!__atomic_load_n(&self->blocked, __ATOMIC_ACQUIRE)) break;
    }

    bool timed_out = deadline && pit_get_ticks() >= deadline;
    self->wait_chan = NULL;
    self->wake_tick = 0;

    if (rflags & TASK_RFLAGS_IF) __asm__ volatile("sti" ::: "memory");
    return timed_out ? -110 : 0;
}

void task_tick(uint64_t now) {
    if (now < earliest_wake()) return;
    if (preempt_count() != 0) return;
    if (atomic_flag_test_and_set_explicit(&sched_lock.f, memory_order_acquire)) return;
    preempt_disable();

    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (!t->in_use || !t->blocked || !t->wake_tick) continue;
        if (now >= t->wake_tick) task_release_wait(t, i);
        else if (t->wake_tick < next) next = t->wake_tick;
    }
    atomic_flag_clear_explicit(&sched_lock.f, memory_order_release);
    preempt_enable();

    set_earliest_wake(next);
}

bool task_current_blocked(void) {
    return __atomic_load_n(&tasks[current_slot()].blocked, __ATOMIC_ACQUIRE);
}

uint64_t task_wake_seq(void) {
    return __atomic_load_n(wake_seq_p(), __ATOMIC_ACQUIRE);
}

void task_unblock_all(void) {
    __atomic_add_fetch(wake_seq_p(), 1, __ATOMIC_RELEASE);

    // 0, not 1: slot 0 is kmain, and kmain is the task parked on input. Every
    // other loop over tasks[] starts at 1, so this one looks like a nyonpo (only comment in the whole os btw)
    for (size_t i = 0; i < TASK_MAX; i++) {
        if (!tasks[i].in_use) continue;
        task_release_wait(&tasks[i], i);
    }
}

struct task *task_current(void) {
    return &tasks[current_slot()];
}

void task_reap_now(void) {
    lock_acquire(LOCK_SCHED, &sched_lock);
    task_reap_zombies();
    lock_release(LOCK_SCHED, &sched_lock);
}

struct task *task_find_zombie(uint32_t pid) {
    for (size_t i = 0; i < TASK_MAX; i++) {
        if (tasks[i].zombie && tasks[i].pid == pid) return &tasks[i];
    }
    return NULL;
}

struct task *task_find_pid(uint32_t pid) {
    for (size_t i = 0; i < TASK_MAX; i++) {
        if (tasks[i].in_use && !tasks[i].zombie && tasks[i].pid == pid) return &tasks[i];
    }
    return NULL;
}

struct task *task_at(size_t i) {
    if (i >= TASK_MAX) return NULL;
    return &tasks[i];
}

void task_kill_pgrp(uint32_t pgid, int sig) {
    if (sig < 0 || sig >= 65) return;
    if (!pgid) {
        for (size_t i = 1; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];
            if (!t->in_use || t->zombie) continue;
            __atomic_or_fetch(&t->sig_pending, 1ULL << (sig - 1), __ATOMIC_RELAXED);
        }
        return;
    }
    for (size_t gi = 0; gi < TASK_MAX; gi++) {
        struct pgroup *g = &pgroups[gi];
        if (!g->used || g->pgid != pgid) continue;
        for (uint32_t m = g->head; m; ) {
            struct task *t = &tasks[m - 1];
            m = t->pg_next;
            if (!t->in_use || t->zombie) continue;
            __atomic_or_fetch(&t->sig_pending, 1ULL << (sig - 1), __ATOMIC_RELAXED);
        }
    }
}

bool task_in_pgrp(uint32_t pgid) {
    struct task *self = task_current();
    if (!self) return false;
    if (!pgid) return true;
    return self->pgid == pgid;
}

bool task_is_leader(uint32_t pid) {
    for (size_t i = 0; i < TASK_MAX; i++)
        if (pgroups[i].used && pgroups[i].pgid == pid) return true;
    return false;
}

int task_set_pgid(uint32_t pid, uint32_t pgid) {
    struct task *self = task_current();
    if (!self) return -1;
    struct task *t = self;
    if (pid) {
        t = task_find_pid(pid);
        if (!t) return -3;
    }
    if (t->sid != self->sid) return -1;
    task_pgrp_join(t, pgid ? pgid : t->pid, t->sid);
    return 0;
}

bool task_pgrp_exists(uint32_t pgid) {
    for (size_t i = 0; i < TASK_MAX; i++)
        if (pgroups[i].used && pgroups[i].pgid == pgid) return true;
    return false;
}

bool task_pgrp_orphaned(uint32_t pgid) {
    for (size_t gi = 0; gi < TASK_MAX; gi++) {
        struct pgroup *g = &pgroups[gi];
        if (!g->used || g->pgid != pgid) continue;
        for (uint32_t m = g->head; m; ) {
            struct task *t = &tasks[m - 1];
            m = t->pg_next;
            if (t->zombie || !t->parent_pid) continue;
            struct task *p = task_find_pid(t->parent_pid);
            if (p && p->sid == t->sid && p->pgid != pgid) return false;
        }
    }
    return true;
}

struct page_table *task_current_pml4(void) {
    return tasks[current_slot()].pml4;
}

uint64_t task_switch_count(void) {
    return switch_count();
}
