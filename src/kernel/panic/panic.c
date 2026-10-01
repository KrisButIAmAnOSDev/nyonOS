#include "kernel/panic/panic.h"
#include "kernel/multitask/task.h"
#include "kernel/mm/vmm/vmm.h"
#include "io/serial/serial.h"
#include "io/kprintf/kprintf.h"
#include "graphics/video/video.h"
#include "boot/limine/limine.h"
#include <stddef.h>

extern __attribute__((section(".limine_requests")))
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

static void dump_syscall_state(struct panic_context* ctx) {
    struct isr_frame* f = &ctx->frame;

    kprintf(PRINT_BOTH, "--- SYSCALL STATE ---\n");
    kprintf(PRINT_BOTH, "  fault vector   = %llu\n", (unsigned long long)f->interrupt_number);
    kprintf(PRINT_BOTH, "  fault is 0x80  = %s\n", f->interrupt_number == 0x80 ? "Y" : "N");
    kprintf(PRINT_BOTH, "  frame cs       = 0x%llx rpl=%llu\n", (unsigned long long)f->cs, (unsigned long long)(f->cs & 3));
    kprintf(PRINT_BOTH, "  frame ss       = 0x%llx\n", (unsigned long long)f->ss);
    kprintf(PRINT_BOTH, "  frame rsp      = 0x%llx\n", (unsigned long long)f->rsp);
}

static void dump_pf(struct panic_context* ctx, uint64_t err) {
    kprintf(PRINT_BOTH, "--- PAGE FAULT DETAIL ---\n");
    kprintf(PRINT_BOTH, "  cr2          = 0x");
    kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr2));
    kprintf(PRINT_BOTH, "\n  err bits     = P:");
    kprintchar((err & (1ULL << 0)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, " W:");
    kprintchar((err & (1ULL << 1)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, " U:");
    kprintchar((err & (1ULL << 2)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, " RSVD:");
    kprintchar((err & (1ULL << 3)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, " FETCH:");
    kprintchar((err & (1ULL << 4)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, " NX:");
    kprintchar((err & (1ULL << 34)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, "\n  cr0.WP      = ");
    kprintchar((ctx->cr0 & (1ULL << 16)) ? '1' : '0', PRINT_BOTH);
    kprintf(PRINT_BOTH, "\n  cr4         = 0x");
    kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr4));
    uint64_t pte = vmm_query(ctx->cr2);
    kprintf(PRINT_BOTH, "\n  leaf PTE    = 0x");
    kprintf(PRINT_BOTH, "%llx", (unsigned long long)(pte));
    if (pte == 0) {
        kprintf(PRINT_BOTH, "   (no mapping found)");
    } else {
        kprintf(PRINT_BOTH, "   P:");
        kprintchar((pte & PAGE_PRESENT) ? '1' : '0', PRINT_BOTH);
        kprintf(PRINT_BOTH, " W:");
        kprintchar((pte & PAGE_WRITE) ? '1' : '0', PRINT_BOTH);
        kprintf(PRINT_BOTH, " U:");
        kprintchar((pte & PAGE_USER) ? '1' : '0', PRINT_BOTH);
        kprintf(PRINT_BOTH, " NX:");
        kprintchar((pte & PAGE_NX) ? '1' : '0', PRINT_BOTH);
    }
    kprintf(PRINT_BOTH, "\n  cr3         = 0x");
    kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr3));
    kprintchar('\n', PRINT_BOTH);
}

static void dump_stack(uint64_t rsp) {
    const volatile uint64_t *sp = (const volatile uint64_t *)rsp;
    kprintf(PRINT_BOTH, "--- STACK (24 qwords from rsp) ---\n");
    for (int i = 0; i < 24; i++) {
        uint64_t v = sp[i];
        kprintf(PRINT_BOTH, "  [");
        kprintf(PRINT_BOTH, "%llx", (unsigned long long)((uint64_t)i * 8));
        kprintf(PRINT_BOTH, "] 0x");
        kprintf(PRINT_BOTH, "%llx", (unsigned long long)(v));
        if (v >= 0xffffffff80000000ULL && v < 0xffffffff80100000ULL) kprintf(PRINT_BOTH, "  <- kernel image");
        kprintchar('\n', PRINT_BOTH);
    }
}

static void dump_task(void) {
    struct task *t = task_current();
    kprintf(PRINT_BOTH, "--- CURRENT TASK ---\n  name=");
    kprintf(PRINT_BOTH, "%s", t->name ? t->name : "?");
    kprintf(PRINT_BOTH, "  in_use=");
    kprintchar(t->in_use ? 'Y' : 'N', PRINT_BOTH);
    kprintf(PRINT_BOTH, "  switches=");
    kprintf(PRINT_BOTH, "%llx", (unsigned long long)(task_switch_count()));
    kprintf(PRINT_BOTH, "  frame=0x");
    kprintf(PRINT_BOTH, "%llx", (unsigned long long)((uint64_t)t->frame));
    if (t->frame) {
        const uint64_t *q = (const uint64_t *)t->frame;
        kprintf(PRINT_BOTH, "\n  frame rip=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(q[17]));
        kprintf(PRINT_BOTH, " cs=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(q[18]));
        kprintf(PRINT_BOTH, " rflags=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(q[19]));
        kprintf(PRINT_BOTH, " rsp=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(q[20]));
        kprintf(PRINT_BOTH, " ss=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(q[21]));
    }
    kprintf(PRINT_BOTH, "\n  stack_base=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(t->stack_base));
    kprintf(PRINT_BOTH, "  stack_top=0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(t->stack_top));
    kprintchar('\n', PRINT_BOTH);
}

void panic_dump_regs(struct panic_context* ctx) {
    struct isr_frame* f = &ctx->frame;
    struct limine_framebuffer* fb_ptr = framebuffer_request.response ? framebuffer_request.response->framebuffers[0] : NULL;
    volatile uint32_t* fb = fb_ptr ? (volatile uint32_t*)fb_ptr->address : NULL;
    uint32_t width = fb_ptr ? fb_ptr->width : 0;
    uint32_t height = fb_ptr ? fb_ptr->height : 0;

    kprintf_attach(fb, width, height);
    kprintf_clear();

    kprintf_home(0, 0);

    kprintf(PRINT_BOTH, "\n========== KERNEL PANIC ==========\n");

    if (ctx->reason) {
        kprintf(PRINT_BOTH, "Assertion: ");
        kprintf(PRINT_BOTH, "%s", ctx->reason);
        kprintchar('\n', PRINT_BOTH);
    } else {
        const char* exc_name;
        if (f->interrupt_number == 255) {
            exc_name = "Unexpected interrupt (unhandled vector 48-255)";
        } else if (f->interrupt_number < 32) {
            exc_name = exception_names[f->interrupt_number];
        } else {
            exc_name = "Unexpected interrupt";
        }
        char num[4];
        num[0] = '0' + (f->interrupt_number / 100);
        num[1] = '0' + ((f->interrupt_number / 10) % 10);
        num[2] = '0' + (f->interrupt_number % 10);
        num[3] = 0;
        kprintf(PRINT_BOTH, "Exception: ");
        kprintf(PRINT_BOTH, "%s", exc_name);
        kprintf(PRINT_BOTH, " (vector ");
        kprintf(PRINT_BOTH, "%s", num);
        kprintf(PRINT_BOTH, ")\n");
    }

    __asm__ volatile("mov %%cr0, %0" : "=r"(ctx->cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(ctx->cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(ctx->cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(ctx->cr4));
    __asm__ volatile("mov %%rsp, %0" : "=r"(ctx->rsp_at_panic));

    kprintf(PRINT_BOTH, "RIP: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rip)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "CS:  0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->cs)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RFLAGS: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rflags)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RSP: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->rsp_at_panic)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RAX: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rax)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RBX: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rbx)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RCX: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rcx)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RDX: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rdx)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RSI: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rsi)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RDI: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rdi)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "RBP: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->rbp)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R8:  0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r8)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R9:  0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r9)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R10: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r10)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R11: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r11)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R12: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r12)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R13: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r13)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R14: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r14)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "R15: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->r15)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "Error Code: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(f->error_code)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "CR0: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr0)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "CR2: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr2)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "CR3: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr3)); kprintchar('\n', PRINT_BOTH);
    kprintf(PRINT_BOTH, "CR4: 0x"); kprintf(PRINT_BOTH, "%llx", (unsigned long long)(ctx->cr4)); kprintchar('\n', PRINT_BOTH);

    kprintf_home(width / 2, 0);

    kprintf(PRINT_BOTH, "========== HALTING ==========\n");
    dump_syscall_state(ctx);
    if (ctx->has_pf) dump_pf(ctx, f->error_code);
    dump_stack(ctx->rsp_at_panic);
    dump_task();
    for (;;) __asm__ volatile("hlt");
}

