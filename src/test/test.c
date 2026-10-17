#include "test.h"

#include "kernel/kprintf/kprintf.h"
#include "kernel/fd/fd.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/multitask/task.h"
#include "kernel/exec/elf.h"
#include "kernel/block/block.h"
#include "kernel/uaccess/uaccess.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/cpu/cpu.h"
#include "drivers/ata/ata.h"
#include "drivers/keyboard/keyboard.h"

extern uint8_t user_test_entry[];
extern uint8_t user_tty_a_entry[];
extern uint8_t user_tty_b_entry[];
extern uint8_t user_tty_c_entry[];
extern uint8_t user_wait_entry[];

bool mount_first_fat(int *out_drive) {
    static block_device_t whole;
    static mbr_entry_t ents[BLOCK_MAX_PARTITIONS];
    static partition_t part;

    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        ata_drive_t *d = ata_get_drive(i);
        if (!d || !d->present || d->atapi) continue;
        if (!block_bind_ata(&whole, i)) continue;

        int nparts = mbr_parse(&whole, ents, BLOCK_MAX_PARTITIONS);
        block_device_t *target = &whole;
        bool via_part = false;

        for (int p = 0; p < nparts; p++) {
            if (!mbr_is_fat(ents[p].type)) continue;
            if (!partition_init(&part, &whole, ents[p].start_lba, ents[p].sector_count)) continue;
            target = &part.dev;
            via_part = true;
            break;
        }

        if (!fs_mount(target)) continue;

        *out_drive = i;
        kprintf(PRINT_SERIAL, "  drive %d: %d mbr entries, mounted %s\n",
                i, nparts, via_part ? "FAT partition" : "whole device");
        return true;
    }

    return false;
}

static char txt_byte(uint32_t i) {
    static const char head[] = "that my jarona";
    uint32_t hl = sizeof(head) - 1;
    if (i < hl) return head[i];
    return '.';
}

