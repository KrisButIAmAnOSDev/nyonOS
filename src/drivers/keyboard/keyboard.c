#include "keyboard.h"
#include "kernel/kprintf/kprintf.h"
#include "arch/x86_64/pic/pic.h"
#include "kernel/multitask/task.h"
#include <stdbool.h>
#include "kernel/sync/preempt.h"

static char keyboard_buffer[256];
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

static char tty_wait_chan;

static bool keyboard_buffer_push(char c) {
    if (c == '\b') {
        size_t head = __atomic_load_n(&buffer_head, __ATOMIC_ACQUIRE);
        size_t tail = __atomic_load_n(&buffer_tail, __ATOMIC_ACQUIRE);
        if (head == tail) return false;
        size_t prev = (head + 255) % 256;
        if (keyboard_buffer[prev] == '\n') return false;
        __atomic_store_n(&buffer_head, prev, __ATOMIC_RELEASE);
        return true;
    }
    size_t head = __atomic_load_n(&buffer_head, __ATOMIC_RELAXED);
    size_t tail = __atomic_load_n(&buffer_tail, __ATOMIC_ACQUIRE);
    size_t next = (head + 1) % 256;
    if (next == tail) return false;
    keyboard_buffer[head] = c;
    __atomic_store_n(&buffer_head, next, __ATOMIC_RELEASE);
    return true;
}

void *keyboard_wait_chan(void) {
    return &tty_wait_chan;
}

bool keyboard_line_ready(void) {
    size_t head = __atomic_load_n(&buffer_head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&buffer_tail, __ATOMIC_ACQUIRE);
    if ((head + 1) % 256 == tail) return true;
    for (size_t i = tail; i != head; i = (i + 1) % 256) {
        if (keyboard_buffer[i] == '\n') return true;
    }
    return false;
}

static bool keyboard_buffer_pop(char *c) {
    size_t head = __atomic_load_n(&buffer_head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&buffer_tail, __ATOMIC_RELAXED);
    if (head == tail) return false;
    *c = keyboard_buffer[tail];
    __atomic_store_n(&buffer_tail, (tail + 1) % 256, __ATOMIC_RELEASE);
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

static uint32_t stdin_owner = 0;

void keyboard_set_owner(uint32_t pid) {
    __atomic_store_n(&stdin_owner, pid, __ATOMIC_RELEASE);
}

uint32_t keyboard_owner(void) {
    return __atomic_load_n(&stdin_owner, __ATOMIC_ACQUIRE);
}

void keyboard_handler(struct isr_frame *frame) {
    (void)frame;
    if (!(inb(0x64) & 0x01)) return;
    uint8_t sc = inb(0x60);
    char c = (char)scancode_to_ascii(sc);
    if (!c) return;

    keyboard_buffer_push(c);

    if (keyboard_owner() == 0 && !kprintf_busy()) {
        if (c == '\b') kprintf(PRINT_SCREEN, " \b\b");
        else kprintf(PRINT_SCREEN, "%c", c);
    }

    task_wake_chan(&tty_wait_chan);
    task_unblock_all();
}

bool keyboard_try_pop(char *out) {
    char c;
    while (keyboard_buffer_pop(&c)) {
        if (c) {
            *out = c;
            return true;
        }
    }
    return false;
}

bool keyboard_inject_char(char c) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    bool kept = keyboard_buffer_push(c);
    task_wake_chan(&tty_wait_chan);
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return kept;
}

void keyboard_process_buffer(void) {
    if (keyboard_owner()) return;

    char c;
    while (keyboard_try_pop(&c)) {
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
