#include "sys.h"

extern long __libc_argc;
extern char **__libc_argv;


int main(void) {

    write_str(STDOUT, "hello from C, argc=");

    unsigned long argc = (unsigned long)__libc_argc;
    write_str(STDOUT, "\n");
    char digits[24];
    unsigned long n = 0;
    if (argc == 0) digits[n++] = '0';
    while (argc) {
        digits[n++] = (char)('0' + (argc % 10));
        argc /= 10;
    }
    char out[24];
    for (unsigned long i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    write(STDOUT, out, n);

    write_str(STDOUT, " argv0=");
    if (__libc_argv && __libc_argv[0]) {
        write_str(STDOUT, __libc_argv[0]);
    } else {
        write_str(STDOUT, "(none)");
    }
    write_str(STDOUT, "\n");

    return 0;
}