void fs_test(void) {
    static char buf[4096] __attribute__((aligned(4)));
    static uint8_t bigbuf[4096] __attribute__((aligned(4)));
    static const char inner_expect[] = "inner payload\n";

    kprintf(PRINT_SERIAL, "FS test: probing disks for a FAT volume\n");

    int drv = -1;
    if (!mount_first_fat(&drv)) {
        kprintf(PRINT_SERIAL, "  no FAT volume found\n");
        return;
    }

    bool m = true;
    kprintf(PRINT_SERIAL, "  mounted: ");
    kprintchar(m ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    if (!m) return;

    fs_volume_t *f = fs_get();
    kprintf(PRINT_SERIAL, "  %s label '%s' bpbsig '%s'\n", fs_fat_name(f->cluster_count), f->label, f->fs_type);
    kprintf(PRINT_SERIAL, "  bytes/sector %u sectors/cluster %u fats %u root entries %u\n",
            f->bytes_per_sector, f->sectors_per_cluster, f->fat_count, f->root_entry_count);
    kprintf(PRINT_SERIAL, "  total sectors %u sectors/fat %u data_start %u clusters %u\n",
            f->total_sectors, f->sectors_per_fat, f->data_start, f->cluster_count);

    fs_node_t root;
    if (!fs_lookup("/", &root)) {
        kprintf(PRINT_SERIAL, "  root node: N\n");
        return;
    }

    uint32_t n = 0;
    fs_node_t e;
    while (n < 8 && fs_iterate(&root, n, &e)) {
        kprintf(PRINT_SERIAL, "  [%u] %s %s cluster %u size %u\n", n, e.name, e.is_dir ? "DIR" : "FILE", e.cluster, e.size);
        n++;
    }
    kprintf(PRINT_SERIAL, "  root entries listed: %u\n", n);

    bool have_test_txt = false, have_sub = false, have_bin = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!fs_iterate(&root, i, &e)) continue;
        if (fs_name_eq(e.name, "TEST.TXT")) have_test_txt = true;
        if (fs_name_eq(e.name, "SUB")) have_sub = true;
        if (fs_name_eq(e.name, "BIN")) have_bin = true;
    }
    bool fixtures = have_test_txt && have_sub && have_bin;
    kprintf(PRINT_SERIAL, "  test fixtures present: ");
    kprintchar(fixtures ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  (TEST.TXT %u SUB %u BIN %u)\n",
            have_test_txt, have_sub, have_bin);

    fs_node_t file;
    bool got = fs_lookup("/TEST.TXT", &file);
    kprintf(PRINT_SERIAL, "  lookup /TEST.TXT: ");
    kprintchar(got ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    if (!got) return;

    uint32_t read = fs_node_read_all(&file, bigbuf, sizeof(bigbuf));
    bool size_ok = read == file.size && read == 3000;
    bool content_ok = size_ok;
    for (uint32_t i = 0; content_ok && i < read; i++) {
        if (bigbuf[i] != (uint8_t)txt_byte(i)) content_ok = false;
    }
    kprintf(PRINT_SERIAL, "  size %u (dir says %u): ", read, file.size);
    kprintchar(size_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  content matches: ");
    kprintchar(content_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    uint32_t shown = read < 20 ? read : 20;
    kprintf(PRINT_SERIAL, "  contents: ");
    for (uint32_t i = 0; i < shown; i++) kprintchar((char)bigbuf[i], PRINT_SERIAL);
    if (read > shown) kprintf(PRINT_SERIAL, "... (%u bytes total)", read);
    kprintchar('\n', PRINT_SERIAL);

    uint32_t clus_size = (uint32_t)fs_get()->sectors_per_cluster * fs_get()->bytes_per_sector;
    char edge[16];
    bool edge_ok = fs_node_read(&file, clus_size - 4, edge, 16);
    for (int i = 0; edge_ok && i < 16; i++) {
        if ((uint8_t)edge[i] != (uint8_t)txt_byte(clus_size - 4 + (uint32_t)i)) edge_ok = false;
    }
    kprintf(PRINT_SERIAL, "  spans %u clusters, boundary read: ", (read + clus_size - 1) / clus_size);
    kprintchar(edge_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    char partial[8];
    bool p1 = fs_node_read(&file, 5, partial, 4);
    bool p2 = p1 && partial[0] == 'm' && partial[1] == 'y' && partial[2] == ' ' && partial[3] == 'j';
    bool oob = !fs_node_read(&file, file.size, partial, 1);
    bool missing = !fs_lookup("/NOPE.TXT", &e);
    kprintf(PRINT_SERIAL, "  offset read of 'my j': ");
    kprintchar(p2 ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  read past eof refused: ");
    kprintchar(oob ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  missing file refused: ");
    kprintchar(missing ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    fs_node_t sub;
    bool got_sub = fs_lookup("/SUB", &sub) && sub.is_dir;
    uint32_t sub_count = 0;
    fs_node_t it;
    while (got_sub && fs_iterate(&sub, sub_count, &it)) {
        kprintf(PRINT_SERIAL, "    SUB[%u] %s %s %u\n", sub_count, it.name, it.is_dir ? "DIR" : "FILE", it.size);
        sub_count++;
    }
    kprintf(PRINT_SERIAL, "  /SUB is a dir: ");
    kprintchar(got_sub ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  entries (dot skipped): %u\n", sub_count);

    fs_node_t inner;
    bool got_inner = fs_lookup("/SUB/INNER.TXT", &inner);
    uint32_t inner_read = got_inner ? fs_node_read_all(&inner, buf, sizeof(buf)) : 0;
    bool inner_ok = got_inner && inner_read == sizeof(inner_expect) - 1;
    for (uint32_t i = 0; inner_ok && i < inner_read; i++) {
        if (buf[i] != inner_expect[i]) inner_ok = false;
    }
    kprintf(PRINT_SERIAL, "  /SUB/INNER.TXT content: ");
    kprintchar(inner_ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    if (inner_read && inner_read < sizeof(buf)) {
        buf[inner_read] = 0;
        kprintf(PRINT_SERIAL, "  contents: %s\n", buf);
    }

    bool bad_path = !fs_lookup("/SUB/NOPE.TXT", &it) && !fs_lookup("/TESTTX.TXT", &it);
    bool dir_read_refused = !fs_node_read(&sub, 0, buf, 16);
    kprintf(PRINT_SERIAL, "  bad paths refused: ");
    kprintchar(bad_path ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  read on a directory refused: ");
    kprintchar(dir_read_refused ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    kprintf(PRINT_SERIAL, "FS test: usable: ");
    kprintchar((fixtures && size_ok && content_ok && edge_ok && p2 && oob && missing &&
                got_sub && sub_count == 1 && inner_ok && bad_path && dir_read_refused) ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

#define TTY_RES_DONE  16
#define TTY_RES_READY 17
#define TTY_RES_GO    18
#define TTY_RES_ARG   19
#define TTY_RES_GOT   32
#define TTY_UNSET     0xdeadbeefdeadbeefULL

static volatile uint64_t *tty_peer(const char *name, vaddr_t rip, size_t code_bytes, size_t code_pages,
                                   uint64_t arg, uint32_t *pid_out, paddr_t *page_out) {
    paddr_t code_phys, results_phys;
    if (!pmm_alloc(&code_phys, code_pages)) return NULL;
    if (!pmm_alloc(&results_phys, 1)) { pmm_free(code_phys, code_pages); return NULL; }

    volatile uint8_t *dst = (volatile uint8_t *)(vmm_hhdm_offset + code_phys);
    for (size_t i = 0; i < code_bytes; i++) dst[i] = user_test_entry[i];

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + results_phys);
    for (int i = 0; i < 64; i++) res[i] = TTY_UNSET;
    res[TTY_RES_GO] = 0;
    res[TTY_RES_ARG] = arg;

    struct task *t = task_spawn_ring3_lent(name, code_phys, code_pages, TASK_USER_CODE_VIRT, rip, results_phys, 0x60000000ULL);
    if (!t) {
        pmm_free(code_phys, code_pages);
        pmm_free(results_phys, 1);
        return NULL;
    }
    *pid_out = t->pid;
    *page_out = results_phys;
    return res;
}

void tty_inject(const char *s) {
    while (*s) keyboard_inject_char(*s++);
}

void tty_drain(void) {
    char c;
    while (keyboard_try_pop(&c)) { }
    keyboard_set_owner(0);
}

static const char *const c_stdin_lines[] = {
    "",
    "nyon from stdin\n",
    "second line\n",
    "abcdefghij\n",
    "xy\n",
    "back\bspace\n"
};
#define C_STDIN_LINES ((int)(sizeof(c_stdin_lines) / sizeof(c_stdin_lines[0])))

static const char *const c_fd_labels[C_CHECKS] = {
    "open /TEST.TXT returns an fd",
    "read 14 bytes from a C program",
    "read content matches",
    "pread 14 bytes at offset 0",
    "pread content matches",
    "fstat succeeds",
    "fstat reports size 3000",
    "stat by path succeeds",
    "stat reports size 3000",
    "isatty on a file fd is 0",
    "isatty on fd 1 is 1",
    "open a directory with O_DIRECTORY",
    "getdents returns one entry",
    "getdents entry size is 14",
    "getdents past the end returns 0",
    "getpid is nonzero",
    "sleep returns 0",
    "debug_print refused without the gate",
    "dup2 onto fd 5",
    "write through the dup2 fd",
    "isatty on the dup2 fd",
    "dup2 from a bad fd refused",
    "dup2 onto itself",
    "F_GETFD on the dup2 fd",
    "F_SETFD clears close-on-exec",
    "ioctl reads the stream destination",
    "ioctl changes the destination",
    "ioctl restores the destination",
    "read on stdout refused",
    "F_GETFL on stdout",
    "F_DUPFD returns an fd >= 20",
    "close the F_DUPFD result",
    "ioctl on a bad fd refused",
    "read on stderr refused",
    "write on stdin refused",
    "isatty on stderr is 1",
    "dup2 past the inline table grows it",
    "write through the grown-table dup2",
    "close the grown-table dup2",
    "lseek SET 0 resets position",
    "re-read after lseek returns 4",
    "lseek past end clamps to file size",
    "read at EOF returns 0",
    "dup with narrowed rights",
    "write through a read-only fd refused",
    "close the dup returns 0",
    "close the file fd returns 0",
    "stale fd after close refused",
    "double close refused",
    "open a missing file returns ENOENT",
    "open a directory without O_DIRECTORY is EISDIR",
    "kernel pointer to read rejected",
    "write to stdout via fd 1",
    "getpid agrees between syscall and int 0x80",
    "int 0x80 fallback still writes",
    "syscall keeps rsp and all regs across sleep+stat",
    "int 0x80 keeps rsp and all regs across sleep+stat",
    "unknown syscall number is ENOSYS",
    "rcx changes across syscall (return address)",
    "r11 bit 9 set across syscall (flags)",
    "bss starts zeroed at the front",
    "bss starts zeroed at the end",
    "data initialiser survived loading",
    "rodata content survived loading",
    "bss is writable",
    "isatty on stdin is 1",
    "F_SETFL O_NONBLOCK on stdin accepted",
    "F_GETFL reports O_NONBLOCK on stdin",
    "non-blocking read of an empty tty is EAGAIN",
    "F_SETFL clears O_NONBLOCK on stdin",
    "F_GETFL no longer reports O_NONBLOCK",
    "stdin claims the tty and is the foreground",
    "blocking read returns the whole injected line",
    "injected line content is nyon from stdin",
    "short line read returns its length",
    "short line content is xy",
    "read of 4 takes only 4 bytes",
    "partial read content is abcd",
    "rest of the line comes on the next read",
    "remainder content is efghij",
    "backspace-edited line length",
    "backspace-edited line content is baspace",
    "stdout default destination is PRINT_BOTH",
    "stderr default destination is PRINT_SERIAL",
    "stderr destination can be changed",
    "changing stderr leaves stdout alone",
    "write to stderr returns the byte count",
    "dup2 of stdout returns the fd",
    "write through the stdout dup",
    "the stdout dup shares the destination",
    "close the stdout dup",
    "dup2 of stderr returns the fd",
    "the stderr dup has its own destination",
    "write through the stderr dup",
    "close the stderr dup",
    "setting the destination on stdin refused",
    "write on stdin refused",
    "null buffer on stdin is EFAULT",
    "kernel pointer to stderr is EFAULT"
};

void fd_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/FDTEST", &node) && !fs_lookup("/BIN/fdtest", &node)) {
        kprintf(PRINT_SERIAL, "FD ring3: /BIN/fdtest not found\n");
        return;
    }

    paddr_t phys;
    if (!pmm_alloc(&phys, 1)) { kprintf(PRINT_SERIAL, "FD ring3: alloc failed\n"); return; }

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + phys);
    for (int i = 0; i < C_RES_SLOTS; i++) res[i] = 0xdeadbeefdeadbeefULL;

    struct elf_shared shared = { .phys = phys, .virt = C_RES_VIRT, .pages = 1, .owned_by_loader = false };

    struct elf_image img;
    const char *err = NULL;
    int r = elf_load_shared(&node, &img, &err, NULL, 0, &shared);
    if (r) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "FD ring3: load refused: %s\n", err ? err : "?");
        return;
    }

    size_t before = pmm_free_pages();

    struct task *t = task_spawn_vmspace("ring3c", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "FD ring3: spawn failed\n");
        return;
    }

    uint32_t pid_served = t->pid;
    tty_drain();
    for (int i = 0; i < 2000 && res[C_RES_DONE] == 0xdeadbeefdeadbeefULL && task_find_pid(pid_served); i++) {
        uint64_t req = res[C_RES_STDIN_REQ];
        uint64_t ack = res[C_RES_STDIN_ACK];
        if (req != ack) {
            uint64_t line = req < C_STDIN_LINES ? req : 0;
            res[C_RES_STDIN_ACK] = req;
            tty_inject(c_stdin_lines[line]);
        }
        pit_sleep(5);
    }

    bool ran = res[C_RES_DONE] != 0xdeadbeefdeadbeefULL;
    uint32_t pid = t->pid;
    for (int i = 0; i < 400 && task_find_pid(pid); i++) pit_sleep(5);
    task_reap_now();

    if (!ran) {
        kprintf(PRINT_SERIAL, "FD ring3: the c test never reported\n");
        pmm_free(phys, 1);
        return;
    }

    int bad = 0;
    for (int i = 0; i < C_CHECKS; i++) {
        if (res[C_RES_CHECK_BASE + i] == 1) continue;
        bad++;
        kprintf(PRINT_SERIAL, "    FAIL %s: got %lld, wanted %lld\n",
                c_fd_labels[i], (long long)res[C_RES_FAIL_BASE + i],
                (long long)res[C_RES_GOT_BASE + i]);
    }

    size_t after = pmm_free_pages();

    kprintf(PRINT_SERIAL, "FD ring3: %d of %d checks passed: ", C_CHECKS - bad, C_CHECKS);
    kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "FD ring3: loaded by the elf loader as pie, no leak: ");
    kprintchar(after >= before ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  free %zu -> %zu\n", before, after);

    pmm_free(phys, 1);
}

void mm_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/MMTEST", &node) && !fs_lookup("/BIN/mmtest", &node)) {
        kprintf(PRINT_SERIAL, "MM ring3: /BIN/mmtest not found\n");
        return;
    }

    paddr_t phys;
    if (!pmm_alloc(&phys, 1)) { kprintf(PRINT_SERIAL, "MM ring3: alloc failed\n"); return; }

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + phys);
    for (int i = 0; i < C_RES_SLOTS; i++) res[i] = 0xdeadbeefdeadbeefULL;

    struct elf_shared shared = { .phys = phys, .virt = C_RES_VIRT, .pages = 1, .owned_by_loader = false };

    struct elf_image img;
    const char *err = NULL;
    int r = elf_load_shared(&node, &img, &err, NULL, 0, &shared);
    if (r) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "MM ring3: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("ring3mm", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "MM ring3: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 600 && res[C_RES_DONE] == 0xdeadbeefdeadbeefULL && task_find_pid(pid); i++) pit_sleep(5);

    bool ran = res[C_RES_DONE] != 0xdeadbeefdeadbeefULL;
    for (int i = 0; i < 400 && task_find_pid(pid); i++) pit_sleep(5);
    task_reap_now();

    if (!ran) {
        kprintf(PRINT_SERIAL, "MM ring3: the test never reported\n");
        pmm_free(phys, 1);
        return;
    }

    int checks_run = 0, bad = 0;
    for (int i = 0; i < C_MAX_CHECKS; i++) {
        if (res[C_RES_CHECK_BASE + i] == 0xdeadbeefdeadbeefULL) break;
        checks_run++;
        if (res[C_RES_CHECK_BASE + i] == 1) continue;
        bad++;
        kprintf(PRINT_SERIAL, "    FAIL mm check %d: got %lld, wanted %lld\n", i,
                (long long)res[C_RES_FAIL_BASE + i], (long long)res[C_RES_GOT_BASE + i]);
    }

    kprintf(PRINT_SERIAL, "MM ring3: %d of %d checks passed: ", checks_run - bad, checks_run);
    kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    pmm_free(phys, 1);
}

void futex_pair_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/MMTEST", &node) && !fs_lookup("/BIN/mmtest", &node)) {
        kprintf(PRINT_SERIAL, "FUTEX ring3: /BIN/mmtest not found\n");
        return;
    }

    paddr_t phys;
    if (!pmm_alloc(&phys, 1)) { kprintf(PRINT_SERIAL, "FUTEX ring3: alloc failed\n"); return; }

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + phys);
    for (int i = 0; i < C_RES_SLOTS; i++) res[i] = 0xdeadbeefdeadbeefULL;

    struct elf_shared shared = { .phys = phys, .virt = C_RES_VIRT, .pages = 1, .owned_by_loader = false };

    static const char *wait_argv[] = { "mmtest", "waiter" };
    static const char *wake_argv[] = { "mmtest", "waker" };

    struct elf_image img;
    const char *err = NULL;
    if (elf_load_shared(&node, &img, &err, wait_argv, 2, &shared)) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "FUTEX ring3: load refused: %s\n", err ? err : "?");
        return;
    }

    struct vmspace *vs = img.vs;
    struct task *t1 = task_spawn_vmspace("ring3wt", vs, img.entry, img.stack_top);
    elf_destroy(&img);

    pit_sleep(60);

    struct elf_image img2;
    if (elf_load_shared(&node, &img2, &err, wake_argv, 2, &shared)) {
        if (t1) { for (int i = 0; i < 400 && task_find_pid(t1->pid); i++) pit_sleep(5); }
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "FUTEX ring3: second load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t2 = task_spawn_vmspace("ring3wk", img2.vs, img2.entry, img2.stack_top);
    elf_destroy(&img2);

    if (!t1 || !t2) {
        kprintf(PRINT_SERIAL, "FUTEX ring3: spawn failed\n");
        pmm_free(phys, 1);
        return;
    }

    for (int i = 0; i < 900; i++) {
        pit_sleep(5);
        if (res[466] != 0xdeadbeefdeadbeefULL && res[468] != 0xdeadbeefdeadbeefULL) break;
    }
    task_reap_now();

    uint64_t waiter = res[466], waker = res[468];
    bool ok = (waiter == 100) && (waker == 100);
    kprintf(PRINT_SERIAL, "FUTEX ring3: waiter blocked, woken by second task: ");
    kprintchar(ok ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    if (!ok)
        kprintf(PRINT_SERIAL, "    FAIL waiter=%lld waker=%lld\n", (long long)waiter, (long long)waker);

    pmm_free(phys, 1);
}

void mlibc_hello_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/MHELLO", &node) && !fs_lookup("/BIN/mhello", &node)) {
        kprintf(PRINT_SERIAL, "MLIBC: /BIN/mhello not found\n");
        return;
    }

    size_t before = pmm_free_pages();

    struct elf_image img;
    const char *err = NULL;
    static const char *args[2] = { "/BIN/mhello", "kawkaw" };
    if (elf_load_argv(&node, &img, &err, args, 2)) {
        kprintf(PRINT_SERIAL, "MLIBC: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("mlibc", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "MLIBC: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 4000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    size_t after = pmm_free_pages();

    kprintf(PRINT_SERIAL, "MLIBC: libc program ran and exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "MLIBC: no leaked frames: ");
    kprintchar(after >= before ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  free %zu -> %zu\n", before, after);
}


void mandel_run(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/MANDEL", &node) && !fs_lookup("/BIN/mandel", &node)) {
        kprintf(PRINT_SERIAL, "MAND: /BIN/mandel not found\n");
        return;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load(&node, &img, &err)) {
        kprintf(PRINT_SERIAL, "MAND: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("mandel", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "MAND: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 8000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    kprintf(PRINT_SERIAL, "MAND: program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

void featuretest_run(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/FEATEST", &node) && !fs_lookup("/BIN/featest", &node)) {
        kprintf(PRINT_SERIAL, "FEAT: /BIN/featuretest not found\n");
        return;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load(&node, &img, &err)) {
        kprintf(PRINT_SERIAL, "FEAT: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("feat", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "FEAT: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 6000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    kprintf(PRINT_SERIAL, "FEAT: program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

void proctest_run(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/PROCTEST", &node) && !fs_lookup("/BIN/proctest", &node)) {
        kprintf(PRINT_SERIAL, "PROC: /BIN/proctest not found\n");
        return;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load(&node, &img, &err)) {
        kprintf(PRINT_SERIAL, "PROC: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("proc", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "PROC: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 6000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    kprintf(PRINT_SERIAL, "PROC: program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

void clktest_run(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/CLKTEST", &node) && !fs_lookup("/BIN/clktest", &node)) {
        kprintf(PRINT_SERIAL, "CLK: /BIN/clktest not found\n");
        return;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load(&node, &img, &err)) {
        kprintf(PRINT_SERIAL, "CLK: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("clk", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "CLK: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 4000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    kprintf(PRINT_SERIAL, "CLK: program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

void forktest_run(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/FORKTEST", &node) && !fs_lookup("/BIN/forktest", &node)) {
        kprintf(PRINT_SERIAL, "FORK ring3: /BIN/forktest not found\n");
        return;
    }

    paddr_t phys;
    if (!pmm_alloc(&phys, 1)) {
        kprintf(PRINT_SERIAL, "FORK ring3: alloc failed\n");
        return;
    }

    volatile uint64_t *res = (volatile uint64_t *)(vmm_hhdm_offset + phys);
    for (int i = 0; i < C_RES_SLOTS; i++) res[i] = 0xdeadbeefdeadbeefULL;

    size_t before = pmm_free_pages();

    struct elf_shared shared = { .phys = phys, .virt = C_RES_VIRT, .pages = 1, .owned_by_loader = false };

    struct elf_image img;
    const char *err = NULL;
    if (elf_load_shared(&node, &img, &err, NULL, 0, &shared)) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "FORK ring3: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("fork", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        pmm_free(phys, 1);
        kprintf(PRINT_SERIAL, "FORK ring3: spawn failed\n");
        return;
    }

    bool ran = false;
    uint32_t pid = t->pid;
    for (int i = 0; i < 2000 && task_find_pid(pid); i++) {
        if (res[C_RES_DONE] != 0xdeadbeefdeadbeefULL) { ran = true; break; }
        pit_sleep(5);
    }
    if (res[C_RES_DONE] != 0xdeadbeefdeadbeefULL) ran = true;
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    if (!ran) {
        kprintf(PRINT_SERIAL, "FORK ring3: the test never reported\n");
        pmm_free(phys, 1);
        return;
    }

    static const char *labels[C_MAX_CHECKS] = {
        "fork returns child pid", "fork pid_out matches", "fork child pid differs",
        "fork parent marker untouched", "fork waitpid returns child",
        "fork waitpid status carries code", "fork marker still untouched after wait",
        "clone(0) returns child pid", "clone(0) pid_out matches",
        "clone(0) parent marker untouched", "clone(0) waitpid(-1) returns child",
        "clone(0) waitpid status carries code",
        "vfork returns child pid", "vfork pid_out matches",
        "vfork child write is visible to parent",
        "vfork waitpid returns child", "vfork status is zero",
        "clone(CLONE_VM) returns child pid", "clone(CLONE_VM) pid_out matches",
        "clone(CLONE_VM) child write is visible to parent",
        "clone(CLONE_VM) waitpid returns child", "clone(CLONE_VM) status carries code",
        "waitpid(-1,WNOHANG) with no children is ECHILD",
        "waitpid(unknown pid,WNOHANG) is ECHILD",
        "waitpid(pid < -1) is EINVAL",
        "fork(NULL) is EFAULT", "vfork(NULL) is EFAULT", "getpid still positive",
    };

    int checks = 0;
    for (int i = 0; i < C_MAX_CHECKS; i++) {
        if (res[C_RES_CHECK_BASE + i] == 1) { checks++; continue; }
        if (res[C_RES_CHECK_BASE + i] == 0xdeadbeefdeadbeefULL) continue;
        kprintf(PRINT_SERIAL, "    FAIL %s: got %lld, wanted %lld\n",
                labels[i] ? labels[i] : "?",
                (long long)res[C_RES_FAIL_BASE + i],
                (long long)res[C_RES_GOT_BASE + i]);
    }

    int bad = 0;
    for (int i = 0; i < C_MAX_CHECKS; i++)
        if (res[C_RES_CHECK_BASE + i] == 0) bad++;

    kprintf(PRINT_SERIAL, "FORK ring3: %d of %d checks passed: ", checks, checks + bad);
    kprintchar(bad == 0 && checks > 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "FORK ring3: test program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    size_t after = pmm_free_pages();
    kprintf(PRINT_SERIAL, "FORK ring3: every child and its stack reclaimed, no leak: ");
    kprintchar(after == before ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  free %zu -> %zu\n", before, after);

    pmm_free(phys, 1);
}

void sysdep_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/SYSDEPTEST", &node) && !fs_lookup("/BIN/sdtest", &node)) {
        kprintf(PRINT_SERIAL, "SYSD: /BIN/sdtest not found\n");
        return;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load(&node, &img, &err)) {
        kprintf(PRINT_SERIAL, "SYSD: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("sysdep", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "SYSD: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 4000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    kprintf(PRINT_SERIAL, "SYSD: program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

void thread_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/THRTEST", &node) && !fs_lookup("/BIN/thrtest", &node)) {
        kprintf(PRINT_SERIAL, "THREADS: /BIN/thrtest not found\n");
        return;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load(&node, &img, &err)) {
        kprintf(PRINT_SERIAL, "THREADS: load refused: %s\n", err ? err : "?");
        return;
    }

    struct task *t = task_spawn_vmspace("threads", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) {
        kprintf(PRINT_SERIAL, "THREADS: spawn failed\n");
        return;
    }

    uint32_t pid = t->pid;
    for (int i = 0; i < 8000 && task_find_pid(pid); i++) pit_sleep(5);
    bool alive = task_find_pid(pid) != NULL;
    task_reap_now();

    kprintf(PRINT_SERIAL, "THREADS: program exited: ");
    kprintchar(!alive ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

static bool tty_reap(uint32_t pid) {
    for (int i = 0; i < 200 && task_find_pid(pid); i++) pit_sleep(5);
    return task_find_pid(pid) == NULL;
}

static bool tty_wait(volatile uint64_t *res, int slot) {
    for (int i = 0; i < 400 && res[slot] == TTY_UNSET && res[TTY_RES_DONE] == TTY_UNSET; i++) pit_sleep(10);
    return res[slot] != TTY_UNSET;
}

static int tty_score(const char *who, volatile uint64_t *res, const char *const *what, int n) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (res[i] == 1) continue;
        bad++;
        if (res[TTY_RES_GOT + i] != TTY_UNSET)
            kprintf(PRINT_SERIAL, "    FAIL %s: %s, got %lld\n", who, what[i], (long long)res[TTY_RES_GOT + i]);
        else
            kprintf(PRINT_SERIAL, "    FAIL %s: %s, never ran\n", who, what[i]);
    }
    return bad;
}

void tty_isolation_test(vaddr_t a_rip, vaddr_t b_rip, vaddr_t c_rip, size_t code_bytes, size_t code_pages) {
    static const char *const what_a[7] = {
        "first read of a free tty claims it",
        "owner sees itself as foreground",
        "owner can release the tty",
        "released tty has no foreground",
        "free tty can be taken back",
        "handing the tty to a dead pid is refused",
        "foreground is visible through stdout too"
    };
    static const char *const what_b[7] = {
        "background read gets EIO",
        "background sees the real owner",
        "background cannot steal the tty",
        "background cannot release the tty",
        "owner unchanged after the attempts",
        "background can still write",
        "second background read still EIO"
    };
    static const char *const what_c[5] = {
        "owner exit released the tty",
        "new reader claims the free tty",
        "new reader is foreground",
        "handing the tty to the exited owner refused",
        "foreground unchanged after refusal"
    };

    uint32_t apid = 0, bpid = 0, cpid = 0;
    paddr_t apage = 0, bpage = 0, cpage = 0;
    int bad = 0, total = 20;

    volatile uint64_t *ra = tty_peer("ttyowner", a_rip, code_bytes, code_pages, 0, &apid, &apage);
    if (!ra) { kprintf(PRINT_SERIAL, "TTY ring3: owner spawn failed\n"); return; }

    volatile uint64_t *rb = NULL, *rc = NULL;
    if (tty_wait(ra, TTY_RES_READY) && ra[TTY_RES_READY] == apid) {
        rb = tty_peer("ttybg", b_rip, code_bytes, code_pages, apid, &bpid, &bpage);
        if (rb) tty_wait(rb, TTY_RES_DONE);
    }

    ra[TTY_RES_GO] = 1;
    tty_wait(ra, TTY_RES_DONE);
    bool a_gone = tty_reap(apid);

    if (rb && a_gone) {
        rc = tty_peer("ttynext", c_rip, code_bytes, code_pages, apid, &cpid, &cpage);
        if (rc) tty_wait(rc, TTY_RES_DONE);
    }

    bool all_gone = a_gone && (!rb || tty_reap(bpid)) && (!rc || tty_reap(cpid));

    bad += tty_score("owner", ra, what_a, 7);
    if (rb) bad += tty_score("background", rb, what_b, 7);
    else { bad += 7; kprintf(PRINT_SERIAL, "    FAIL background never ran\n"); }
    if (rc) bad += tty_score("next", rc, what_c, 5);
    else { bad += 5; kprintf(PRINT_SERIAL, "    FAIL next never ran\n"); }

    if (keyboard_owner() != 0) { bad++; kprintf(PRINT_SERIAL, "    FAIL tty still owned by %u after all exited\n", keyboard_owner()); }

    if (all_gone) {
        pmm_free(apage, 1);
        if (rb) pmm_free(bpage, 1);
        if (rc) pmm_free(cpage, 1);
    }

    kprintf(PRINT_SERIAL, "TTY ring3: %d of %d isolation checks passed (3 processes): ", total - bad, total);
    kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

static bool tty_asleep_on(uint32_t pid, void *chan, bool want_deadline) {
    struct task *t = task_find_pid(pid);
    if (!t || !t->blocked) return false;
    if (t->wait_chan != chan) return false;
    return want_deadline ? t->wake_tick != 0 : t->wake_tick == 0;
}

void wait_test(vaddr_t rip, size_t code_bytes, size_t code_pages) {
    static const char *const what[13] = {
        "stdin is blocking by default",
        "F_SETFL O_NONBLOCK accepted",
        "F_GETFL reports O_NONBLOCK",
        "F_SETFL leaves close-on-exec alone",
        "non-blocking read of an empty tty is EAGAIN",
        "blocking read returns the typed line",
        "line content is hey\\n",
        "half a line is not returned yet",
        "backspace edited line comes back as ac\\n",
        "edited line content",
        "short read takes only what was asked",
        "rest of the line comes on the next read",
        "sleep returns 0"
    };
    static const char *const seen[5] = {
        "reader was asleep on the tty, not spinning",
        "reader stayed asleep on half a line",
        "sleeper was asleep with a deadline",
        "sleep did not return early",
        "tty released after the reader exited"
    };

    uint32_t pid = 0;
    paddr_t page = 0;
    tty_drain();
    volatile uint64_t *r = tty_peer("ttywait", rip, code_bytes, code_pages, 0, &pid, &page);
    if (!r) { kprintf(PRINT_SERIAL, "WAIT ring3: spawn failed\n"); return; }

    void *chan = keyboard_wait_chan();
    bool ok[5] = { false, false, false, false, false };

    if (tty_wait(r, TTY_RES_READY)) {
        pit_sleep(100);
        ok[0] = tty_asleep_on(pid, chan, false);
        tty_inject("hey\n");
    }

    if (tty_wait(r, 20)) {
        tty_inject("ab");
        r[23] = 1;
    }

    if (tty_wait(r, 21)) {
        pit_sleep(100);
        ok[1] = tty_asleep_on(pid, chan, false);
        tty_inject("\bc\nwxyz\n");
    }

    uint64_t t0 = 0;
    for (int i = 0; i < 4000 && r[22] == TTY_UNSET && r[TTY_RES_DONE] == TTY_UNSET; i++) pit_sleep(1);
    if (r[22] != TTY_UNSET) {
        t0 = pit_get_ticks();
        pit_sleep(50);
        ok[2] = tty_asleep_on(pid, NULL, true);
    }

    for (int i = 0; i < 4000 && r[TTY_RES_DONE] == TTY_UNSET; i++) pit_sleep(1);
    uint64_t t1 = pit_get_ticks();
    ok[3] = t0 && r[TTY_RES_DONE] == 1 && t1 - t0 + TASK_QUANTUM_TICKS >= pit_ms_to_ticks(150);

    bool gone = tty_reap(pid);
    ok[4] = gone && keyboard_owner() == 0;

    int bad = tty_score("reader", r, what, 13);
    for (int i = 0; i < 5; i++) {
        if (ok[i]) continue;
        bad++;
        kprintf(PRINT_SERIAL, "    FAIL kernel saw: %s\n", seen[i]);
    }
    if (gone) pmm_free(page, 1);

    kprintf(PRINT_SERIAL, "WAIT ring3: %d of 18 blocking checks passed: ", 18 - bad);
    kprintchar(bad == 0 ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);
}

void elf_bad_test(const char *name, const uint8_t *img, size_t len, size_t file_size, int want, int *bad) {
    uint8_t buf[512];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = img[i < len ? i : len - 1];

    size_t phoff = 0, phentsize = 0, n = 0;
    bool pie = false;
    uint64_t entry = 0;
    int r = elf_validate_hdr(buf, len, file_size, &phoff, &phentsize, &n, &pie, &entry);
    if (r) {
        if (r != want) {
            (*bad)++;
            kprintf(PRINT_SERIAL, "    FAIL %s: got %d (%s), wanted %d\n", name, r, elf_error_text(r), want);
        }
        return;
    }

    elf64_phdr ph[ELF_MAX_PHNUM];
    for (size_t i = 0; i < n && phoff + i * sizeof(elf64_phdr) + sizeof(elf64_phdr) <= len; i++) {
        const uint8_t *base = buf + phoff + i * sizeof(elf64_phdr);
        ph[i].type  = (uint32_t)base[0] | ((uint32_t)base[1] << 8);
        ph[i].flags = (uint32_t)base[4] | ((uint32_t)base[5] << 8);
        uint64_t o = 0, v = 0, pp = 0, f = 0, m = 0, a = 0;
        for (size_t k = 0; k < 8; k++) {
            o  |= (uint64_t)base[8 + k]  << (8 * k);
            v  |= (uint64_t)base[16 + k] << (8 * k);
            pp |= (uint64_t)base[24 + k] << (8 * k);
            f  |= (uint64_t)base[32 + k] << (8 * k);
            m  |= (uint64_t)base[40 + k] << (8 * k);
            a  |= (uint64_t)base[48 + k] << (8 * k);
        }
        ph[i].offset = o; ph[i].vaddr = v; ph[i].paddr = pp;
        ph[i].filesz = f; ph[i].memsz = m; ph[i].align = a;
    }

    bool ok = false;
    r = elf_validate_phdrs(ph, n, file_size, pie, entry, &ok);
    if (r == 0 && !ok) r = ELF_EBADMAGIC;

    if (r != want) {
        (*bad)++;
        kprintf(PRINT_SERIAL, "    FAIL %s: got %d (%s), wanted %d\n", name, r, elf_error_text(r), want);
    }
}

void elf_reject_test(int *bad) {
    static uint8_t good[512];
    size_t before = pmm_free_pages();

    static uint8_t real[1024];
    fs_node_t node;
    size_t fsize = 512;
    if (fs_lookup("/BIN/HELLO", &node) || fs_lookup("/BIN/hello", &node)) {
        fs_node_read(&node, 0, real, sizeof(real));
        fsize = node.size;
    }

    for (size_t i = 0; i < sizeof(good); i++) good[i] = real[i];

    elf_bad_test("a valid binary is accepted", good, 512, fsize, 0, bad);

    uint8_t t[512];
    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];

    t[1] = 'X';
    elf_bad_test("bad magic rejected", t, 512, fsize, ELF_EBADMAGIC, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[4] = 1;
    elf_bad_test("32-bit class rejected", t, 512, fsize, ELF_EBADCLASS, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[5] = 2;
    elf_bad_test("big endian rejected", t, 512, fsize, ELF_EBADDATA, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[18] = 3; t[19] = 0;
    elf_bad_test("wrong machine rejected", t, 512, fsize, ELF_EBADMACHINE, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[16] = 1;
    elf_bad_test("relocatable object rejected", t, 512, fsize, ELF_EBADTYPE, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[56] = 0; t[57] = 0;
    elf_bad_test("zero program headers rejected", t, 512, fsize, ELF_EPHNUM, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[56] = 200; t[57] = 0;
    elf_bad_test("absurd program header count rejected", t, 512, fsize, ELF_EPHNUM, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    for (int k = 0; k < 8; k++) t[32 + k] = (uint8_t)((fsize >> (8 * k)) & 0xff);
    elf_bad_test("program headers past end of file rejected", t, 512, fsize, ELF_EOFFSET, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    for (int k = 0; k < 8; k++) { t[64 + 32 + k] = 0xf0; t[64 + 40 + k] = 0xf0; }
    t[64 + 32] = 0xf0; t[64 + 40] = 0xf8;
    elf_bad_test("filesz past end of file rejected", t, 512, fsize, ELF_EOFFSET, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    for (int k = 0; k < 8; k++) t[64 + 40 + k] = 0;
    t[64 + 40] = 1;
    elf_bad_test("memsz smaller than filesz rejected", t, 512, fsize, ELF_EMEMSZ, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    for (int k = 0; k < 8; k++) { t[64 + 16 + k] = 0; t[64 + 24 + k] = 0; t[64 + 32 + k] = 0; t[64 + 40 + k] = 0; }
    t[64 + 23] = 0x80;
    t[64 + 31] = 0x80;
    t[64 + 32] = 0x00; t[64 + 40] = 0x20;
    t[27] = 0x00; t[28] = 0x80;
    elf_bad_test("kernel-half address rejected", t, 512, fsize, ELF_EADDR, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[64 + 4] = ELF_PF_W | ELF_PF_X;
    elf_bad_test("writable and executable segment rejected", t, 512, fsize, ELF_EWX, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[64] = ELF_PT_INTERP;
    elf_bad_test("dynamic binary rejected for now", t, 512, fsize, ELF_EINTERP, bad);

    for (size_t i = 0; i < sizeof(t); i++) t[i] = good[i];
    t[24] = 0x11; t[25] = 0x22; t[26] = 0x33; t[27] = 0x44;
    t[28] = 0; t[29] = 0; t[30] = 0; t[31] = 0;
    elf_bad_test("entry point outside every segment rejected", t, 512, fsize, ELF_EENTRY, bad);

    size_t after = pmm_free_pages();
    if (after < before) {
        (*bad)++;
        kprintf(PRINT_SERIAL, "    FAIL validator leaked pages: %zu -> %zu\n", before, after);
    }
}

bool run_c_program(void *node, uint64_t *entry_out, const char **argv, size_t argc) {
    struct elf_image img;
    const char *err = NULL;
    int r = elf_load_argv(node, &img, &err, argv, argc);
    if (r) {
        kprintf(PRINT_SERIAL, "ELF: refused: %s (%d)\n", err ? err : "?", r);
        return false;
    }

    if (entry_out) *entry_out = img.entry;

    struct task *t = task_spawn_vmspace("cprog", img.vs, img.entry, img.stack_top);
    elf_destroy(&img);
    if (!t) return false;

    for (int i = 0; i < 600 && task_find_pid(t->pid); i++) pit_sleep(10);
    task_reap_now();

    return task_find_pid(t->pid) == NULL;
}

void elf_run_test(void) {
    fs_node_t node;
    if (!fs_lookup("/BIN/HELLO", &node) && !fs_lookup("/BIN/hello", &node)) {
        kprintf(PRINT_SERIAL, "ELF: /BIN/hello not found\n");
        return;
    }

    size_t before = pmm_free_pages();

    uint64_t e1 = 0, e2 = 0;
    bool ran1 = run_c_program(&node, &e1, NULL, 0);
    bool ran2 = run_c_program(&node, &e2, NULL, 0);
    bool moved = ran1 && ran2 && e1 != e2;

    kprintf(PRINT_SERIAL, "ELF: aslr: two loads at 0x%llx and 0x%llx, differ: ",
            (unsigned long long)e1, (unsigned long long)e2);
    kprintchar(moved ? 'Y' : 'N', PRINT_SERIAL);
    kprintchar('\n', PRINT_SERIAL);

    {
        static const char *args[3] = { "/BIN/hello", "hello", "from nyonOS" };
        bool ok = run_c_program(&node, &e2, args, 3);
        kprintf(PRINT_SERIAL, "ELF: argv passed to a c program: ");
        kprintchar(ok ? 'Y' : 'N', PRINT_SERIAL);
        kprintchar('\n', PRINT_SERIAL);
    }

    for (int i = 0; i < 600; i++) {
        task_reap_now();
        if (pmm_free_pages() >= before) break;
        pit_sleep(10);
    }

    size_t after = pmm_free_pages();
    kprintf(PRINT_SERIAL, "ELF: c program ran and exited with no leak: ");
    kprintchar(after >= before ? 'Y' : 'N', PRINT_SERIAL);
    kprintf(PRINT_SERIAL, "  free %zu -> %zu\n", before, after);
}
