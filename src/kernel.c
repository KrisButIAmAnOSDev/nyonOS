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

    gdt_init(hhdm_offset);

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
    cpu_detect();
    cpu_enable_nx();
    cpu_enable_smep_smap();
    cpu_enable_sse_user();
    cpu_enable_pcid();
    crypto_init();
    syscall_init();
    irq_install(0, pit_handler);
    irq_install(1, keyboard_handler);
    keyboard_init();

    kprintf(PRINT_SERIAL, "nyonOS: init done\n");

    sync_init();
    heap_init();

    if (!ata_init()) kprintf(PRINT_SERIAL, "ATA: no drive found\n");

    task_init();
    task_spawn("aqua", task_aqua);
    task_spawn("seth", task_seth);

    fs_test();

    kprintf_clear();
    kprintf_home(0, 0);
    kprintf(KATTR(PRINT_BOTH, COLOR_MAGENTA), "nyonn nyon nyonn ulelelel nyon leleel nyonn\n");
    kprintf(KATTR(PRINT_SCREEN, COLOR_CYAN), "kawkaw from deltarune\n");
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

    for (;;) {
        uint64_t seq = task_wake_seq();
        keyboard_process_buffer();
        task_block_current(seq);
    }
}
