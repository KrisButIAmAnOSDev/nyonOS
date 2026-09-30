#include "gdt.h"
#include "tss.h"
#include "io/serial/serial.h"
#include "io/kprintf/kprintf.h"
#include <stddef.h>

static uint8_t gdt[GDT_ENTRIES * 8];

uint8_t gdt_tss_desc[16];

extern void gdt_load(struct gdt_pointer *gdtr);

uint16_t gdt_rd_cs(void);
uint16_t gdt_rd_ds(void);
uint16_t gdt_rd_es(void);
uint16_t gdt_rd_ss(void);
uint16_t gdt_rd_fs(void);
uint16_t gdt_rd_gs(void);
uint16_t gdt_rd_tr(void);

__asm__(
    ".text\n"
    ".globl gdt_rd_cs\n"
    ".type gdt_rd_cs,@function\n"
    "gdt_rd_cs:\n"
    "    mov %cs, %ax\n"
    "    ret\n"
    ".globl gdt_rd_ds\n"
    ".type gdt_rd_ds,@function\n"
    "gdt_rd_ds:\n"
    "    mov %ds, %ax\n"
    "    ret\n"
    ".globl gdt_rd_es\n"
    ".type gdt_rd_es,@function\n"
    "gdt_rd_es:\n"
    "    mov %es, %ax\n"
    "    ret\n"
    ".globl gdt_rd_ss\n"
    ".type gdt_rd_ss,@function\n"
    "gdt_rd_ss:\n"
    "    mov %ss, %ax\n"
    "    ret\n"
    ".globl gdt_rd_fs\n"
    ".type gdt_rd_fs,@function\n"
    "gdt_rd_fs:\n"
    "    mov %fs, %ax\n"
    "    ret\n"
    ".globl gdt_rd_gs\n"
    ".type gdt_rd_gs,@function\n"
    "gdt_rd_gs:\n"
    "    mov %gs, %ax\n"
    "    ret\n"
    ".globl gdt_rd_tr\n"
    ".type gdt_rd_tr,@function\n"
    "gdt_rd_tr:\n"
    "    str %ax\n"
    "    ret\n"
    ".previous\n"
);

static void gdt_encode(uint8_t *t, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    t[0] = limit & 0xFF;
    t[1] = (limit >> 8) & 0xFF;
    t[2] = base & 0xFF;
    t[3] = (base >> 8) & 0xFF;
    t[4] = (base >> 16) & 0xFF;
    t[5] = access;
    t[6] = ((limit >> 16) & 0x0F) | (flags << 4);
    t[7] = (base >> 24) & 0xFF;
}

static void gdt_encode_tss(uint8_t *t, uint64_t base, uint32_t limit) {
    gdt_encode(t, (uint32_t)base, limit, 0x89, 0x0);
    t[8]  = (base >> 32) & 0xFF;
    t[9]  = (base >> 40) & 0xFF;
    t[10] = (base >> 48) & 0xFF;
    t[11] = (base >> 56) & 0xFF;
    t[12] = 0;
    t[13] = 0;
    t[14] = 0;
    t[15] = 0;
}

static uint64_t entry_qword(const uint8_t *d) {
    uint64_t q = 0;
    for (int i = 7; i >= 0; i--) q = (q << 8) | d[i];
    return q;
}

void gdt_init(uint64_t hhdm_offset) {
    tss_init(hhdm_offset);

    for (size_t i = 0; i < sizeof(gdt); i++) gdt[i] = 0;

    gdt_encode(&gdt[0x08], 0, 0x000FFFFF, 0x9B, 0xA);
    gdt_encode(&gdt[0x10], 0, 0x000FFFFF, 0x93, 0xC);
    gdt_encode(&gdt[0x18], 0, 0x000FFFFF, 0xF3, 0xC);
    gdt_encode(&gdt[0x20], 0, 0x000FFFFF, 0xFB, 0xA);
    gdt_encode_tss(&gdt[0x28], tss_addr(), sizeof(struct tss) - 1);

    for (size_t i = 0; i < 16; i++) gdt_tss_desc[i] = gdt[0x28 + i];

    struct gdt_pointer gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)gdt
    };

    gdt_load(&gdtr);

    kprintf(PRINT_SERIAL, "GDT: loaded, TSS limit=");
    kprintf(PRINT_SERIAL, "%llx", (unsigned)(sizeof(struct tss) - 1));
    kprintf(PRINT_SERIAL, " ist=");
    for (int i = 0; i < TSS_IST_COUNT; i++) {
        kprintchar('1' + i, PRINT_SERIAL);
    }
    kprintchar('\n', PRINT_SERIAL);
}

void gdt_dump(void) {
    struct gdt_pointer gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));

    kprintf(PRINT_SERIAL, "GDT base=0x");
    kprintf(PRINT_SERIAL, "%llx", (unsigned long long)(gdtr.base));
    kprintf(PRINT_SERIAL, " limit=0x");
    kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdtr.limit));
    kprintchar('\n', PRINT_SERIAL);

    size_t count = ((size_t)gdtr.limit + 1) / 8;
    const uint8_t *table = (const uint8_t *)gdtr.base;

    for (size_t i = 0; i < count; i++) {
        kprintf(PRINT_SERIAL, "  0x");
        kprintf(PRINT_SERIAL, "%llx", (unsigned)(i * 8));
        kprintf(PRINT_SERIAL, ": 0x");
        kprintf(PRINT_SERIAL, "%llx", (unsigned long long)(entry_qword(table + i * 8)));
        kprintchar('\n', PRINT_SERIAL);
    }

    kprintf(PRINT_SERIAL, "cs=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_cs()));
    kprintf(PRINT_SERIAL, " ds=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_ds()));
    kprintf(PRINT_SERIAL, " es=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_es()));
    kprintf(PRINT_SERIAL, " ss=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_ss()));
    kprintf(PRINT_SERIAL, " fs=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_fs()));
    kprintf(PRINT_SERIAL, " gs=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_gs()));
    kprintf(PRINT_SERIAL, " tr=0x"); kprintf(PRINT_SERIAL, "%llx", (unsigned)(gdt_rd_tr()));
    kprintchar('\n', PRINT_SERIAL);
}
