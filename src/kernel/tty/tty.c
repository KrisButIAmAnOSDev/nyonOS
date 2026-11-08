#include "kernel/tty/tty.h"

#include "kernel/lib/string.h"

#include "abi/abi.h"
#include "drivers/keyboard/keyboard.h"
#include "kernel/fd/fd.h"
#include "kernel/multitask/task.h"

#define TTY_ISIG   0000001
#define TTY_ICANON 0000002
#define TTY_ECHO   0000010
#define TTY_ECHOE  0000020
#define TTY_ECHOK  0000040
#define TTY_IEXTEN 0100000

#define TTY_ISTRIP 0000040
#define TTY_INLCR  0000100
#define TTY_IGNCR  0000200
#define TTY_ICRNL  0000400
#define TTY_IXON   0002000

#define TTY_OPOST  0000001
#define TTY_ONLCR  0000004

#define TTY_VINTR  0
#define TTY_VQUIT  1
#define TTY_VERASE 2
#define TTY_VKILL  3
#define TTY_VEOF   4
#define TTY_VMIN   6
#define TTY_VSTART 8
#define TTY_VSTOP  9
#define TTY_VSUSP  10
#define TTY_VEOL   11
#define TTY_VWERASE 14
#define TTY_VEOL2  16

#define CANON_MAX 1024

static struct sys_termios console_termios;
static struct sys_winsize console_winsize = { 25, 80, 0, 0 };
static bool tty_ready;
static bool winsize_known;
static uint32_t tty_fg_pgrp;
static uint32_t tty_ctty_sid;

static char canon_buf[CANON_MAX];
static uint32_t canon_len;
static uint32_t canon_done;
static uint16_t canon_lines[64];
static uint32_t canon_nlines;
static uint32_t canon_head;

static void canon_push_line(uint32_t len) {
    if (canon_nlines >= 64) return;
    canon_lines[(canon_head + canon_nlines) % 64] = (uint16_t)len;
    canon_nlines++;
}

void tty_set_display_size(uint16_t rows, uint16_t cols) {
    console_winsize.ws_row = rows;
    console_winsize.ws_col = cols;
    winsize_known = true;
}

void tty_init(void) {
    console_termios.c_iflag = TTY_ICRNL | TTY_IXON;
    console_termios.c_oflag = TTY_OPOST | TTY_ONLCR;
    console_termios.c_cflag = 0;
    console_termios.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK;
    console_termios.c_line = 0;
    memset(console_termios.c_cc, 0, sizeof console_termios.c_cc);
    console_termios.c_cc[TTY_VINTR] = 3;
    console_termios.c_cc[TTY_VQUIT] = 28;
    console_termios.c_cc[TTY_VERASE] = 127;
    console_termios.c_cc[TTY_VKILL] = 21;
    console_termios.c_cc[TTY_VEOF] = 4;
    console_termios.c_cc[TTY_VSUSP] = 26;
    console_termios.c_cc[TTY_VSTART] = 17;
    console_termios.c_cc[TTY_VSTOP] = 19;
    console_termios.c_cc[TTY_VMIN] = 1;
    console_termios.c_cc[TTY_VWERASE] = 23;
    console_termios.c_ibaud = 0;
    console_termios.c_obaud = 0;
    if (!winsize_known) {
        console_winsize.ws_row = 25;
        console_winsize.ws_col = 80;
    }
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
    bool was_canon = (console_termios.c_lflag & TTY_ICANON) != 0;
    console_termios = *in;
    bool now_canon = (console_termios.c_lflag & TTY_ICANON) != 0;
    if (was_canon && !now_canon) {
        for (uint32_t i = 0; i < canon_len; i++) keyboard_unread(canon_buf[i]);
        canon_len = 0;
        canon_done = 0;
        canon_nlines = 0;
        canon_head = 0;
    }
    return true;
}

bool tty_canonical(void) {
    if (!tty_ready) tty_init();
    return (console_termios.c_lflag & TTY_ICANON) != 0;
}

bool tty_echo(void) {
    if (!tty_ready) tty_init();
    return (console_termios.c_lflag & TTY_ECHO) != 0;
}

char tty_eof_char(void) {
    if (!tty_ready) tty_init();
    return (char)console_termios.c_cc[TTY_VEOF];
}

uint32_t tty_oflag(void) {
    if (!tty_ready) tty_init();
    return console_termios.c_oflag;
}

static void canon_erase(void) {
    if (canon_len > canon_done) canon_len--;
}

