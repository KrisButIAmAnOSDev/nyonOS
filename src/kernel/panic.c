#include "kernel/panic.h"
#include "kernel/multitask/task.h"
#include "kernel/mm/vmm/vmm.h"
#include "io/serial/serial.h"
#include "video/video.h"
#include "limine/limine.h"
#include <stddef.h>

extern __attribute__((used, section(".limine_requests")))
struct limine_framebuffer_request framebuffer_request;

static const char* exception_names[32] = {
    "Divide Error",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Floating-Point Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

static void print_hex64_fb(volatile uint32_t* fb, uint32_t width, uint64_t val, uint32_t x, uint32_t y, uint32_t color) {
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        draw_char(fb, c, x, y, color, width);
        x += 8;
    }
}

static void print_hex64_serial(uint64_t val) {
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        serial_putchar(c);
    }
}



static void print_str_fb(volatile uint32_t* fb, uint32_t width, const char* str, uint32_t x, uint32_t y, uint32_t color) {
    while (*str) {
        draw_char(fb, *str, x, y, color, width);
        x += 8;
        str++;
    }
}

static void dump_pf(struct panic_context* ctx, uint64_t err) {
    serial_print("--- PAGE FAULT DETAIL ---\n");
    serial_print("  cr2          = 0x");
    print_hex64_serial(ctx->cr2);
    serial_print("\n  err bits     = P:");
    serial_putchar((err & (1ULL << 0)) ? '1' : '0');
    serial_print(" W:");
    serial_putchar((err & (1ULL << 1)) ? '1' : '0');
    serial_print(" U:");
    serial_putchar((err & (1ULL << 2)) ? '1' : '0');
    serial_print(" RSVD:");
    serial_putchar((err & (1ULL << 3)) ? '1' : '0');
    serial_print(" FETCH:");
    serial_putchar((err & (1ULL << 4)) ? '1' : '0');
    serial_print(" NX:");
    serial_putchar((err & (1ULL << 34)) ? '1' : '0');
    serial_print("\n  cr0.WP      = ");
    serial_putchar((ctx->cr0 & (1ULL << 16)) ? '1' : '0');
    serial_print("\n  cr4         = 0x");
    print_hex64_serial(ctx->cr4);
    uint64_t pte = vmm_query(ctx->cr2);
    serial_print("\n  leaf PTE    = 0x");
    print_hex64_serial(pte);
    if (pte == 0) {
        serial_print("   (no mapping found)");
    } else {
        serial_print("   P:");
        serial_putchar((pte & PAGE_PRESENT) ? '1' : '0');
        serial_print(" W:");
        serial_putchar((pte & PAGE_WRITE) ? '1' : '0');
        serial_print(" U:");
        serial_putchar((pte & PAGE_USER) ? '1' : '0');
        serial_print(" NX:");
        serial_putchar((pte & PAGE_NX) ? '1' : '0');
    }
    serial_print("\n  cr3         = 0x");
    print_hex64_serial(ctx->cr3);
    serial_putchar('\n');
}

static void dump_stack(uint64_t rsp) {
    const volatile uint64_t *sp = (const volatile uint64_t *)rsp;
    serial_print("--- STACK (24 qwords from rsp) ---\n");
    for (int i = 0; i < 24; i++) {
        uint64_t v = sp[i];
        serial_print("  [");
        print_hex64_serial((uint64_t)i * 8);
        serial_print("] 0x");
        print_hex64_serial(v);
        if (v >= 0xffffffff80000000ULL && v < 0xffffffff80100000ULL) serial_print("  <- kernel image");
        serial_putchar('\n');
    }
}

static void dump_task(void) {
    struct task *t = task_current();
    serial_print("--- CURRENT TASK ---\n  name=");
    serial_print(t->name ? t->name : "?");
    serial_print("  in_use=");
    serial_putchar(t->in_use ? 'Y' : 'N');
    serial_print("  switches=");
    print_hex64_serial(task_switch_count());
    serial_print("  frame=0x");
    print_hex64_serial((uint64_t)t->frame);
    if (t->frame) {
        const uint64_t *q = (const uint64_t *)t->frame;
        serial_print("\n  frame rip=0x"); print_hex64_serial(q[17]);
        serial_print(" cs=0x"); print_hex64_serial(q[18]);
        serial_print(" rflags=0x"); print_hex64_serial(q[19]);
        serial_print(" rsp=0x"); print_hex64_serial(q[20]);
        serial_print(" ss=0x"); print_hex64_serial(q[21]);
    }
    serial_print("\n  stack_base=0x"); print_hex64_serial(t->stack_base);
    serial_print("  stack_top=0x"); print_hex64_serial(t->stack_top);
    serial_putchar('\n');
}

