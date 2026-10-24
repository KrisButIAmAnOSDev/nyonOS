#ifndef _TTY_H
#define _TTY_H

#include <stdint.h>
#include <stdbool.h>

#include "abi/abi.h"

void tty_init(void);

bool tty_termios_get(int fd, struct sys_termios *out);
bool tty_termios_set(int fd, int act, const struct sys_termios *in);
bool tty_canonical(void);
bool tty_echo(void);
char tty_eof_char(void);
bool tty_input_ready(void);
bool tty_winsize_get(int fd, struct sys_winsize *out);
bool tty_winsize_set(int fd, const struct sys_winsize *in);
bool tty_name(int fd, char *buf, size_t size);

bool tty_poll_ready(int fd, uint16_t events, uint16_t *revents);
bool tty_wait_input(uint64_t deadline);

void tty_set_display_size(uint16_t rows, uint16_t cols);

#endif
