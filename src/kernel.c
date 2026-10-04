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
#include "kernel/fd/fd.h"
#include "kernel/block/block.h"
#include "kernel/fs/fs.h"

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

    pmm_free(results_phys, 1);
}

static bool mount_first_fat(int *out_drive) {
    static block_device_t whole;
    static mbr_entry_t ents[BLOCK_MAX_PARTITIONS];
    static partition_t part;

    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        ata_drive_t *d = ata_get_drive(i);
        if (!d || !d->present || d->atapi) continue;
        if (!block_bind_ata(&whole, i)) continue;

        int nparts = mbr_parse(&whole, ents, BLOCK_MAX_PARTITIONS);
        block_device_t *target = &whole;
        bool via_part = false;

        for (int p = 0; p < nparts; p++) {
            if (!mbr_is_fat(ents[p].type)) continue;
            if (!partition_init(&part, &whole, ents[p].start_lba, ents[p].sector_count)) continue;
            target = &part.dev;
            via_part = true;
            break;
        }

        if (!fs_mount(target)) continue;

        *out_drive = i;
        kprintf(PRINT_SERIAL, "  drive %d: %d mbr entries, mounted %s\n",
                i, nparts, via_part ? "FAT partition" : "whole device");
        return true;
    }

    return false;
}

static char txt_byte(uint32_t i) {
    static const char head[] = "that my jarona";
    uint32_t hl = sizeof(head) - 1;
    if (i < hl) return head[i];
    return '.';
}

