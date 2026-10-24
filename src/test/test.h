#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint64_t vaddr_t;

bool mount_first_fat(int *out_drive);
void tty_inject(const char *s);
void tty_drain(void);

void fs_test(void);
void fd_test(void);
void mm_test(void);
void pipe_test(void);
void fswrite_test(void);
void fd2_test(void);
void futex_pair_test(void);
void sysdep_test(void);
void exectest_run(void);
void mandel_run(void);
void featuretest_run(void);
void proctest_run(void);
void clktest_run(void);
void forktest_run(void);
void shell_run(void);
void thread_test(void);
void mlibc_hello_test(void);
void tty_isolation_test(vaddr_t a_rip, vaddr_t b_rip, vaddr_t c_rip,
                        size_t code_bytes, size_t code_pages);
void wait_test(vaddr_t rip, size_t code_bytes, size_t code_pages);
bool run_c_program(void *node, uint64_t *entry_out, const char **argv, size_t argc);
void elf_bad_test(const char *name, const uint8_t *img, size_t len,
                  size_t file_size, int want, int *bad);
void elf_reject_test(int *bad);
void elf_run_test(void);

#endif