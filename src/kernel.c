#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot/limine/limine.h"
#include "graphics/font/font.h"
#include "drivers/serial/serial.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/pic/pic.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/pci/pci.h"
#include "kernel/tty/tty.h"
#include "arch/x86_64/cpu/cpu.h"
#include "kernel/crypto/crypto.h"
#include "kernel/exec/elf.h"
#include "kernel/mm/vmm/vmspace.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/rtc/rtc.h"
#include "kernel/multitask/task.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/lapic/lapic.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/gdt/gdt.h"
#include "drivers/ata/ata.h"
#include "kernel/fd/fd.h"
#include "kernel/block/block.h"
#include "kernel/fs/fs.h"
#include "test/test.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_paging_mode_request limine_paging_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request limine_mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request limine_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

extern uint8_t user_test_entry[];

static void validate_spawn(vaddr_t entry_rip, size_t code_bytes, size_t code_pages) {
    paddr_t code_phys, results_phys;

    if (!pmm_alloc(&code_phys, code_pages) || !pmm_alloc(&results_phys, 1)) {
        kprintf(PRINT_SERIAL, "SYSCALL test: alloc failed\n");
        return;
    }

    volatile uint8_t *dst = (volatile uint8_t *)(vmm_hhdm_offset + code_phys);
    for (size_t i = 0; i < code_bytes; i++) dst[i] = user_test_entry[i];

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + results_phys);
    for (int i = 0; i < 10; i++) res[i] = 0xdeadbeefdeadbeefULL;

    struct task *t = task_spawn_ring3("ring3validate", code_phys, code_pages, TASK_USER_CODE_VIRT, entry_rip, results_phys, 0x60000000ULL);
    if (!t) {
        pmm_free(code_phys, code_pages);
        pmm_free(results_phys, 1);
        kprintf(PRINT_SERIAL, "SYSCALL test: spawn failed\n");
        return;
    }

    for (int i = 0; i < 300 && res[9] == 0xdeadbeefdeadbeefULL; i++) pit_sleep(10);

    if (res[9] == 0xdeadbeefdeadbeefULL) {
        kprintf(PRINT_SERIAL, "SYSCALL test: validation task never reported\n");
        return;
    }

    int64_t expect[9] = { 4, SYS_EINVAL, SYS_EINVAL, SYS_EINVAL, SYS_EFAULT, SYS_EFAULT, SYS_EFAULT, SYS_EFAULT, SYS_ENOSYS };
    const char *what[9] = {
        "valid write returns byte count",
        "bad destination rejected",
        "null buffer rejected",
        "zero length rejected",
        "kernel pointer rejected",
        "unmapped user pointer rejected",
        "range running past mapped page rejected",
        "page boundary straddling the user stack rejected",
        "unknown syscall number rejected"
    };

    int bad = 0;
    for (int i = 0; i < 9; i++) {
        int64_t got = (int64_t)res[i];
        if (got == expect[i]) continue;
        bad++;
        kprintf(PRINT_SERIAL, "  %s: got %lld expected %lld\n", what[i], (long long)got, (long long)expect[i]);
    }

    kprintf(PRINT_SERIAL, "SYSCALL test: ring 3 write validation, %d of 9 correct: ", 9 - bad);
    kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

static void busy_wait_ms(uint64_t ms) {
    uint64_t start = pit_get_ticks();
    while (pit_get_ticks() - start < ms) __asm__ volatile("hlt");
}

static uint64_t smp_hhdm_offset;
static volatile uint32_t smp_go[MAX_CPUS];