static void fs_test(void) {
    static char buf[4096] __attribute__((aligned(4)));
    static uint8_t bigbuf[4096] __attribute__((aligned(4)));
    static const char inner_expect[] = "inner payload\n";

    kprintf(PRINT_SERIAL, "FS test: probing disks for a FAT volume\n");

    int drv = -1;
    if (!mount_first_fat(&drv)) {
        kprintf(PRINT_SERIAL, "  no FAT volume found\n");
        return;
    }

    bool m = true;
    kprintf(PRINT_SERIAL, "  mounted: ");
    kprintchar(m ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    if (!m) return;

    fs_volume_t *f = fs_get();
    kprintf(PRINT_SERIAL, "  %s label '%s' bpbsig '%s'\n", fs_fat_name(f->cluster_count), f->label, f->fs_type);
    kprintf(PRINT_SERIAL, "  bytes/sector %u sectors/cluster %u fats %u root entries %u\n",
            f->bytes_per_sector, f->sectors_per_cluster, f->fat_count, f->root_entry_count);
    kprintf(PRINT_SERIAL, "  total sectors %u sectors/fat %u data_start %u clusters %u\n",
            f->total_sectors, f->sectors_per_fat, f->data_start, f->cluster_count);

    fs_node_t root;
    if (!fs_lookup("/", &root)) {
        kprintf(PRINT_SERIAL, "  root node: N\n");
        return;
    }

    uint32_t n = 0;
    fs_node_t e;
    while (n < 8 && fs_iterate(&root, n, &e)) {
        kprintf(PRINT_SERIAL, "  [%u] %s %s cluster %u size %u\n", n, e.name, e.is_dir ? "DIR" : "FILE", e.cluster, e.size);
        n++;
    }
    kprintf(PRINT_SERIAL, "  root entries listed: %u\n", n);

    fs_node_t file;
    bool got = fs_lookup("/TEST.TXT", &file);
    kprintf(PRINT_SERIAL, "  lookup /TEST.TXT: ");
    kprintchar(got ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    if (!got) return;

    uint32_t read = fs_node_read_all(&file, bigbuf, sizeof(bigbuf));
    bool size_ok = read == file.size && read == 3000;
    bool content_ok = size_ok;
    for (uint32_t i = 0; content_ok && i < read; i++) {
        if (bigbuf[i] != (uint8_t)txt_byte(i)) content_ok = false;
    }
    kprintf(PRINT_SERIAL, "  size %u (dir says %u): ", read, file.size);
    kprintchar(size_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  content matches: ");
    kprintchar(content_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    uint32_t shown = read < 20 ? read : 20;
    kprintf(PRINT_SERIAL, "  contents: ");
    for (uint32_t i = 0; i < shown; i++) kprintchar((char)bigbuf[i], PRINT_SERIAL);
    if (read > shown) kprintf(PRINT_SERIAL, "... (%u bytes total)", read);
    kprintchar('\n', PRINT_SERIAL);

    uint32_t clus_size = (uint32_t)fs_get()->sectors_per_cluster * fs_get()->bytes_per_sector;
    char edge[16];
    bool edge_ok = fs_node_read(&file, clus_size - 4, edge, 16);
    for (int i = 0; edge_ok && i < 16; i++) {
        if ((uint8_t)edge[i] != (uint8_t)txt_byte(clus_size - 4 + (uint32_t)i)) edge_ok = false;
    }
    kprintf(PRINT_SERIAL, "  spans %u clusters, boundary read: ", (read + clus_size - 1) / clus_size);
    kprintchar(edge_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    char partial[8];
    bool p1 = fs_node_read(&file, 5, partial, 4);
    bool p2 = p1 && partial[0] == 'm' && partial[1] == 'y' && partial[2] == ' ' && partial[3] == 'j';
    bool oob = !fs_node_read(&file, file.size, partial, 1);
    bool missing = !fs_lookup("/NOPE.TXT", &e);
    kprintf(PRINT_SERIAL, "  offset read of 'my j': ");
    kprintchar(p2 ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  read past eof refused: ");
    kprintchar(oob ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  missing file refused: ");
    kprintchar(missing ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    fs_node_t sub;
    bool got_sub = fs_lookup("/SUB", &sub) && sub.is_dir;
    uint32_t sub_count = 0;
    fs_node_t it;
    while (got_sub && fs_iterate(&sub, sub_count, &it)) {
        kprintf(PRINT_SERIAL, "    SUB[%u] %s %s %u\n", sub_count, it.name, it.is_dir ? "DIR" : "FILE", it.size);
        sub_count++;
    }
    kprintf(PRINT_SERIAL, "  /SUB is a dir: ");
    kprintchar(got_sub ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  entries (dot skipped): %u\n", sub_count);

    fs_node_t inner;
    bool got_inner = fs_lookup("/SUB/INNER.TXT", &inner);
    uint32_t inner_read = got_inner ? fs_node_read_all(&inner, buf, sizeof(buf)) : 0;
    bool inner_ok = got_inner && inner_read == sizeof(inner_expect) - 1;
    for (uint32_t i = 0; inner_ok && i < inner_read; i++) {
        if (buf[i] != inner_expect[i]) inner_ok = false;
    }
    kprintf(PRINT_SERIAL, "  /SUB/INNER.TXT content: ");
    kprintchar(inner_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    if (inner_read && inner_read < sizeof(buf)) {
        buf[inner_read] = 0;
        kprintf(PRINT_SERIAL, "  contents: %s\n", buf);
    }

    bool bad_path = !fs_lookup("/SUB/NOPE.TXT", &it) && !fs_lookup("/TESTTX.TXT", &it);
    bool dir_read_refused = !fs_node_read(&sub, 0, buf, 16);
    kprintf(PRINT_SERIAL, "  bad paths refused: ");
    kprintchar(bad_path ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  read on a directory refused: ");
    kprintchar(dir_read_refused ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "FS test: usable: ");
    kprintchar((n == 2 && size_ok && content_ok && edge_ok && p2 && oob && missing &&
                got_sub && sub_count == 1 && inner_ok && bad_path && dir_read_refused) ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

static void fd_spawn(vaddr_t entry_rip, size_t code_bytes, size_t code_pages) {
    paddr_t code_phys, results_phys;

    if (!pmm_alloc(&code_phys, code_pages) || !pmm_alloc(&results_phys, 1)) {
        kprintf(PRINT_SERIAL, "FD ring3: alloc failed\n");
        return;
    }

    volatile uint8_t *dst = (volatile uint8_t *)(vmm_hhdm_offset + code_phys);
    for (size_t i = 0; i < code_bytes; i++) dst[i] = user_test_entry[i];

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + results_phys);
    for (int i = 0; i < FD_RES_SLOTS; i++) res[i] = 0xdeadbeefdeadbeefULL;

    struct task *t = task_spawn_ring3("ring3fd", code_phys, code_pages, TASK_USER_CODE_VIRT,
                                      entry_rip, results_phys, 0x60000000ULL);
    if (!t) {
        pmm_free(code_phys, code_pages);
        pmm_free(results_phys, 1);
        kprintf(PRINT_SERIAL, "FD ring3: spawn failed\n");
        return;
    }

    for (int i = 0; i < 400 && res[FD_RES_DONE] == 0xdeadbeefdeadbeefULL; i++) pit_sleep(10);

    if (res[FD_RES_DONE] == 0xdeadbeefdeadbeefULL) {
        kprintf(PRINT_SERIAL, "FD ring3: task never reported\n");
        return;
    }

    static const char *what[FD_CHECKS] = {
        "open /TEST.TXT returns an fd",
        "read 14 bytes from ring 3",
        "content matches that my jarona",
        "lseek SET 0 resets position",
        "re-read after lseek returns 4",
        "lseek past end clamps to file size",
        "read at EOF returns 0",
        "dup with narrowed rights",
        "write through read-only fd refused",
        "close returns 0",
        "stale fd after close refused",
        "double close refused",
        "open missing file returns ENOENT",
        "open directory returns EISDIR",
        "kernel pointer to read rejected",
        "write to stdout via fd 1",
        "pread 14 bytes at offset 0",
        "pread content matches",
        "fstat reports size 3000",
        "stat by path reports size 3000",
        "isatty on a file fd is 0",
        "isatty on fd 1 is 1",
        "getpid returns nonzero",
        "sleep returns 0",
        "debug_print refused without the gate"
    };

    int bad = 0;
    for (int i = 0; i < FD_CHECKS; i++) {
        if (res[i] != 1) { bad++; kprintf(PRINT_SERIAL, "    FAIL %s\n", what[i]); }
    }

    kprintf(PRINT_SERIAL, "FD ring3: %d of %d checks passed: ", FD_CHECKS - bad, FD_CHECKS);
    kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    pmm_free(results_phys, 1);
}

static int64_t call_sys(int nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    struct isr_frame f;
    for (size_t i = 0; i < sizeof(f) / 8; i++) ((uint64_t *)&f)[i] = 0;
    f.cs = GDT_KERNEL_DATA;
    f.rax = (uint64_t)nr;
    f.rdi = a; f.rsi = b; f.rdx = c; f.r10 = d;
    syscall_dispatch(&f);
    return (int64_t)f.rax;
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
    pit_init(1000);
    lapic_unmask_ext_int(lapic_virt);
    idt_init();
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
        extern uint8_t user_zfd_entry[];
        extern uint8_t user_zfd_end[];

        size_t code_bytes = (size_t)(user_zfd_end - user_test_entry);
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
            vaddr_t fd_rip = TASK_USER_CODE_VIRT + (size_t)(user_zfd_entry - user_test_entry);
            fd_spawn(fd_rip, code_bytes, code_pages);
        }

        {
            int bad = 0;
            int tty = (int)call_sys(SYS_ISATTY, 1, 0, 0, 0);
            int file = fd_open(task_current(), "/TEST.TXT", FD_OPEN_READ);
            int nottty = file >= 0 ? (int)call_sys(SYS_ISATTY, (uint64_t)file, 0, 0, 0) : -1;

            if (tty != 1) { kprintf(PRINT_SERIAL, "    FAIL isatty(1)=%d\n", tty); bad++; }
            if (file < 0) { kprintf(PRINT_SERIAL, "    FAIL open for isatty\n"); bad++; }
            else if (nottty != 0) { kprintf(PRINT_SERIAL, "    FAIL isatty(file)=%d\n", nottty); bad++; }

            struct sys_stat st;
            if (file >= 0) {
                int64_t r = call_sys(SYS_FSTAT, (uint64_t)file, (uint64_t)(uintptr_t)&st, 0, 0);
                if (r != 0 || st.size != 3000) {
                    kprintf(PRINT_SERIAL, "    FAIL fstat r=%lld size=%llu\n", (long long)r, (unsigned long long)st.size);
                    bad++;
                }
            }

            struct sys_stat sp;
            int64_t r = call_sys(SYS_STAT, (uint64_t)(uintptr_t)"/TEST.TXT", (uint64_t)(uintptr_t)&sp, 0, 0);
            if (r != 0 || sp.size != 3000) {
                kprintf(PRINT_SERIAL, "    FAIL stat r=%lld size=%llu\n", (long long)r, (unsigned long long)sp.size);
                bad++;
            }

            int dir = fd_open(task_current(), "/SUB", FD_OPEN_READ);
            if (dir >= 0) {
                struct sys_dirent de;
                int64_t g = call_sys(SYS_GETDENTS, (uint64_t)dir, 0, (uint64_t)(uintptr_t)&de, 0);
                if (g != 1) { kprintf(PRINT_SERIAL, "    FAIL getdents first=%lld\n", (long long)g); bad++; }
                else {
                    int64_t g2 = call_sys(SYS_GETDENTS, (uint64_t)dir, 99, (uint64_t)(uintptr_t)&de, 0);
                    if (g2 != 0) { kprintf(PRINT_SERIAL, "    FAIL getdents end=%lld\n", (long long)g2); bad++; }
                }
                fd_close(task_current(), dir);
            }

            if (file >= 0) {
                char buf[16] = {0};
                int64_t pr = call_sys(SYS_PREAD, (uint64_t)file, (uint64_t)(uintptr_t)buf, 14, 0);
                int ok = (pr == 14);
                for (int i = 0; i < 14 && ok; i++) if (buf[i] != "that my jarona"[i]) ok = 0;
                if (!ok) { kprintf(PRINT_SERIAL, "    FAIL pread r=%lld\n", (long long)pr); bad++; }
                fd_close(task_current(), file);
            }

            int64_t pid = call_sys(SYS_GETPID, 0, 0, 0, 0);
            if (pid != 0) { kprintf(PRINT_SERIAL, "    FAIL getpid=%lld\n", (long long)pid); bad++; }

            int64_t sl = call_sys(SYS_SLEEP, 2, 0, 0, 0);
            if (sl != 0) { kprintf(PRINT_SERIAL, "    FAIL sleep=%lld\n", (long long)sl); bad++; }

            int64_t dp = call_sys(SYS_DEBUG_PRINT, (uint64_t)(uintptr_t)"debug_print ok\n", 15, 0, 0);
            if (dp != 15) { kprintf(PRINT_SERIAL, "    FAIL debug_print=%lld\n", (long long)dp); bad++; }

            if (call_sys(0, 0, 0, 0, 0) != SYS_ENOSYS) { kprintf(PRINT_SERIAL, "    FAIL nr 0 not ENOSYS\n"); bad++; }
            if (call_sys(77, 0, 0, 0, 0) != SYS_ENOSYS) { kprintf(PRINT_SERIAL, "    FAIL nr 77 not ENOSYS\n"); bad++; }
            if (call_sys(250, 0, 0, 0, 0) != SYS_ENOSYS) { kprintf(PRINT_SERIAL, "    FAIL nr 250 not ENOSYS\n"); bad++; }

            kprintf(PRINT_SERIAL, "FD kernel: 13 new syscall checks (pread/stat/fstat/getdents/isatty/getpid/sleep/debug/reserved): ");
            kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
            kprintf(PRINT_SERIAL, "  %d bad\n", bad);
        }

        {
            struct task *ft = task_current();
            int base = 0;
            int many[40];
            int opened = 0;
            for (int i = 0; i < 40; i++) {
                many[i] = fd_open(ft, "/TEST.TXT", FD_OPEN_READ);
                if (many[i] < 0) break;
                opened++;
            }
            for (int i = 0; i < opened; i++) fd_close(ft, many[i]);
            kprintf(PRINT_SERIAL, "FD kernel: %d concurrent fds past inline %d: ", opened, FD_INLINE);
            kprintchar(opened > FD_INLINE ? 'Y' : 'N', PRINT_SERIAL);
            kprintchar('\n', PRINT_SERIAL);

            struct kobject *o0 = fd_get_checked(ft, 0, KOBJ_TYPE_ANY, FD_RIGHT_READ);
            struct kobject *o1 = fd_get_checked(ft, 1, KOBJ_TYPE_ANY, FD_RIGHT_WRITE);
            struct kobject *o2 = fd_get_checked(ft, 2, KOBJ_TYPE_ANY, FD_RIGHT_WRITE);
            if (o0) fd_put(o0);
            if (o1) fd_put(o1);
            if (o2) fd_put(o2);
            kprintf(PRINT_SERIAL, "FD kernel: stdio survives table growth: ");
            kprintchar((o0 && o1 && o2) ? 'Y' : 'N', PRINT_SERIAL);
            kprintchar('\n', PRINT_SERIAL);
            (void)base;
        }

        {
            struct task tmp;
            size_t before = pmm_free_pages();

            int cycles = 3000;
            fd_table_init(&tmp);
            for (int i = 0; i < cycles; i++) {
                int a = fd_open(&tmp, "/TEST.TXT", FD_OPEN_READ);
                if (a >= 0) fd_close(&tmp, a);
                int b = fd_open(&tmp, "/TEST.TXT", FD_OPEN_READ);
                if (b >= 0) fd_table_clear(&tmp);
                fd_table_init(&tmp);
            }

            size_t after = pmm_free_pages();
            kprintf(PRINT_SERIAL, "FD kernel: teardown leak check, %d cycles: ", cycles);
            kprintchar(after >= before ? 'Y' : 'N', PRINT_SERIAL);
            kprintf(PRINT_SERIAL, " (%zu -> %zu)\n", before, after);
        }
    }

    for (;;) {
        uint64_t seq = task_wake_seq();
        keyboard_process_buffer();
        task_block_current(seq);
    }
}
