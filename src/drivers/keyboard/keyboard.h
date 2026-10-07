#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86_64/idt/idt.h"

void keyboard_init(void);
void keyboard_handler(struct isr_frame *frame);
void keyboard_process_buffer(void);
bool keyboard_try_pop(char *out);
void keyboard_set_owner(uint32_t pid);
uint32_t keyboard_owner(void);
bool keyboard_inject_char(char c);
bool keyboard_line_ready(void);
void *keyboard_wait_chan(void);

#endif