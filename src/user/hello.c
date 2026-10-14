static long sys_write(int fd, const void *buf, unsigned long len) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(2L), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_exit(int code) {
    __asm__ volatile(
        "syscall"
        :
        : "a"(16L), "D"((long)code)
        : "rcx", "r11", "memory"
    );
    for (;;) { }
}

static void putnum(long v) {
    char b[24];
    int i = 0;
    if (v == 0) b[i++] = '0';
    unsigned long u = (unsigned long)v;
    while (u) { b[i++] = (char)('0' + (u % 10)); u /= 10; }
    char o[24];
    for (int j = 0; j < i; j++) o[j] = b[i - 1 - j];
    sys_write(1, o, (unsigned)i);
}

static void puts_(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static const char probe[] = "MARKER";
static unsigned long data_probe = 0x1122334455667788UL;
static unsigned long bss_probe;

int main(int argc, char **argv) {
    if (data_probe != 0x1122334455667788UL) puts_("BUG: .data initialiser lost\n");
    else puts_(".data initialiser survived\n");
    bss_probe = 9;
    if (bss_probe != 9) puts_("BUG: bss not writable\n");
    if (probe[0] != 'M') puts_("BUG: rodata lost\n");
    (void)probe;

    puts_("argc=");
    putnum(argc);
    puts_("\n");
    for (int i = 0; i < argc; i++) {
        puts_("argv[");
        putnum(i);
        puts_("]=");
        puts_(argv[i]);
        puts_("\n");
    }

    sys_exit(0);

    return 0;
}
