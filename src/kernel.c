#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot/limine/limine.h"
#include "graphics/font/font.h"
#include "graphics/video/video.h"
#include "drivers/serial/serial.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/pic/pic.h"
#include "drivers/keyboard/keyboard.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "arch/x86_64/pit/pit.h"
#include "kernel/multitask/task.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/lapic/lapic.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/gdt/gdt.h"
#include "drivers/ata/ata.h"

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
        pmm_free(code_phys, code_pages);
        pmm_free(results_phys, 1);
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

    pmm_free(code_phys, code_pages);
    pmm_free(results_phys, 1);
}

static void ata_test(void) {
    static uint8_t scratch[ATA_SECTOR_SIZE];
    static uint8_t saved[ATA_SECTOR_SIZE];
    static uint8_t verify[ATA_SECTOR_SIZE];
    static uint8_t big[ATA_SECTOR_SIZE * 400];

    kprintf(PRINT_SERIAL, "ATA test: probing\n");
    bool any = ata_init();
    kprintf(PRINT_SERIAL, "  drives found: %d\n", ata_drive_count());

    int found = -1;
    int optical = 0;
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        ata_drive_t *d = ata_get_drive(i);
        if (!d || !d->present) continue;
        if (d->atapi) {
            optical++;
            continue;
        }
        found = i;
        kprintf(PRINT_SERIAL, "  [%d] %s\n", i, d->model);
        kprintf(PRINT_SERIAL, "      serial %s\n", d->serial);
        kprintf(PRINT_SERIAL, "      sectors %u (%u MB)  lba48 %s\n", d->sectors, d->sectors / 2048, d->lba48 ? "yes" : "no");
    }
    if (optical) kprintf(PRINT_SERIAL, "  (%d optical device(s) skipped)\n", optical);

    if (found < 0) {
        kprintf(PRINT_SERIAL, "ATA test: no drive present: N\n");
        return;
    }

    kprintf(PRINT_SERIAL, "  testing drive %d (%s)\n", found, ata_get_drive(found)->model);

    bool lba28_read = ata_read_sectors(found, 0, 1, scratch);
    kprintf(PRINT_SERIAL, "  read sector 0: ");
    kprintchar(lba28_read ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  boot signature 0x55AA: ");
    kprintchar((scratch[510] == 0x55 && scratch[511] == 0xAA) ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    bool repeat = ata_read_sectors(found, 0, 1, verify);
    bool stable = repeat;
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
        if (verify[i] != scratch[i]) stable = false;
    }
    kprintf(PRINT_SERIAL, "  read twice, identical: ");
    kprintchar(stable ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    bool multi = ata_read_sectors(found, 0, 4, verify);
    bool multi_same = multi;
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
        if (verify[i] != scratch[i]) multi_same = false;
    }
    kprintf(PRINT_SERIAL, "  multi-sector read 4 agrees: ");
    kprintchar(multi_same ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    uint32_t scratch_lba = 100000;
    bool got_scratch = ata_read_sectors(found, scratch_lba, 1, saved);

    for (int i = 0; i < ATA_SECTOR_SIZE; i++) scratch[i] = (uint8_t)(i * 7 + 13);

    bool wrote = ata_write_sectors(found, scratch_lba, 1, scratch);
    bool read_back = wrote && ata_read_sectors(found, scratch_lba, 1, verify);
    bool match = read_back;
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
        if (verify[i] != scratch[i]) match = false;
    }
    kprintf(PRINT_SERIAL, "  write then read back lba %u: ", scratch_lba);
    kprintchar(match ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    bool boot_intact = ata_read_sectors(found, 0, 1, verify) &&
                       verify[510] == 0x55 && verify[511] == 0xAA;
    kprintf(PRINT_SERIAL, "  boot sector untouched: ");
    kprintchar(boot_intact ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    bool restored = true;
    if (got_scratch) restored = ata_write_sectors(found, scratch_lba, 1, saved);
    if (got_scratch && restored) {
        restored = ata_read_sectors(found, scratch_lba, 1, verify);
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
            if (verify[i] != saved[i]) restored = false;
        }
    }
    kprintf(PRINT_SERIAL, "  scratch sector restored: ");
    kprintchar(restored ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    bool big_ok = ata_read_sectors(found, 0, 400, big);
    for (int i = 0; i < 4 && big_ok; i++) {
        if (!ata_read_sectors(found, (uint32_t)i, 1, verify)) { big_ok = false; break; }
        for (int j = 0; j < ATA_SECTOR_SIZE; j++) {
            if (verify[j] != big[i * ATA_SECTOR_SIZE + j]) { big_ok = false; break; }
        }
    }
    kprintf(PRINT_SERIAL, "  400-sector read (2 chunks) consistent: ");
    kprintchar(big_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    bool rejected = !ata_read_sectors(found, 0x0FFFFF00u, 1, scratch);
    kprintf(PRINT_SERIAL, "  out of range lba refused: ");
    kprintchar(rejected ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "ATA test: drive usable: ");
    kprintchar((any && stable && multi_same && match && restored) ? 'Y' : 'N', PRINT_SERIAL);
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
    kprintf(PRINT_SERIAL, "nyonOS: Kernel loaded!\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        kprintf(PRINT_SERIAL, "nyonOS: Limine revision not supported!\n");
        for (;;) __asm__ volatile("hlt");
    }
    kprintf(PRINT_SERIAL, "nyonOS: Limine revision OK!\n");

    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        kprintf(PRINT_SERIAL, "nyonOS: No framebuffer!\n");
        for (;;) __asm__ volatile("hlt");
    }
    kprintf(PRINT_SERIAL, "nyonOS: Framebuffer found!\n");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) {
        kprintf(PRINT_SERIAL, "nyonOS: Wrong framebuffer format!\n");
        for (;;) __asm__ volatile("hlt");
    }
    kprintf(PRINT_SERIAL, "nyonOS: Framebuffer format OK!\n");

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
    gdt_dump();

    paddr_t lapic_phys = 0xFEE00000;
    vaddr_t lapic_virt = hhdm_offset + lapic_phys;
    if (!vmm_map(lapic_virt, lapic_phys, 1, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD)) {
       kprintf(PRINT_SERIAL, "nyonOS: Failed to map LAPIC!\n");
       for (;;) __asm__ volatile("hlt");
    }

    pic_remap(0x20, 0x28);
    pic_clear_mask(2);
    pit_init(1000);
    lapic_unmask_ext_int(lapic_virt);
    idt_init();
    syscall_init();
    irq_install(0, pit_handler);
    irq_install(1, keyboard_handler);
    keyboard_init();

    kprintf(PRINT_SERIAL, "nyonOS: init done\n");

    heap_init();

    ata_test();

    sync_init();
    task_init();
    task_spawn("aqua", task_aqua);
    task_spawn("seth", task_seth);

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
                if (!task_spawn_ring3("ring3fault", uphys, code_pages, TASK_USER_CODE_VIRT, fault_rip, 0, 0)) {
                    pmm_free(uphys, code_pages);
                    kprintf(PRINT_SERIAL, "RING3 test: fault task spawn FAILED\n");
                } else {
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
    }

    for (;;) {
        keyboard_process_buffer();
        task_block_current();
    }
}
