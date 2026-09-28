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

static void dump_pf(struct panic_context* ctx, uint64_t err) {
    pout("--- PAGE FAULT DETAIL ---\n");
    pout("  cr2          = 0x");
    phex(ctx->cr2);
    pout("\n  err bits     = P:");
    poutc((err & (1ULL << 0)) ? '1' : '0');
    pout(" W:");
    poutc((err & (1ULL << 1)) ? '1' : '0');
    pout(" U:");
    poutc((err & (1ULL << 2)) ? '1' : '0');
    pout(" RSVD:");
    poutc((err & (1ULL << 3)) ? '1' : '0');
    pout(" FETCH:");
    poutc((err & (1ULL << 4)) ? '1' : '0');
    pout(" NX:");
    poutc((err & (1ULL << 34)) ? '1' : '0');
    pout("\n  cr0.WP      = ");
    poutc((ctx->cr0 & (1ULL << 16)) ? '1' : '0');
    pout("\n  cr4         = 0x");
    phex(ctx->cr4);
    uint64_t pte = vmm_query(ctx->cr2);
    pout("\n  leaf PTE    = 0x");
    phex(pte);
    if (pte == 0) {
        pout("   (no mapping found)");
    } else {
        pout("   P:");
        poutc((pte & PAGE_PRESENT) ? '1' : '0');
        pout(" W:");
        poutc((pte & PAGE_WRITE) ? '1' : '0');
        pout(" U:");
        poutc((pte & PAGE_USER) ? '1' : '0');
        pout(" NX:");
        poutc((pte & PAGE_NX) ? '1' : '0');
    }
    pout("\n  cr3         = 0x");
    phex(ctx->cr3);
    poutc('\n');
}

static void dump_stack(uint64_t rsp) {
    const volatile uint64_t *sp = (const volatile uint64_t *)rsp;
    pout("--- STACK (24 qwords from rsp) ---\n");
    for (int i = 0; i < 24; i++) {
        uint64_t v = sp[i];
        pout("  [");
        phex((uint64_t)i * 8);
        pout("] 0x");
        phex(v);
        if (v >= 0xffffffff80000000ULL && v < 0xffffffff80100000ULL) pout("  <- kernel image");
        poutc('\n');
    }
}

static void dump_task(void) {
    struct task *t = task_current();
    pout("--- CURRENT TASK ---\n  name=");
    pout(t->name ? t->name : "?");
    pout("  in_use=");
    poutc(t->in_use ? 'Y' : 'N');
    pout("  switches=");
    phex(task_switch_count());
    pout("  frame=0x");
    phex((uint64_t)t->frame);
    if (t->frame) {
        const uint64_t *q = (const uint64_t *)t->frame;
        pout("\n  frame rip=0x"); phex(q[17]);
        pout(" cs=0x"); phex(q[18]);
        pout(" rflags=0x"); phex(q[19]);
        pout(" rsp=0x"); phex(q[20]);
        pout(" ss=0x"); phex(q[21]);
    }
    pout("\n  stack_base=0x"); phex(t->stack_base);
    pout("  stack_top=0x"); phex(t->stack_top);
    poutc('\n');
}

void panic_dump_regs(struct panic_context* ctx) {
    struct isr_frame* f = &ctx->frame;
    struct limine_framebuffer* fb_ptr = framebuffer_request.response ? framebuffer_request.response->framebuffers[0] : NULL;
    volatile uint32_t* fb = fb_ptr ? (volatile uint32_t*)fb_ptr->address : NULL;
    uint32_t width = fb_ptr ? fb_ptr->width : 0;
    uint32_t height = fb_ptr ? fb_ptr->height : 0;

    video_attach(fb, width, height);
    video_clear();

    video_home(0, 0);

    pout("\n========== KERNEL PANIC ==========\n");

    if (ctx->reason) {
        pout("Assertion: ");
        pout(ctx->reason);
        poutc('\n');
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
        pout("Exception: ");
        pout(exc_name);
        pout(" (vector ");
        pout(num);
        pout(")\n");
    }

    __asm__ volatile("mov %%cr0, %0" : "=r"(ctx->cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(ctx->cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(ctx->cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(ctx->cr4));
    __asm__ volatile("mov %%rsp, %0" : "=r"(ctx->rsp_at_panic));

    pout("RIP: 0x"); phex(f->rip); poutc('\n');
    pout("CS:  0x"); phex(f->cs); poutc('\n');
    pout("RFLAGS: 0x"); phex(f->rflags); poutc('\n');
    pout("RSP: 0x"); phex(ctx->rsp_at_panic); poutc('\n');
    pout("RAX: 0x"); phex(f->rax); poutc('\n');
    pout("RBX: 0x"); phex(f->rbx); poutc('\n');
    pout("RCX: 0x"); phex(f->rcx); poutc('\n');
    pout("RDX: 0x"); phex(f->rdx); poutc('\n');
    pout("RSI: 0x"); phex(f->rsi); poutc('\n');
    pout("RDI: 0x"); phex(f->rdi); poutc('\n');
    pout("RBP: 0x"); phex(f->rbp); poutc('\n');
    pout("R8:  0x"); phex(f->r8); poutc('\n');
    pout("R9:  0x"); phex(f->r9); poutc('\n');
    pout("R10: 0x"); phex(f->r10); poutc('\n');
    pout("R11: 0x"); phex(f->r11); poutc('\n');
    pout("R12: 0x"); phex(f->r12); poutc('\n');
    pout("R13: 0x"); phex(f->r13); poutc('\n');
    pout("R14: 0x"); phex(f->r14); poutc('\n');
    pout("R15: 0x"); phex(f->r15); poutc('\n');
    pout("Error Code: 0x"); phex(f->error_code); poutc('\n');
    pout("CR0: 0x"); phex(ctx->cr0); poutc('\n');
    pout("CR2: 0x"); phex(ctx->cr2); poutc('\n');
    pout("CR3: 0x"); phex(ctx->cr3); poutc('\n');
    pout("CR4: 0x"); phex(ctx->cr4); poutc('\n');

    video_home(width / 2, 0);

    pout("========== HALTING ==========\n");
    if (ctx->has_pf) dump_pf(ctx, f->error_code);
    dump_stack(ctx->rsp_at_panic);
    dump_task();
    for (;;) __asm__ volatile("hlt");
}

static void capture_frame(struct isr_frame* f) {
    uint64_t g[15];
    uint64_t rip;

    __asm__ volatile(
        "mov %%r15,  0(%0)\n\t"
        "mov %%r14,  8(%0)\n\t"
        "mov %%r13, 16(%0)\n\t"
        "mov %%r12, 24(%0)\n\t"
        "mov %%r11, 32(%0)\n\t"
        "mov %%r10, 40(%0)\n\t"
        "mov %%r9,  48(%0)\n\t"
        "mov %%r8,  56(%0)\n\t"
        "mov %%rbp, 64(%0)\n\t"
        "mov %%rdi, 72(%0)\n\t"
        "mov %%rsi, 80(%0)\n\t"
        "mov %%rdx, 88(%0)\n\t"
        "mov %%rcx, 96(%0)\n\t"
        "mov %%rbx,104(%0)\n\t"
        "mov %%rax,112(%0)\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "mov %%rax,%1\n\t"
        "1:\n\t"
        : : "r"(g), "r"(rip) : "rax", "cc", "memory"
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