#include "gdt.h"
#include "tss.h"
#include "io/serial/serial.h"
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

static void print_hex64(uint64_t val) {
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        serial_putchar(c);
    }
}

static void print_hex8(uint64_t val) {
    for (int i = 1; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        serial_putchar(c);
    }
}

static void print_hex16(uint64_t val) {
    for (int i = 3; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        serial_putchar(c);
    }
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
    gdt_encode(&gdt[0x18], 0, 0x000FFFFF, 0xFB, 0xA);
    gdt_encode(&gdt[0x20], 0, 0x000FFFFF, 0xF3, 0xC);
    gdt_encode_tss(&gdt[0x28], tss_addr(), sizeof(struct tss) - 1);

    for (size_t i = 0; i < 16; i++) gdt_tss_desc[i] = gdt[0x28 + i];

    struct gdt_pointer gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)gdt
    };

    gdt_load(&gdtr);

    serial_print("GDT: loaded, TSS limit=");
    print_hex16(sizeof(struct tss) - 1);
    serial_print(" ist=");
    for (int i = 0; i < TSS_IST_COUNT; i++) {
        serial_putchar('1' + i);
    }
    serial_putchar('\n');
}

void gdt_dump(void) {
    struct gdt_pointer gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));

    serial_print("GDT base=0x");
    print_hex64(gdtr.base);
    serial_print(" limit=0x");
    print_hex16(gdtr.limit);
    serial_putchar('\n');

    size_t count = ((size_t)gdtr.limit + 1) / 8;
    const uint8_t *table = (const uint8_t *)gdtr.base;

    for (size_t i = 0; i < count; i++) {
        serial_print("  0x");
        print_hex8(i * 8);
        serial_print(": 0x");
        print_hex64(entry_qword(table + i * 8));
        serial_putchar('\n');
    }

    serial_print("cs=0x"); print_hex16(gdt_rd_cs());
    serial_print(" ds=0x"); print_hex16(gdt_rd_ds());
    serial_print(" es=0x"); print_hex16(gdt_rd_es());
    serial_print(" ss=0x"); print_hex16(gdt_rd_ss());
    serial_print(" fs=0x"); print_hex16(gdt_rd_fs());
    serial_print(" gs=0x"); print_hex16(gdt_rd_gs());
    serial_print(" tr=0x"); print_hex16(gdt_rd_tr());
    serial_putchar('\n');
}
