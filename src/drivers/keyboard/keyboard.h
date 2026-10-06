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
void keyboard_claim_stdin(void);
bool keyboard_stdin_owned(void);
bool keyboard_inject_char(char c);

#endif