void panic_dump_regs(struct panic_context* ctx) {
    struct isr_frame* f = &ctx->frame;
    struct limine_framebuffer* fb_ptr = framebuffer_request.response ? framebuffer_request.response->framebuffers[0] : NULL;
    volatile uint32_t* fb = fb_ptr ? (volatile uint32_t*)fb_ptr->address : NULL;
    uint32_t width = fb_ptr ? fb_ptr->width : 0;
    uint32_t height = fb_ptr ? fb_ptr->height : 0;

    serial_print("\n========== KERNEL PANIC ==========\n");

    if (fb) {
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                fb[y * width + x] = 0x000000;
            }
        }
        print_str_fb(fb, width, "KERNEL PANIC", 8, 8, 0xFF0000);
    }

    const char* exc_name;
    if (f->interrupt_number == 255) {
        exc_name = "Unexpected interrupt (unhandled vector 48-255)";
    } else if (f->interrupt_number < 32) {
        exc_name = exception_names[f->interrupt_number];
    } else {
        exc_name = "Unexpected interrupt";
    }
    char msg[128];
    serial_print("Exception: ");
    serial_print(exc_name);
    serial_print(" (vector ");
    char num[4];
    num[0] = '0' + (f->interrupt_number / 100);
    num[1] = '0' + ((f->interrupt_number / 10) % 10);
    num[2] = '0' + (f->interrupt_number % 10);
    num[3] = 0;
    serial_print(num);
    serial_print(")\n");

    if (fb) {
        print_str_fb(fb, width, "Exception: ", 8, 32, 0xFFFFFF);
        print_str_fb(fb, width, exc_name, 8 + 11 * 8, 32, 0xFFFFFF);
        print_str_fb(fb, width, " (vector ", 8 + 11 * 8 + 8 * 16, 32, 0xFFFFFF);
        char num[4];
        num[0] = '0' + (f->interrupt_number / 100);
        num[1] = '0' + ((f->interrupt_number / 10) % 10);
        num[2] = '0' + (f->interrupt_number % 10);
        num[3] = 0;
        print_str_fb(fb, width, num, 8 + 11 * 8 + 8 * 16 + 8 * 10, 32, 0xFFFFFF);
        print_str_fb(fb, width, ")", 8 + 11 * 8 + 8 * 16 + 8 * 10 + 8 * 3, 32, 0xFFFFFF);
    }

    __asm__ volatile("mov %%cr0, %0" : "=r"(ctx->cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(ctx->cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(ctx->cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(ctx->cr4));
    __asm__ volatile("mov %%rsp, %0" : "=r"(ctx->rsp_at_panic));

    serial_print("RIP: 0x"); print_hex64_serial(f->rip); serial_print("\n");
    serial_print("CS:  0x"); print_hex64_serial(f->cs); serial_print("\n");
    serial_print("RFLAGS: 0x"); print_hex64_serial(f->rflags); serial_print("\n");
    serial_print("RSP: 0x"); print_hex64_serial(ctx->rsp_at_panic); serial_print("\n");
    serial_print("RAX: 0x"); print_hex64_serial(f->rax); serial_print("\n");
    serial_print("RBX: 0x"); print_hex64_serial(f->rbx); serial_print("\n");
    serial_print("RCX: 0x"); print_hex64_serial(f->rcx); serial_print("\n");
    serial_print("RDX: 0x"); print_hex64_serial(f->rdx); serial_print("\n");
    serial_print("RSI: 0x"); print_hex64_serial(f->rsi); serial_print("\n");
    serial_print("RDI: 0x"); print_hex64_serial(f->rdi); serial_print("\n");
    serial_print("RBP: 0x"); print_hex64_serial(f->rbp); serial_print("\n");
    serial_print("R8:  0x"); print_hex64_serial(f->r8); serial_print("\n");
    serial_print("R9:  0x"); print_hex64_serial(f->r9); serial_print("\n");
    serial_print("R10: 0x"); print_hex64_serial(f->r10); serial_print("\n");
    serial_print("R11: 0x"); print_hex64_serial(f->r11); serial_print("\n");
    serial_print("R12: 0x"); print_hex64_serial(f->r12); serial_print("\n");
    serial_print("R13: 0x"); print_hex64_serial(f->r13); serial_print("\n");
    serial_print("R14: 0x"); print_hex64_serial(f->r14); serial_print("\n");
    serial_print("R15: 0x"); print_hex64_serial(f->r15); serial_print("\n");
    serial_print("Error Code: 0x"); print_hex64_serial(f->error_code); serial_print("\n");
    serial_print("CR0: 0x"); print_hex64_serial(ctx->cr0); serial_print("\n");
    serial_print("CR2: 0x"); print_hex64_serial(ctx->cr2); serial_print("\n");
    serial_print("CR3: 0x"); print_hex64_serial(ctx->cr3); serial_print("\n");
    serial_print("CR4: 0x"); print_hex64_serial(ctx->cr4); serial_print("\n");

    if (fb) {
        uint32_t y = 56;
        print_str_fb(fb, width, "RIP: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rip, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "CS:  0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->cs, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RFLAGS: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rflags, 88, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RSP: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, ctx->rsp_at_panic, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RAX: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rax, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RBX: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rbx, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RCX: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rcx, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RDX: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rdx, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RSI: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rsi, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RDI: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rdi, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "RBP: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->rbp, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R8:  0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r8, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R9:  0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r9, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R10: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r10, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R11: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r11, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R12: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r12, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R13: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r13, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R14: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r14, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "R15: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->r15, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "Err:  0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, f->error_code, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "CR0: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, ctx->cr0, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "CR2: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, ctx->cr2, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "CR3: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, ctx->cr3, 64, y, 0xFFFFFF);
        y += 16; print_str_fb(fb, width, "CR4: 0x", 8, y, 0xFFFFFF); print_hex64_fb(fb, width, ctx->cr4, 64, y, 0xFFFFFF);
    }

    serial_print("========== HALTING ==========\n");
    dump_pf(ctx, f->error_code);
    dump_stack(ctx->rsp_at_panic);
    dump_task();
    for (;;) __asm__ volatile("hlt");
}

void panic(const char* msg __attribute__((unused)), struct isr_frame* frame) {
    struct panic_context ctx;
    ctx.frame = *frame;
    panic_dump_regs(&ctx);
}