static void cpu_idle(void) {
    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

static void bsp_mp_done(struct limine_mp_info *info) {
    (void)info;
}

static uint64_t ap_stack_top[MAX_CPUS];

static void ap_entry(struct limine_mp_info *info) {
    if (!info) { cpu_idle(); return; }

    uint32_t id = info->processor_id;
    uint64_t top = (id < MAX_CPUS) ? ap_stack_top[id] : 0;
    if (top) __asm__ volatile("mov %0, %%rsp" :: "r"(top) : "memory");

    vmm_switch_address_space(kernel_pml4);

    extern void isr_stub_resched(void);
    idt_install(LAPIC_RESCHED_VECTOR, (void *)isr_stub_resched, 0x8E);
    idt_reload();
    cpu_bootstrap_ap(id, info->lapic_id, smp_hhdm_offset);

    while (id >= MAX_CPUS || !__atomic_load_n(&smp_go[id], __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }

    task_init_cpu();
    lapic_timer_set(cpu_self()->lapic_base, LAPIC_TIMER_PERIOD, LAPIC_TIMER_DIV);

    __asm__ volatile("sti" ::: "memory");

    for (;;) __asm__ volatile("hlt" ::: "memory");
}

static void lock_invariant_test(void) {
    spinlock_t probe = SPINLOCK_INIT;
    uint32_t depth0 = sync_lock_depth();
    uint32_t pre0 = preempt_count();

    lock_acquire(LOCK_HEAP, &probe);
    bool depth_inside = sync_lock_depth() == depth0 + 1;
    busy_wait_ms(40);
    lock_release(LOCK_HEAP, &probe);

    uint32_t depth1 = sync_lock_depth();
    uint32_t pre1 = preempt_count();

    bool ok = depth_inside && depth1 == depth0 && pre0 == 0 && pre1 == 0;

    kprintf(PRINT_SERIAL, "LOCK test: lock across a timer tick survived: ");
    kprintchar(ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  depth %u->%u preempt %u->%u cpu %u\n",
            depth0, depth1, pre0, pre1, cpu_id());
}


static void task_aqua(void) {
    for (int round = 0; round < 4; round++) {
        char *p = kmalloc(64);
        if (!p) { kprintf(PRINT_SERIAL, "[aqua] kmalloc failed\n"); task_exit(); }
        for (int i = 0; i < 64; i++) p[i] = (char)0xA5;
        busy_wait_ms(60);
        for (int i = 0; i < 64; i++) {
            if (p[i] != (char)0xA5) { kprintf(PRINT_SERIAL, "[aqua] DATA CORRUPTED\n"); }
        }
        kprintf(PRINT_SERIAL, "[aqua] pass ");
        kprintf(PRINT_SERIAL, "%llu", (unsigned long long)((uint64_t)round));
        kprintchar('\n', PRINT_SERIAL);
        kfree(p);
    }
    task_exit();
}

static void task_seth(void) {
    for (int round = 0; round < 4; round++) {
        char *p = kmalloc(64);
        if (!p) { kprintf(PRINT_SERIAL, "[seth] kmalloc failed\n"); task_exit(); }
        for (int i = 0; i < 64; i++) p[i] = (char)0x5A;
        busy_wait_ms(60);
        for (int i = 0; i < 64; i++) {
            if (p[i] != (char)0x5A) { kprintf(PRINT_SERIAL, "[seth] DATA CORRUPTED\n"); }
        }
        kprintf(PRINT_SERIAL, "[seth] pass ");
        kprintf(PRINT_SERIAL, "%llu", (unsigned long long)((uint64_t)round));
        kprintchar('\n', PRINT_SERIAL);
        kfree(p);
    }
    task_exit();
}

void kmain(void) {
    serial_init();

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        kprintf(PRINT_SERIAL, "nyonOS: Limine revision not supported!\n");
        for (;;) __asm__ volatile("hlt");
    }

    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        kprintf(PRINT_SERIAL, "nyonOS: No framebuffer!\n");
        for (;;) __asm__ volatile("hlt");
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) {
        kprintf(PRINT_SERIAL, "nyonOS: Wrong framebuffer format!\n");
        for (;;) __asm__ volatile("hlt");
    }

    kprintf_attach((volatile uint32_t *)fb->address, fb->width, fb->height);
    tty_set_display_size((uint16_t)fb->height, (uint16_t)fb->width);

    if (memmap_request.response == NULL) {
        kprintf(PRINT_SERIAL, "nyonOS: No memmap response!\n");
        for (;;) __asm__ volatile("hlt");
    }

    if (hhdm_request.response == NULL) {
        kprintf(PRINT_SERIAL, "nyonOS: No HHDM response!\n");
        for (;;) __asm__ volatile("hlt");
    }

    uint64_t hhdm_offset = hhdm_request.response->offset;

    pmm_init(memmap_request.response, hhdm_offset);
    vmm_init(hhdm_offset);

    gdt_init(hhdm_offset, 0);

    cpu_bootstrap();

    paddr_t lapic_phys = 0xFEE00000;
    vaddr_t lapic_virt = hhdm_offset + lapic_phys;
    if (!vmm_map(lapic_virt, lapic_phys, 1, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD)) {
       kprintf(PRINT_SERIAL, "nyonOS: Failed to map LAPIC!\n");
       for (;;) __asm__ volatile("hlt");
    }

    pic_remap(0x20, 0x28);
    pic_clear_mask(2);
    idt_init();
    pit_init(1000);
    rtc_init();
    lapic_unmask_ext_int(lapic_virt);

    if (limine_paging_request.response) {
        cpu_paging_mode_report(limine_paging_request.response->mode, true);
    }

    smp_hhdm_offset = hhdm_offset;

    cpu_mark_online(0, 0);
    cpu_detect();
    cpu_enable_nx();
    cpu_enable_smep_smap();
    cpu_enable_sse_user();
    cpu_enable_pcid();

    kprintf(PRINT_SERIAL, "CPU: limine says %s paging, vmm says %s, agree: %c\n",
            cpu_paging_mode_name(), vmm_5level ? "5-level" : "4-level",
            cpu_paging_mode_matches() ? 'Y' : 'N');
    {
        uint32_t ncpu = 1;
        struct limine_mp_response *mp = (struct limine_mp_response *)limine_mp_request.response;
        if (mp) {
            ncpu = (uint32_t)mp->cpu_count;
            if (ncpu > MAX_CPUS) ncpu = MAX_CPUS;
            for (uint32_t i = 0; i < ncpu; i++) {
                struct limine_mp_info *mi = mp->cpus[i];
                if (!mi) continue;
                mi->goto_address = (i == 0) ? bsp_mp_done : ap_entry;
            }
            cpu_set_detected(ncpu);
        }

        for (uint32_t i = 1; i < ncpu; i++) {
            paddr_t sp = 0;
            if (pmm_alloc(&sp, 4)) ap_stack_top[i] = vmm_hhdm_offset + sp + 4 * PAGE_SIZE;
            __atomic_store_n(&smp_go[i], 1, __ATOMIC_RELEASE);
        }

        if (mp && ncpu > 1) mp->cpus[0]->goto_address(mp->cpus[0]);

        {
            uint64_t start = cpu_rdtsc();
            while (cpu_online_count() < ncpu && cpu_rdtsc() - start < 20000000000ULL) {
                __asm__ volatile("pause" ::: "memory");
            }
        }

        kprintf(PRINT_SERIAL, "SMP: limine reports %u cpus, online %u, bsp lapic_id=%u\n",
                ncpu, cpu_online_count(),
                (unsigned)lapic_id_read(hhdm_offset + LAPIC_DEFAULT_PHYS));
    }

    kprintf(PRINT_SERIAL, "CPU: percpu self=%p id=%u online=%u of %u\n",
            (void *)cpu_self(), cpu_id(), cpu_online_count(), cpu_count());

    crypto_init();
    syscall_init();
    irq_install(0, pit_handler);
    irq_install(1, keyboard_handler);
    keyboard_init();

    kprintf(PRINT_SERIAL, "nyonOS: init done\n");

    sync_init();
    heap_init();

    if (!ata_init()) kprintf(PRINT_SERIAL, "ATA: no drive found\n");

    pci_init();
    pci_assign_bars();
    for (int i = 0; i < pci_device_count(); i++) {
        pci_device_t *dev = (pci_device_t *)pci_device_at(i);
        pci_program_bars(dev);
    }

    task_init();
    lock_invariant_test();
    task_spawn("aqua", task_aqua);
    task_spawn("seth", task_seth);

    fs_test();

    kprintf_clear();
    kprintf_home(0, 0);
    kprintf(PRINT_BOTH, "\x1b[35mnyonn nyon nyonn ulelelel nyon leleel nyonn\x1b[0m\n");
    kprintf(PRINT_SCREEN, "\x1b[36mkawkaw from deltarune\x1b[0m\n");
    kprintf(PRINT_SERIAL, "ululululululelelleelele uleeelle nyon -another kawkaw\n");

    {
        extern uint8_t user_fault_entry[];
        extern uint8_t user_validate_entry[];
        extern uint8_t user_test_end[];

        size_t code_bytes = (size_t)(user_test_end - user_test_entry);
        size_t code_pages = (code_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        vaddr_t fault_rip = TASK_USER_CODE_VIRT + (size_t)(user_fault_entry - user_test_entry);
        vaddr_t validate_rip = TASK_USER_CODE_VIRT + (size_t)(user_validate_entry - user_test_entry);

        size_t free_before = pmm_free_pages();
        int spawned = 0;

        for (int i = 0; i < 2; i++) {
            paddr_t uphys;
            if (!pmm_alloc(&uphys, code_pages)) break;

            volatile uint8_t *dst = (volatile uint8_t *)(vmm_hhdm_offset + uphys);
            for (size_t k = 0; k < code_bytes; k++) dst[k] = user_test_entry[k];

            if (!task_spawn_ring3("ring3", uphys, code_pages, TASK_USER_CODE_VIRT, TASK_USER_CODE_VIRT, 0, 0)) {
                pmm_free(uphys, code_pages);
                break;
            }
            spawned++;
        }

        kprintf(PRINT_SERIAL, "RING3 test: spawned %d of 2 concurrent ring 3 tasks: ", spawned);
        kprintchar(spawned == 2 ? 'Y' : 'N', PRINT_SERIAL);
        kprintchar('\n', PRINT_SERIAL);

        {
            paddr_t uphys;
            if (pmm_alloc(&uphys, code_pages)) {
                volatile uint8_t *dst = (volatile uint8_t *)(vmm_hhdm_offset + uphys);
                for (size_t k = 0; k < code_bytes; k++) dst[k] = user_test_entry[k];
                struct task *ft = task_spawn_ring3("ring3fault", uphys, code_pages, TASK_USER_CODE_VIRT, fault_rip, 0, 0);
                if (!ft) {
                    pmm_free(uphys, code_pages);
                    kprintf(PRINT_SERIAL, "RING3 test: fault task spawn FAILED\n");
                } else {
                    ft->expect_fault = true;
                    kprintf(PRINT_SERIAL, "RING3 test: fault task spawned\n");
                }
            } else {
                kprintf(PRINT_SERIAL, "RING3 test: fault task alloc FAILED\n");
            }
        }

        kprintf(PRINT_SERIAL, "RING3 test: free pages before=%u\n", (unsigned)free_before);

        pit_sleep(800);

        size_t free_after = pmm_free_pages();
        kprintf(PRINT_SERIAL, "RING3 test: kernel survived the fault: ");
        kprintchar(task_switch_count() > 0 ? 'Y' : 'N', PRINT_SERIAL);
        kprintf(PRINT_SERIAL, "  free pages after=%u reclaimed: ", (unsigned)free_after);
        kprintchar(free_after >= free_before ? 'Y' : 'N', PRINT_SERIAL);
        kprintchar('\n', PRINT_SERIAL);

        validate_spawn(validate_rip, code_bytes, code_pages);

        {
            extern uint8_t user_tty_a_entry[];
            extern uint8_t user_tty_b_entry[];
            extern uint8_t user_tty_c_entry[];
            tty_isolation_test(TASK_USER_CODE_VIRT + (size_t)(user_tty_a_entry - user_test_entry),
                               TASK_USER_CODE_VIRT + (size_t)(user_tty_b_entry - user_test_entry),
                               TASK_USER_CODE_VIRT + (size_t)(user_tty_c_entry - user_test_entry),
                               code_bytes, code_pages);
            extern uint8_t user_wait_entry[];
            wait_test(TASK_USER_CODE_VIRT + (size_t)(user_wait_entry - user_test_entry), code_bytes, code_pages);
        }
        {
            fd_test();
            mm_test();
            pipe_test();
            fswrite_test();
            fd2_test();
            futex_pair_test();
            exectest_run();
            mandel_run();
            featuretest_run();
            proctest_run();
            clktest_run();
            forktest_run();
            sysdep_test();
            thread_test();
            mlibc_hello_test();
        }

        {
            int bad = 0;
            elf_reject_test(&bad);
            kprintf(PRINT_SERIAL, "ELF reject: %d of 15 corrupt binaries refused: ", 15 - bad);
            kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
            kprintchar('\n', PRINT_SERIAL);
        }

        elf_run_test();
    }

    shell_run();

    {
        uint32_t n = cpu_count();
        uint64_t total = 0;
        for (uint32_t i = 0; i < n; i++) total += cpu_resched_count(i);
        kprintf(PRINT_SERIAL, "SMP: %llu resched total across %u cpus\n",
                (unsigned long long)total, n);
        for (uint32_t i = 0; i < n; i++) {
            kprintf(PRINT_SERIAL, "SMP: cpu%u resched %llu switches %llu\n", i,
                    (unsigned long long)cpu_resched_count(i),
                    (unsigned long long)cpu_switch_count(i));
        }
    }

    for (;;) {
        uint64_t seq = task_wake_seq();
        keyboard_process_buffer();
        task_block_current(seq);
    }
}