int tty_canon_input(uint8_t c, char *echo_out) {
    if (!tty_ready) tty_init();
    if (!(console_termios.c_lflag & TTY_ICANON)) return 0;
    if (echo_out) *echo_out = (char)c;

    if (c == '\r') {
        if (console_termios.c_iflag & TTY_IGNCR) return 3;
        if (console_termios.c_iflag & TTY_ICRNL) c = '\n';
    } else if (c == '\n') {
        if (console_termios.c_iflag & TTY_INLCR) c = '\r';
    }
    if (console_termios.c_iflag & TTY_ISTRIP) c &= 0x7f;
    if (echo_out) *echo_out = (char)c;

    if (c == (uint8_t)console_termios.c_cc[TTY_VKILL]) {
        canon_len = canon_done;
        return 3;
    }
    if (c == (uint8_t)console_termios.c_cc[TTY_VERASE] || c == 8 || c == 127) {
        canon_erase();
        return 2;
    }
    if ((console_termios.c_lflag & TTY_IEXTEN) &&
        c == (uint8_t)console_termios.c_cc[TTY_VWERASE]) {
        while (canon_len > canon_done && canon_buf[canon_len - 1] == ' ') canon_erase();
        while (canon_len > canon_done && canon_buf[canon_len - 1] != ' ') canon_erase();
        return 2;
    }
    if (c == (uint8_t)console_termios.c_cc[TTY_VEOF]) {
        canon_push_line(canon_len - canon_done);
        canon_done = canon_len;
        return 3;
    }
    if (c == '\n' || c == (uint8_t)console_termios.c_cc[TTY_VEOL] ||
        c == (uint8_t)console_termios.c_cc[TTY_VEOL2]) {
        if (canon_len < CANON_MAX) canon_buf[canon_len++] = (char)c;
        canon_push_line(canon_len - canon_done);
        canon_done = canon_len;
        return 1;
    }
    if (canon_len < CANON_MAX) canon_buf[canon_len++] = (char)c;
    return 1;
}

int tty_canon_read(void *buf, uint32_t len) {
    if (!tty_ready) tty_init();
    if (canon_nlines == 0) return FD_EAGAIN;

    uint32_t line = canon_lines[canon_head];
    if (line == 0) {
        canon_head = (canon_head + 1) % 64;
        canon_nlines--;
        return 0;
    }

    uint32_t n = line < len ? line : len;
    if (n) memcpy(buf, canon_buf, n);

    memmove(canon_buf, canon_buf + n, canon_len - n);
    canon_len -= n;
    canon_done -= n;

    if (n == line) {
        canon_head = (canon_head + 1) % 64;
        canon_nlines--;
    } else {
        canon_lines[canon_head] = (uint16_t)(line - n);
    }
    return (int)n;
}

bool tty_input_ready(void) {
    if (!tty_ready) tty_init();
    if (!tty_canonical()) return keyboard_has_input();
    return canon_nlines > 0;
}

bool tty_winsize_get(int fd, struct sys_winsize *out) {
    (void)fd;
    if (!tty_ready) tty_init();
    *out = console_winsize;
    return true;
}

bool tty_winsize_set(int fd, const struct sys_winsize *in) {
    (void)fd;
    console_winsize = *in;
    winsize_known = true;
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

    if (tty_input_ready()) {
        if (events & ABI_POLLIN) *revents |= ABI_POLLIN;
    }

    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;

    return *revents != 0;
}

bool tty_wait_input(uint64_t deadline) {
    task_wait(keyboard_wait_chan(), deadline);
    return tty_input_ready();
}

uint32_t tty_fg(void) {
    if (!tty_ready) tty_init();
    return tty_fg_pgrp;
}

void tty_set_fg(uint32_t pgid) {
    if (!tty_ready) tty_init();
    tty_fg_pgrp = pgid;
}

uint32_t tty_ctty_session(void) {
    if (!tty_ready) tty_init();
    return tty_ctty_sid;
}

bool tty_acquire_ctty(uint32_t session_id) {
    if (!tty_ready) tty_init();
    if (tty_ctty_sid && tty_ctty_sid != session_id) return false;
    tty_ctty_sid = session_id;
    tty_fg_pgrp = 0;
    return true;
}

void tty_release_ctty(uint32_t session_id) {
    if (!tty_ready) tty_init();
    if (tty_ctty_sid == session_id) {
        tty_ctty_sid = 0;
        tty_fg_pgrp = 0;
    }
}

bool tty_isig(void) {
    if (!tty_ready) tty_init();
    return (console_termios.c_lflag & TTY_ISIG) != 0;
}

int tty_cc_signal(char c) {
    if (!tty_ready) tty_init();
    if (!(console_termios.c_lflag & TTY_ISIG)) return 0;
    if (!c) return 0;
    uint8_t u = (uint8_t)c;
    if (u && u == console_termios.c_cc[TTY_VINTR]) return 2;
    if (u && u == console_termios.c_cc[TTY_VQUIT]) return 3;
    if (u && u == console_termios.c_cc[TTY_VSUSP]) return 20;
    return 0;
}