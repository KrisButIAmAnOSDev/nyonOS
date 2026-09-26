#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

void keyboard_init(void);
void keyboard_handler(void);
void keyboard_process_buffer(void);

#endif