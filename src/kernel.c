#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine/limine.h"
#include "font/font.h"
#include "video/video.h"
#include "io/serial/serial.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/pic/pic.h"
#include "arch/x86_64/pit/pit.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
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

    pic_remap(0x20, 0x28);
    pit_init(1000);

    idt_init();

    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint32_t width = fb->width;

    print_str(fb_ptr, "nyonn nyon nyonn ulelelel nyon leleel nyonn", 0, 0, 0xffffff, width);
    print_str(fb_ptr, "-kawkaw from deltarune", 0, 16, 0xffffff, width);

    serial_print("nyonOS: Drawing complete!\n");

    serial_print("PIT test: sleeping 1000ms...\n");
    pit_sleep(1000);
    serial_print("PIT test: woke up after 1000ms\n");

    for (;;) __asm__ volatile("hlt");
}
