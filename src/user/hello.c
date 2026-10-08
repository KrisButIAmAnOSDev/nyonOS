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

static void puts_(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static const char probe[] = "MARKER";
static unsigned long data_probe = 0x1122334455667788UL;
static unsigned long bss_probe;

int main(void) {
    if (data_probe != 0x1122334455667788UL) puts_("BUG: .data initialiser lost\n");
    else puts_(".data initialiser survived\n");
    bss_probe = 9;
    if (bss_probe != 9) puts_("BUG: bss not writable\n");
    if (probe[0] != 'M') puts_("BUG: rodata lost\n");
    (void)probe;
    puts_("nyon nyon ulelele nyon uleleele lele from nyon\n");

    sys_exit(0);

    return 0;
}
