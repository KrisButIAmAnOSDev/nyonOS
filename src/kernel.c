#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine/limine.h"
#include "font/font.h"
#include "video/video.h"
#include "io/serial/serial.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/gdt/gdt.h"
#include "arch/x86_64/pic/pic.h"
#include "io/keyboard/keyboard.h"
#include "mm/pmm/pmm.h"
#include "mm/heap/heap.h"
#include "mm/vmm/vmm.h"
#include "arch/x86_64/lapic/lapic.h"

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

void kmain(void) {
    serial_init();
    serial_print("nyonOS: Kernel loaded!\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        serial_print("nyonOS: Limine revision not supported!\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_print("nyonOS: Limine revision OK!\n");

    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        serial_print("nyonOS: No framebuffer!\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_print("nyonOS: Framebuffer found!\n");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) {
        serial_print("nyonOS: Wrong framebuffer format!\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_print("nyonOS: Framebuffer format OK!\n");

    if (memmap_request.response == NULL) {
        serial_print("nyonOS: No memmap response!\n");
        for (;;) __asm__ volatile("hlt");
    }
    
    if (hhdm_request.response == NULL) {
        serial_print("nyonOS: No HHDM response!\n");
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
       serial_print("nyonOS: Failed to map LAPIC!\n");
       for (;;) __asm__ volatile("hlt");
    }

    pic_remap(0x20, 0x28);
    lapic_unmask_ext_int(lapic_virt);
    idt_init();
    keyboard_init();

    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint32_t width = fb->width;

    print_str(fb_ptr, "nyonn nyon nyonn ulelelel nyon leleel nyonn", 0, 0, 0xff00ff, width);
    print_str(fb_ptr, "-kawkaw from deltarune", 50, 16, 0xff00ff, width);

    serial_print("nyonOS: Drawing complete!\n");

    serial_print("PMM test: alloc 3 pages...\n");
    paddr_t p = pmm_alloc(3);
    if (p) {
        serial_print("  got 0x");
        char buf[16];
        size_t idx = 0, t = p;
        if (t == 0) buf[idx++] = '0';
        else { char rev[16]; size_t j = 0; while (t) { rev[j++] = "0123456789abcdef"[t % 16]; t /= 16; } while (j--) buf[idx++] = rev[j]; }
        buf[idx] = 0;
        serial_print(buf);
        serial_print("\n");
        pmm_free(p, 3);
        serial_print("  freed\n");
    } else {
        serial_print("  FAILED\n");
    }

    serial_print("VMM test: map 2 pages...\n");
    paddr_t phys = pmm_alloc(2);
    vaddr_t virt = 0xFFFFFFFFC0000000;
    if (phys && vmm_map(virt, phys, 2, VMM_DEFAULT_FLAGS)) {
        serial_print("  mapped 0x");
        char buf[16];
        size_t idx = 0, t = virt;
        if (t == 0) buf[idx++] = '0';
        else { char rev[16]; size_t j = 0; while (t) { rev[j++] = "0123456789abcdef"[t % 16]; t /= 16; } while (j--) buf[idx++] = rev[j]; }
        buf[idx] = 0;
        serial_print(buf);
        serial_print(" -> 0x");
        t = phys; idx = 0;
        if (t == 0) buf[idx++] = '0';
        else { char rev[16]; size_t j = 0; while (t) { rev[j++] = "0123456789abcdef"[t % 16]; t /= 16; } while (j--) buf[idx++] = rev[j]; }
        buf[idx] = 0;
        serial_print(buf);
        serial_print("\n");
        
        paddr_t phys2 = vmm_virt_to_phys(virt);
        serial_print("  virt_to_phys: 0x");
        t = phys2; idx = 0;
        if (t == 0) buf[idx++] = '0';
        else { char rev[16]; size_t j = 0; while (t) { rev[j++] = "0123456789abcdef"[t % 16]; t /= 16; } while (j--) buf[idx++] = rev[j]; }
        buf[idx] = 0;
        serial_print(buf);
        serial_print("\n");
        
        vmm_unmap(virt, 2);
        serial_print("  unmapped\n");
        pmm_free(phys, 2);
    } else {
        serial_print("  FAILED\n");
    }

    heap_init();

    serial_print("HEAP test: basic\n");
    char *ha = kmalloc(64);
    char *hb = kmalloc(64);
    for (int i = 0; i < 64; i++) ha[i] = 0xAA;
    for (int i = 0; i < 64; i++) hb[i] = 0xBB;
    serial_print("  a!=b: ");
    serial_putchar(ha != hb ? 'Y' : 'N');
    int intact = 1;
    for (int i = 0; i < 64; i++) if (ha[i] != (char)0xAA) intact = 0;
    serial_print("  a intact: ");
    serial_putchar(intact ? 'Y' : 'N');
    kfree(hb);
    kfree(ha);
    char *hc = kmalloc(64);
    serial_print("  reuse head: ");
    serial_putchar(hc == ha ? 'Y' : 'N');
    kfree(hc);
    serial_putchar('\n');

    serial_print("HEAP test: ALIGN_UP overflow\n");
    char *hh = kmalloc(SIZE_MAX);
    serial_print("  kmalloc(SIZE_MAX) = ");
    serial_print(hh ? "NON-NULL  <-- bug" : "NULL  ok");
    serial_putchar('\n');

    serial_print("HEAP test: double free\n");
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
    serial_print("  heap still healthy after rejects: ");
    serial_putchar(probe ? 'Y' : 'N');
    kfree(probe);
    serial_putchar('\n');

    serial_print("HEAP test: kfree of a wild pointer\n");
    kfree((void *)0x1234);
    serial_print("  rejected, no crash: Y\n");

    serial_print("HEAP test: kfree of a misaligned pointer\n");
    char *ma = kmalloc(64);
    kfree(ma + 8);
    kfree(ma);
    serial_print("  rejected, no crash: Y\n");

    serial_print("HEAP test: churn (alloc/free 200x)\n");
    int churn_ok = 1;
    for (int i = 0; i < 200; i++) {
        char *p = kmalloc(96);
        if (!p) { churn_ok = 0; break; }
        for (int k = 0; k < 96; k++) p[k] = (char)i;
        for (int k = 0; k < 96; k++) if (p[k] != (char)i) churn_ok = 0;
        kfree(p);
    }
    serial_print("  survived: ");
    serial_putchar(churn_ok ? 'Y' : 'N');
    serial_putchar('\n');

    serial_print("HEAP test: trim returns pages\n");
    serial_print("  committed now = ");
    {
        uint64_t t = heap_committed_bytes();
        char b[24];
        size_t i = 0;
        if (t == 0) b[i++] = '0';
        else { char r[24]; size_t j = 0; while (t) { r[j++] = '0' + (t % 10); t /= 10; } while (j) b[i++] = r[--j]; }
        b[i] = 0;
        serial_print(b);
        serial_print(" bytes (0 = fully returned)\n");
    }

    for (;;) {
        keyboard_process_buffer();
        __asm__ volatile("hlt");
    }
}
