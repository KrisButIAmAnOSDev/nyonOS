#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>
#include "arch/x86_64/idt/idt.h"

void keyboard_init(void);
void keyboard_handler(struct isr_frame *frame);
void keyboard_process_buffer(void);

#endif