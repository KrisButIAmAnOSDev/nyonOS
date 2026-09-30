#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot/limine/limine.h"
#include "graphics/font/font.h"
#include "graphics/video/video.h"
#include "io/serial/serial.h"
#include "io/kprintf/kprintf.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/pic/pic.h"
#include "io/keyboard/keyboard.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "arch/x86_64/pit/pit.h"
#include "kernel/multitask/task.h"
#include "kernel/mm/vmm/vmm.h"
#include "arch/x86_64/lapic/lapic.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/gdt/gdt.h"

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

static void test_map_unmap_churn(void) {
    size_t baseline = pmm_free_pages();

    paddr_t phys;
    int i;
    int mapped = 0;

    for (i = 0; i < 64; i++) {
        vaddr_t virt = 0xFFFFFFFFB0000000ULL + (vaddr_t)i * 0x200000;
        if (!pmm_alloc(&phys, 1)) break;
        if (!vmm_map(virt, phys, 1, VMM_DEFAULT_FLAGS)) { pmm_free(phys, 1); break; }
        if (!vmm_unmap(virt, 1)) break;
        pmm_free(phys, 1);
        mapped++;
    }

    size_t after = pmm_free_pages();
    long leaked = (long)baseline - (long)after;

    kprintf(PRINT_SERIAL, "VMM test: map/unmap churn x");
    kprintf(PRINT_SERIAL, "%llu", (unsigned long long)((uint64_t)mapped));
    kprintf(PRINT_SERIAL, "\n  free_pages baseline=");
    kprintf(PRINT_SERIAL, "%llu", (unsigned long long)((uint64_t)baseline));
    kprintf(PRINT_SERIAL, " after=");
    kprintf(PRINT_SERIAL, "%llu", (unsigned long long)((uint64_t)after));
    kprintf(PRINT_SERIAL, "  leaked=");
    kprintf(PRINT_SERIAL, "%llu", (unsigned long long)((uint64_t)(leaked > 0 ? leaked : 0)));
    kprintf(PRINT_SERIAL, "  reclaimed: ");
    kprintchar(leaked == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
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

    kprintf(PRINT_SERIAL, "PMM test: alloc 3 pages...\n");
    paddr_t p;
    if (pmm_alloc(&p, 3)) {
                kprintf(PRINT_SERIAL, "  got 0x%llx\n", (unsigned long long)p);
        kprintf(PRINT_SERIAL, "\n");
        pmm_free(p, 3);
        kprintf(PRINT_SERIAL, "  freed\n");
    } else {
        kprintf(PRINT_SERIAL, "  FAILED\n");
    }

    kprintf(PRINT_SERIAL, "PMM test: OOM reporting\n");
    paddr_t oom;
    bool oom_rejected = !pmm_alloc(&oom, pmm_total_pages() + 1);
    paddr_t zero;
    bool zero_rejected = !pmm_alloc(&zero, 0);
    kprintf(PRINT_SERIAL, "  oversized request rejected: ");
    kprintchar(oom_rejected ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  zero-page request rejected: ");
    kprintchar(zero_rejected ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "VMM test: map 2 pages...\n");
    paddr_t phys;
    bool got_phys = pmm_alloc(&phys, 2);
    vaddr_t virt = 0xFFFFFFFFC0000000;
    if (got_phys && vmm_map(virt, phys, 2, VMM_DEFAULT_FLAGS)) {
        kprintf(PRINT_SERIAL, "  mapped 0x%llx -> 0x%llx\n", (unsigned long long)virt, (unsigned long long)phys);

        paddr_t phys2 = vmm_virt_to_phys(virt);
        kprintf(PRINT_SERIAL, "  virt_to_phys: 0x%llx\n", (unsigned long long)phys2);

        vmm_unmap(virt, 2);
        kprintf(PRINT_SERIAL, "  unmapped\n");
        pmm_free(phys, 2);
    } else {
        kprintf(PRINT_SERIAL, "  FAILED\n");
    }

    kprintf(PRINT_SERIAL, "PIT test: pit_sleep\n");
    {
        uint64_t t0 = pit_get_ticks();
        pit_sleep(100);
        uint64_t t1 = pit_get_ticks();
        uint64_t d100 = t1 - t0;

        t0 = pit_get_ticks();
        pit_sleep(500);
        t1 = pit_get_ticks();
        uint64_t d500 = t1 - t0;

        kprintf(PRINT_SERIAL, "  100ms slept, ticks advanced ");
        kprintf(PRINT_SERIAL, "%llu", (unsigned long long)(d100));
        kprintf(PRINT_SERIAL, "  (expect ~100)\n  500ms slept, ticks advanced ");
        kprintf(PRINT_SERIAL, "%llu", (unsigned long long)(d500));
        kprintf(PRINT_SERIAL, "  (expect ~500)\n  ticker alive: ");
        kprintchar(d100 > 0 ? 'Y' : 'N', PRINT_SERIAL);
        kprintf(PRINT_SERIAL, "  in range: ");
        kprintchar((d100 >= 95 && d100 <= 110 && d500 >= 490 && d500 <= 520) ? 'Y' : 'N', PRINT_SERIAL);
        kprintchar('\n', PRINT_SERIAL);
    }

    test_map_unmap_churn();

    kprintf(PRINT_SERIAL, "KPRINTF test: %d %u %x %X %s %c %% %ld %lu %zu\n", -42, 42u, 0xdeadbeefu, 0xcafeu, "str", 'Z', -123456789L, 123456789UL, (size_t)4096);
    kprintf(PRINT_SERIAL, "KPRINTF test: ptr=%p null=%s\n", (void *)0xffffffff80000000ULL, (char *)0);

    heap_init();

    kprintf(PRINT_SERIAL, "HEAP test: basic\n");
    char *ha = kmalloc(64);
    char *hb = kmalloc(64);
    for (int i = 0; i < 64; i++) ha[i] = 0xAA;
    for (int i = 0; i < 64; i++) hb[i] = 0xBB;
    kprintf(PRINT_SERIAL, "  a!=b: ");
    kprintchar(ha != hb ? 'Y' : 'N', PRINT_SERIAL);
    int intact = 1;
    for (int i = 0; i < 64; i++) if (ha[i] != (char)0xAA) intact = 0;
    kprintf(PRINT_SERIAL, "  a intact: ");
    kprintchar(intact ? 'Y' : 'N', PRINT_SERIAL);
    kfree(hb);
    kfree(ha);
    char *hc = kmalloc(64);
    kprintf(PRINT_SERIAL, "  reuse head: ");
    kprintchar(hc == ha ? 'Y' : 'N', PRINT_SERIAL);
    kfree(hc);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "HEAP test: ALIGN_UP overflow\n");
    char *hh = kmalloc(SIZE_MAX);
    kprintf(PRINT_SERIAL, "  kmalloc(SIZE_MAX) = ");
    kprintf(PRINT_SERIAL, "%s", hh ? "NON-NULL  <-- bug" : "NULL  ok");
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "HEAP test: double free\n");
    char *q1 = kmalloc(64);
    char *q2 = kmalloc(64);
    char *q3 = kmalloc(64);
    char *q4 = kmalloc(64);
    kfree(q1);
    kfree(q3);
    kfree(q2);
    kfree(q2);
    kfree(q1);
    kfree(q3);
    kfree(q4);
    char *probe = kmalloc(64);
    kprintf(PRINT_SERIAL, "  heap still healthy after rejects: ");
    kprintchar(probe ? 'Y' : 'N', PRINT_SERIAL);
    kfree(probe);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "HEAP test: kfree of a wild pointer\n");
    kfree((void *)0x1234);
    kprintf(PRINT_SERIAL, "  rejected, no crash: Y\n");

    kprintf(PRINT_SERIAL, "HEAP test: kfree of a misaligned pointer\n");
    char *ma = kmalloc(64);
    kfree(ma + 8);
    kfree(ma);
    kprintf(PRINT_SERIAL, "  rejected, no crash: Y\n");

    kprintf(PRINT_SERIAL, "HEAP test: churn (alloc/free 200x)\n");
    int churn_ok = 1;
    for (int i = 0; i < 200; i++) {
        char *p = kmalloc(96);
        if (!p) { churn_ok = 0; break; }
        for (int k = 0; k < 96; k++) p[k] = (char)i;
        for (int k = 0; k < 96; k++) if (p[k] != (char)i) churn_ok = 0;
        kfree(p);
    }
    kprintf(PRINT_SERIAL, "  survived: ");
    kprintchar(churn_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "HEAP test: trim returns pages\n");
    kprintf(PRINT_SERIAL, "  committed now = ");
    {
        uint64_t t = heap_committed_bytes();
        char b[24];
        size_t i = 0;
        if (t == 0) b[i++] = '0';
        else { char r[24]; size_t j = 0; while (t) { r[j++] = '0' + (t % 10); t /= 10; } while (j) b[i++] = r[--j]; }
        b[i] = 0;
        kprintf(PRINT_SERIAL, "%s", b);
        kprintf(PRINT_SERIAL, " bytes (0 = fully returned)\n");
    }

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
        extern uint8_t user_test_entry[];
        extern uint8_t user_test_end[];

        paddr_t uphys;
        vaddr_t uvirt = 0x0000000000400000ULL;
        size_t code_bytes = (size_t)(user_test_end - user_test_entry);
        size_t code_pages = (code_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        if (pmm_alloc(&uphys, code_pages) && vmm_map(uvirt, uphys, code_pages, VMM_USER_FLAGS)) {
            volatile uint8_t *dst = (volatile uint8_t *)(uvirt);
            for (size_t i = 0; i < code_bytes; i++) dst[i] = user_test_entry[i];
            task_spawn_ring3("ring3", uvirt);
        } else {
            kprintf(PRINT_SERIAL, "RING3 test: FAILED to map user code\n");
        }
    }

    for (;;) {
        keyboard_process_buffer();
        __asm__ volatile("hlt");
    }
}
