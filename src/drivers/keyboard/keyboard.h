#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "abi/abi.h"
#include "arch/x86_64/idt/idt.h"

void keyboard_init(void);
void keyboard_handler(struct isr_frame *frame);
void keyboard_process_buffer(void);
bool keyboard_try_pop(char *out);
void keyboard_set_owner(uint32_t pid);
uint32_t keyboard_owner(void);
bool keyboard_inject_char(char c);
bool keyboard_line_ready(void);
bool keyboard_has_input(void);
bool keyboard_has_char(char c);
void *keyboard_wait_chan(void);
void keyboard_event_push(uint16_t code, int32_t value);
uint32_t keyboard_event_seq(void);
bool keyboard_event_pop_at(struct sys_input_event *out, uint32_t *seq);
bool keyboard_event_available_at(uint32_t seq);
void *keyboard_input_wait_chan(void);

#endif