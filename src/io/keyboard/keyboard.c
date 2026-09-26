#include "keyboard.h"
#include "arch/x86_64/pic/pic.h"
#include "io/serial/serial.h"
#include "video/video.h"
#include "font/font.h"
#include "limine/limine.h"
#include <stdbool.h>

__attribute__((used, section(".limine_requests")))
extern volatile struct limine_framebuffer_request framebuffer_request;

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
    bool extended = false;
    static bool extended_prev = false;
    
    if (scancode == 0xE0) {
        extended_prev = true;
        return 0;
    }
    
    if (extended_prev) {
        extended_prev = false;
        extended = true;
    }
    
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

void keyboard_handler(void) {
    keyboard_wait_output();
    uint8_t scancode = inb(0x60);
    keyboard_buffer_push(scancode);
    pic_send_eoi(1);
}

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static bool framebuffer_ready = false;

static void put_char_fb(char c) {
    if (!framebuffer_ready) return;
    
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint32_t width = fb->width;
    uint32_t height = fb->height;
    
    if (c == '\n') {
        cursor_x = 0;
        cursor_y += 16;
    } else if (c == '\b') {
        if (cursor_x >= 8) {
            cursor_x -= 8;
            draw_char(fb_ptr, ' ', cursor_x, cursor_y, 0x000000, width);
        }
    } else if (c >= 32 && c <= 126) {
        draw_char(fb_ptr, c, cursor_x, cursor_y, 0xffffff, width);
        cursor_x += 8;
    }
    
    if (cursor_x >= width - 8) {
        cursor_x = 0;
        cursor_y += 16;
    }
    if (cursor_y >= height - 16) {
        cursor_y = 0;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                fb_ptr[y * width + x] = 0x000000;
            }
        }
    }
}

void keyboard_process_buffer(void) {
    if (!framebuffer_ready) {
        if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
            framebuffer_ready = true;
            cursor_x = 0;
            cursor_y = 200;
        }
    }
    
    uint8_t scancode;
    while (keyboard_buffer_pop(&scancode)) {
        uint8_t ascii = scancode_to_ascii(scancode);
        if (ascii) {
            put_char_fb(ascii);
            serial_print("KEY: ");
            char buf[2];
            buf[0] = ascii;
            buf[1] = 0;
            serial_print(buf);
            serial_print("\n");
        }
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
    keyboard_wait_input();
    outb(0x64, 0x60);
    keyboard_wait_output();
    outb(0x60, status);
    
    keyboard_wait_input();
    outb(0x64, 0xAE);
    keyboard_wait_output();
    outb(0x60, 0xF4);
    keyboard_wait_output();
    
    outb(0x64, 0xA8);
    keyboard_wait_output();
    outb(0x60, 0xF4);
    keyboard_wait_output();
    
    pic_clear_mask(1);
    
    serial_print("Keyboard: Initialized\n");
}