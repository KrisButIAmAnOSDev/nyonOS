#include "kernel/tty/tty.h"

#include "abi/abi.h"
#include "drivers/keyboard/keyboard.h"
#include "kernel/fd/fd.h"
#include "kernel/multitask/task.h"

#define TTY_ICANON 0000002
#define TTY_ISIG   0000001
#define TTY_ECHO   0000010
#define TTY_VINTR  0
#define TTY_VEOF   4
#define TTY_VKILL  3

static struct sys_termios console_termios;
static struct sys_winsize console_winsize = { 25, 80, 0, 0 };
static bool tty_ready;

void tty_set_display_size(uint16_t rows, uint16_t cols) {
    console_winsize.ws_row = rows;
    console_winsize.ws_col = cols;
}

void tty_init(void) {
    console_termios.c_iflag = 0;
    console_termios.c_oflag = 0;
    console_termios.c_cflag = 0;
    console_termios.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO;
    console_termios.c_line = 0;
    console_termios.c_cc[TTY_VINTR] = 3;   /* ^C */
    console_termios.c_cc[TTY_VEOF] = 4;    /* ^D */
    console_termios.c_cc[TTY_VKILL] = 3;   /* ^\ */
    console_termios.c_ibaud = 0;
    console_termios.c_obaud = 0;
    console_winsize.ws_row = 25;
    console_winsize.ws_col = 80;
    tty_ready = true;
}

bool tty_termios_get(int fd, struct sys_termios *out) {
    (void)fd;
    if (!tty_ready) tty_init();
    *out = console_termios;
    return true;
}

bool tty_termios_set(int fd, int act, const struct sys_termios *in) {
    (void)fd;
    (void)act;
    if (!tty_ready) tty_init();
    console_termios = *in;
    return true;
}

bool tty_winsize_get(int fd, struct sys_winsize *out) {
    (void)fd;
    *out = console_winsize;
    return true;
}

bool tty_winsize_set(int fd, const struct sys_winsize *in) {
    (void)fd;
    console_winsize = *in;
    return true;
}

bool tty_name(int fd, char *buf, size_t size) {
    (void)fd;
    if (!size) return false;
    const char *n = "console";
    size_t i = 0;
    while (n[i] && i + 1 < size) { buf[i] = n[i]; i++; }
    buf[i] = 0;
    return true;
}

bool tty_poll_ready(int fd, uint16_t events, uint16_t *revents) {
    (void)fd;
    *revents = 0;

    if (!tty_ready) tty_init();

    /* The console is the only pollable device that can block. */
    if (keyboard_owner() != 0) {
        if (events & ABI_POLLIN) *revents |= ABI_POLLIN;
    } else if (keyboard_line_ready()) {
        if (events & ABI_POLLIN) *revents |= ABI_POLLIN;
    }

    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;

    return *revents != 0;
}

bool tty_wait_input(uint64_t deadline) {
    task_wait(keyboard_wait_chan(), deadline);
    return keyboard_line_ready();
}
