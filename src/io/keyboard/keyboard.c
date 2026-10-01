#include "keyboard.h"
#include "io/kprintf/kprintf.h"
#include "arch/x86_64/pic/pic.h"
#include "kernel/multitask/task.h"
#include <stdbool.h>

static uint8_t keyboard_buffer[256];
static size_t buffer_head = 0;
static size_t buffer_tail = 0;
static bool shift_pressed = false;
static bool caps_lock = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void keyboard_wait_input(void) {
    while (inb(0x64) & 0x02);
}

static void keyboard_wait_output(void) {
    while (!(inb(0x64) & 0x01));
}

static void keyboard_buffer_push(uint8_t scancode) {
    size_t next = (buffer_head + 1) % 256;
    if (next != buffer_tail) {
        keyboard_buffer[buffer_head] = scancode;
        buffer_head = next;
    }
}

static bool keyboard_buffer_pop(uint8_t *scancode) {
    if (buffer_head == buffer_tail) return false;
    *scancode = keyboard_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % 256;
    return true;
}

static const uint8_t scancode_set1[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint8_t scancode_set1_shift[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static uint8_t scancode_to_ascii(uint8_t scancode) {
    if (scancode == 0xE0) return 0;

    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) shift_pressed = false;
        else if (released == 0x1D) ctrl_pressed = false;
        else if (released == 0x38) alt_pressed = false;
        return 0;
    }

    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = true; return 0; }
    if (scancode == 0x1D) { ctrl_pressed = true; return 0; }
    if (scancode == 0x38) { alt_pressed = true; return 0; }
    if (scancode == 0x3A) { caps_lock = !caps_lock; return 0; }

    uint8_t ascii = 0;
    if (shift_pressed ^ caps_lock) {
        if (scancode < sizeof(scancode_set1_shift)) ascii = scancode_set1_shift[scancode];
    } else {
        if (scancode < sizeof(scancode_set1)) ascii = scancode_set1[scancode];
    }

    return ascii;
}

void keyboard_handler(struct isr_frame *frame) {
    (void)frame;
    keyboard_wait_output();
    uint8_t scancode = inb(0x60);
    keyboard_buffer_push(scancode);

    task_unblock_all();
}

bool keyboard_try_pop(char *out) {
    uint8_t scancode;
    if (!keyboard_buffer_pop(&scancode)) return false;

    char c = (char)scancode_to_ascii(scancode);
    if (!c) return false;

    *out = c;
    return true;
}

void keyboard_process_buffer(void) {
    char c;
    while (keyboard_try_pop(&c)) {
        kprintf(KATTR(PRINT_SCREEN, COLOR_WHITE), "%c", c);
    }
}

void keyboard_init(void) {
    keyboard_wait_input();
    outb(0x64, 0xAE);

    keyboard_wait_input();
    outb(0x64, 0x20);
    keyboard_wait_output();
    uint8_t status = inb(0x60);
    status |= 0x01;
    status &= ~0x10;
    status |= 0x40;
    keyboard_wait_input();
    outb(0x64, 0x60);
    keyboard_wait_input();
    outb(0x60, status);

    keyboard_wait_input();
    outb(0x64, 0xAE);
    keyboard_wait_input();
    outb(0x60, 0xF4);
    keyboard_wait_output();

    keyboard_wait_input();
    outb(0x64, 0xA8);
    keyboard_wait_input();
    outb(0x60, 0xF4);
    keyboard_wait_output();

    pic_clear_mask(1);

    kprintf(PRINT_SERIAL, "Keyboard: Initialized\n");
}