static void capture_frame(struct isr_frame* f) {
    uint64_t g[15];
    uint64_t rip;

    __asm__ volatile(
        "mov %%r15,  0(%1)\n\t"
        "mov %%r14,  8(%1)\n\t"
        "mov %%r13, 16(%1)\n\t"
        "mov %%r12, 24(%1)\n\t"
        "mov %%r11, 32(%1)\n\t"
        "mov %%r10, 40(%1)\n\t"
        "mov %%r9,  48(%1)\n\t"
        "mov %%r8,  56(%1)\n\t"
        "mov %%rbp, 64(%1)\n\t"
        "mov %%rdi, 72(%1)\n\t"
        "mov %%rsi, 80(%1)\n\t"
        "mov %%rdx, 88(%1)\n\t"
        "mov %%rcx, 96(%1)\n\t"
        "mov %%rbx,104(%1)\n\t"
        "mov %%rax,112(%1)\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "mov %%rax,%0\n\t"
        "1:\n\t"
        : "=r"(rip) : "r"(g) : "rax", "cc", "memory"
    );

    f->r15 = g[0];  f->r14 = g[1];  f->r13 = g[2];  f->r12 = g[3];
    f->r11 = g[4];  f->r10 = g[5];  f->r9  = g[6];  f->r8  = g[7];
    f->rbp = g[8];  f->rdi = g[9];  f->rsi = g[10]; f->rdx = g[11];
    f->rcx = g[12]; f->rbx = g[13]; f->rax = g[14];
    f->rip = rip;
}

void panic_assert(const char* msg) {
    struct panic_context ctx;
    uint8_t* z = (uint8_t*)&ctx;
    for (size_t i = 0; i < sizeof(ctx); i++) z[i] = 0;

    capture_frame(&ctx.frame);
    ctx.reason = msg;
    ctx.has_pf = 0;

    panic_dump_regs(&ctx);

    for (;;) __asm__ volatile("hlt");
}

void panic(const char* msg __attribute__((unused)), struct isr_frame* frame) {
    struct panic_context ctx;
    uint8_t* z = (uint8_t*)&ctx;
    for (size_t i = 0; i < sizeof(ctx); i++) z[i] = 0;

    ctx.frame = *frame;
    ctx.reason = NULL;
    ctx.has_pf = (frame->interrupt_number == 14);

    panic_dump_regs(&ctx);
}