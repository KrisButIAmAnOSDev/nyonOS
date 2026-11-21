#include "kernel/vfs/vfs.h"
#include "kernel/lib/string.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/pmm/pmm.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/mm/vmm/vmspace.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/mm/pagecache/pcache.h"
#include "kernel/mm/slab/slab.h"
#include "kernel/multitask/task.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/cpu/cpu.h"

#define PROC_BUF_MAX 4096
#define PROC_ATTR_SELF 0x40

enum {
    PROC_KIND_NONE = 0,
    PROC_KIND_MEMINFO,
    PROC_KIND_UPTIME,
    PROC_KIND_VERSION,
    PROC_KIND_CPUINFO,
    PROC_KIND_STAT,
    PROC_KIND_LOADAVG,
    PROC_KIND_TASK_STAT,
    PROC_KIND_TASK_STATUS,
    PROC_KIND_TASK_CMDLINE,
    PROC_KIND_SELF_EXE,
};

enum {
    PROC_DIR_ROOT = 0,
    PROC_DIR_PID,
    PROC_DIR_SELF,
};

static const char *const proc_root_files[] = {
    "meminfo", "uptime", "version", "cpuinfo", "stat", "loadavg",
};

static const uint32_t proc_root_kinds[] = {
    PROC_KIND_MEMINFO, PROC_KIND_UPTIME, PROC_KIND_VERSION,
    PROC_KIND_CPUINFO, PROC_KIND_STAT, PROC_KIND_LOADAVG,
};

#define PROC_ROOT_FILE_COUNT ((uint32_t)(sizeof(proc_root_files) / sizeof(proc_root_files[0])))

static const char *const proc_task_files[] = { "stat", "status", "cmdline" };

static const uint32_t proc_task_kinds[] = {
    PROC_KIND_TASK_STAT, PROC_KIND_TASK_STATUS, PROC_KIND_TASK_CMDLINE,
};

#define PROC_TASK_FILE_COUNT ((uint32_t)(sizeof(proc_task_files) / sizeof(proc_task_files[0])))

static char proc_bufs[MAX_CPUS][PROC_BUF_MAX];

static char *proc_scratch(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) cpu = 0;
    return proc_bufs[cpu];
}

static bool proc_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if ((*a | 0x20) != (*b | 0x20)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool proc_parse_pid(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    *out = v;
    return true;
}

static struct task *proc_find_task(uint32_t pid) {
    for (size_t i = 1; i < TASK_MAX; i++) {
        struct task *t = task_at(i);
        if (t && t->in_use && t->pid == pid) return t;
    }
    return NULL;
}

static uint32_t proc_live_tasks(void) {
    uint32_t n = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        struct task *t = task_at(i);
        if (t && t->in_use) n++;
    }
    return n;
}

static uint32_t proc_nth_task(uint32_t index, uint32_t *pid_out) {
    uint32_t n = 0;
    for (size_t i = 1; i < TASK_MAX; i++) {
        struct task *t = task_at(i);
        if (!t || !t->in_use) continue;
        if (n == index) {
            if (pid_out) *pid_out = t->pid;
            return n + 1;
        }
        n++;
    }
    return 0;
}

static void proc_fill(fs_node_t *out, const char *name, bool is_dir, uint32_t kind,
                      uint32_t pid, uint32_t size, bool self_dir) {
    memset(out, 0, sizeof(*out));
    size_t n = strlen(name);
    if (n >= FS_MAX_NAME) n = FS_MAX_NAME - 1;
    memcpy(out->name, name, n);
    out->attr = is_dir ? 0x10 : 0x20;
    if (self_dir) out->attr |= PROC_ATTR_SELF;
    out->cluster = kind;
    out->size = size;
    out->is_dir = is_dir;
    out->is_root = false;
    out->dir_cluster = pid;
    out->dir_index = 0;
    out->ops = proc_ops();
    out->dev_id = 0;
    out->cache_key = 0;
    out->mode = is_dir ? 0755 : 0444;
    out->uid = 0;
    out->gid = 0;
}

static uint32_t proc_dir_kind(const fs_node_t *node) {
    if (!node) return PROC_DIR_ROOT;
    if (node->attr & PROC_ATTR_SELF) return PROC_DIR_SELF;
    if (node->is_root) return PROC_DIR_ROOT;
    return PROC_DIR_PID;
}


static uint64_t proc_uptime_centis(void) {
    uint32_t freq = pit_frequency();
    if (!freq) return 0;
    return (pit_get_ticks() * 100) / freq;
}

static uint64_t proc_task_vsize(const struct task *t) {
    if (!t || !t->vs) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < t->vs->region_count; i++) {
        const struct vm_region *r = &t->vs->regions[i];
        total += r->end - r->start;
    }
    return total;
}

static char proc_task_state(const struct task *t) {
    if (!t) return 'X';
    if (t->zombie) return 'Z';
    if (t->stopped) return 'T';
    if (t->blocked) return 'S';
    return 'R';
}

static uint32_t proc_gen_meminfo(char *buf, uint32_t cap) {
    size_t total = pmm_total_pages();
    size_t free_pages = pmm_free_pages();
    uint64_t total_kb = (uint64_t)total * (PAGE_SIZE / 1024);
    uint64_t free_kb = (uint64_t)free_pages * (PAGE_SIZE / 1024);
    uint64_t used_kb = total_kb > free_kb ? total_kb - free_kb : 0;

    size_t reserved = pmm_reserved_pages();
    size_t used_pages = total > free_pages ? total - free_pages : 0;
    size_t os_pages = used_pages > reserved ? used_pages - reserved : 0;

    size_t t_heap = pmm_pages_by_tag(PMM_TAG_HEAP);
    size_t t_cache = pmm_pages_by_tag(PMM_TAG_CACHE);
    size_t t_ptab = pmm_pages_by_tag(PMM_TAG_PTABLES);
    size_t t_cow = pmm_pages_by_tag(PMM_TAG_COW);
    size_t t_elf = pmm_pages_by_tag(PMM_TAG_ELF);
    size_t t_stack = pmm_pages_by_tag(PMM_TAG_STACK);
    size_t t_slab = pmm_pages_by_tag(PMM_TAG_SLAB);
    size_t t_user = pmm_pages_by_tag(PMM_TAG_USER);
    size_t t_buf = pmm_cache_pages_used();

    size_t tagged = t_heap + t_cache + t_ptab + t_cow + t_elf + t_stack +
                    t_slab + t_user + t_buf;
    size_t untagged = os_pages > tagged ? os_pages - tagged : 0;

    int n = snprintf(buf, cap,
        "MemTotal:     %8llu kB\n"
        "MemFree:      %8llu kB\n"
        "MemUsed:      %8llu kB\n"
        "MemAvailable: %8llu kB\n"
        "PagesTotal:   %8llu\n"
        "PagesFree:    %8llu\n"
        "PagesReserved:%8llu\n"
        "PagesUsed:    %8llu\n"
        "PagesOS:      %8llu\n"
        "PageSize:     %8llu\n"
        "UserFrames:   %8llu\n"
        "Tasks:        %8llu\n"
        "AllocHeap:    %8llu\n"
        "AllocCache:   %8llu\n"
        "AllocPageTbl: %8llu\n"
        "AllocCow:     %8llu\n"
        "AllocElf:     %8llu\n"
        "AllocStack:   %8llu\n"
        "AllocSlab:    %8llu\n"
        "AllocUser:    %8llu\n"
        "AllocPcacheBuf:%7llu\n"
        "AllocUntagged:%8llu\n",
        (unsigned long long)total_kb,
        (unsigned long long)free_kb,
        (unsigned long long)used_kb,
        (unsigned long long)free_kb,
        (unsigned long long)total,
        (unsigned long long)free_pages,
        (unsigned long long)reserved,
        (unsigned long long)used_pages,
        (unsigned long long)os_pages,
        (unsigned long long)PAGE_SIZE,
        (unsigned long long)task_user_frames(),
        (unsigned long long)task_count_live(),
        (unsigned long long)t_heap,
        (unsigned long long)t_cache,
        (unsigned long long)t_ptab,
        (unsigned long long)t_cow,
        (unsigned long long)t_elf,
        (unsigned long long)t_stack,
        (unsigned long long)t_slab,
        (unsigned long long)t_user,
        (unsigned long long)t_buf,
        (unsigned long long)untagged);
    return n > 0 ? (uint32_t)n : 0;
}

static uint32_t proc_gen_uptime(char *buf, uint32_t cap) {
    uint64_t cs = proc_uptime_centis();
    uint64_t up = cs / 100;
    uint64_t idle = cs - up * 100;

    int n = snprintf(buf, cap, "%8llu.%02llu %8llu.%02llu\n",
                     (unsigned long long)up, (unsigned long long)idle,
                     (unsigned long long)up, (unsigned long long)idle);
    return n > 0 ? (uint32_t)n : 0;
}

static uint32_t proc_gen_version(char *buf, uint32_t cap) {
    int n = snprintf(buf, cap,
        "nyonOS version 26.0 (x86_64) Limine bootloader ext2 root\n");
    return n > 0 ? (uint32_t)n : 0;
}

static uint32_t proc_gen_cpuinfo(char *buf, uint32_t cap) {
    const struct cpu_features *f = cpu_features_get();
    uint32_t ncpus = cpu_count();
    if (!ncpus) ncpus = 1;

    char brand[64], vendor[16];
    cpu_brand_string(brand, sizeof brand);
    cpu_vendor_string(vendor, sizeof vendor);
    if (!brand[0]) snprintf(brand, sizeof brand, "unknown cpu");

    uint32_t used = 0;
    for (uint32_t i = 0; i < ncpus; i++) {
        char line[256];
        int n = snprintf(line, sizeof line,
            "processor\t: %u\n"
            "vendor_id\t: %s\n"
            "model name\t: %s\n"
            "cpu family\t: %u\n"
            "model\t\t: %u\n"
            "stepping\t: %u\n"
            "cpu MHz\t\t: %u.000\n"
            "flags\t\t: %s%s%s%s%s%s%s%s%s%s%s%s%s\n"
            "bogomips\t: %u.00\n"
            "cache size\t: %u KB\n",
            i, vendor, brand,
            f ? f->family : 6u, f ? f->model : 142u, f ? f->stepping : 9u,
            f && f->tsc_mhz ? f->tsc_mhz : 1000,
            f && f->nx ? "nx " : "",
            f && f->smep ? "smep " : "",
            f && f->smap ? "smap " : "",
            f && f->apic ? "apic " : "",
            f && f->fpu ? "fpu " : "",
            f && f->sse ? "sse " : "",
            f && f->sse2 ? "sse2 " : "",
            f && f->rdrand ? "rdrand " : "",
            f && f->fsgsbase ? "fsgsbase " : "",
            f && f->pcid ? "pcid " : "",
            f && f->tsc ? "tsc " : "",
            f && f->syscall ? "syscall " : "",
            f && f->invpcid ? "invpcid " : "",
            (f && f->tsc_mhz ? f->tsc_mhz : 1000) * 1000u,
            f ? f->cache_kb : 256u);
        if (n <= 0) break;
        if (used + (uint32_t)n >= cap) break;
        memcpy(buf + used, line, (size_t)n);
        used += (uint32_t)n;
    }
    return used;
}

static uint32_t proc_gen_stat(char *buf, uint32_t cap) {
    uint32_t ncpus = cpu_count();
    if (!ncpus) ncpus = 1;

    uint64_t busy = 0, idle = 0, ctxt = 0;
    for (uint32_t i = 0; i < ncpus; i++) {
        busy += cpu_percpu_at(i)->busy_ticks;
        idle += cpu_percpu_at(i)->idle_ticks;
        ctxt += cpu_percpu_at(i)->switch_count;
    }

    uint32_t used = 0;
    int n = snprintf(buf, cap, "cpu  %8llu 0 0 %8llu 0 0 0 0 0 0\n",
                     (unsigned long long)busy, (unsigned long long)idle);
    if (n <= 0) return 0;
    used = (uint32_t)n;

    for (uint32_t i = 0; i < ncpus; i++) {
        n = snprintf(buf + used, cap - used, "cpu%u %8llu 0 0 %8llu 0 0 0 0 0 0\n",
                     i,
                     (unsigned long long)cpu_percpu_at(i)->busy_ticks,
                     (unsigned long long)cpu_percpu_at(i)->idle_ticks);
        if (n <= 0) break;
        used += (uint32_t)n;
        if (used >= cap) break;
    }

    n = snprintf(buf + used, cap - used,
                 "intr %8llu\n"
                 "ctxt %8llu\n"
                 "processes %8llu\n"
                 "procs_running %8llu\n"
                 "procs_blocked %8llu\n"
                 "btime %8llu\n",
                 (unsigned long long)(busy + idle),
                 (unsigned long long)ctxt,
                 (unsigned long long)proc_live_tasks(),
                 (unsigned long long)1,
                 (unsigned long long)0,
                 (unsigned long long)0);
    if (n > 0) used += (uint32_t)n;
    return used;
}

static uint32_t proc_gen_loadavg(char *buf, uint32_t cap) {
    uint32_t n = proc_live_tasks();
    int r = snprintf(buf, cap, "%8llu.%02llu %8llu.%02llu %8llu.%02llu %8llu/%8llu\n",
                     0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL,
                     (unsigned long long)n, (unsigned long long)n);
    return r > 0 ? (uint32_t)r : 0;
}

static uint32_t proc_gen_task_stat(const fs_node_t *node, char *buf, uint32_t cap) {
    struct task *t = proc_find_task(node->dir_cluster);
    if (!t) {
        int n = snprintf(buf, cap, "0 (gone) X 0 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0\n");
        return n > 0 ? (uint32_t)n : 0;
    }

    const char *name = t->name ? t->name : "?";
    int n = snprintf(buf, cap,
        "%u (%s) %c %u %u %u %u %u %u %llu %llu %llu %llu %llu %llu %llu %llu %d %d %d %llu %llu %llu %llu\n",
        t->pid, name, proc_task_state(t),
        t->parent_pid, t->pgid, t->sid, 0u, t->pgid,
        0ULL, 0ULL, 0ULL, 0ULL,
        0ULL, 0ULL, 0ULL, 0ULL,
        20, 0, 1,
        0ULL, proc_task_vsize(t), 0ULL, 0ULL);
    return n > 0 ? (uint32_t)n : 0;
}

static uint32_t proc_gen_task_status(const fs_node_t *node, char *buf, uint32_t cap) {
    struct task *t = proc_find_task(node->dir_cluster);
    if (!t) {
        int n = snprintf(buf, cap, "Name:\tgone\nState:\tX\nTgid:\t0\nPid:\t0\nPPid:\t0\n");
        return n > 0 ? (uint32_t)n : 0;
    }

    const char *name = t->name ? t->name : "?";
    uint64_t vsize_kb = proc_task_vsize(t) / 1024;
    int n = snprintf(buf, cap,
        "Name:\t%s\n"
        "State:\t%c (%s)\n"
        "Tgid:\t%u\n"
        "Pid:\t%u\n"
        "PPid:\t%u\n"
        "Uid:\t%u\t%u\t%u\t%u\n"
        "Gid:\t%u\t%u\t%u\t%u\n"
        "Threads:\t%u\n"
        "VmSize:\t%8llu kB\n"
        "VmRSS:\t%8llu kB\n"
        "voluntary_ctxt_switches:\t%llu\n"
        "nonvoluntary_ctxt_switches:\t%llu\n",
        name, proc_task_state(t),
        proc_task_state(t) == 'R' ? "running" :
        proc_task_state(t) == 'S' ? "sleeping" :
        proc_task_state(t) == 'Z' ? "zombie" : "stopped",
        t->pid, t->pid, t->parent_pid,
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
        1u,
        (unsigned long long)vsize_kb,
        0ULL,
        0ULL, 0ULL);
    return n > 0 ? (uint32_t)n : 0;
}

static uint32_t proc_gen_task_cmdline(const fs_node_t *node, char *buf, uint32_t cap) {
    struct task *t = proc_find_task(node->dir_cluster);
    const char *name = t && t->name ? t->name : "";
    size_t n = strlen(name);
    if (n + 1 > cap) n = cap - 1;
    memcpy(buf, name, n);
    if (n < cap) buf[n] = 0;
    return (uint32_t)n + 1;
}

static uint32_t proc_generate(const fs_node_t *node, char *buf, uint32_t cap) {
    if (!node) return 0;

    switch (node->cluster) {
        case PROC_KIND_MEMINFO: return proc_gen_meminfo(buf, cap);
        case PROC_KIND_UPTIME: return proc_gen_uptime(buf, cap);
        case PROC_KIND_VERSION: return proc_gen_version(buf, cap);
        case PROC_KIND_CPUINFO: return proc_gen_cpuinfo(buf, cap);
        case PROC_KIND_STAT: return proc_gen_stat(buf, cap);
        case PROC_KIND_LOADAVG: return proc_gen_loadavg(buf, cap);
        case PROC_KIND_TASK_STAT: return proc_gen_task_stat(node, buf, cap);
        case PROC_KIND_TASK_STATUS: return proc_gen_task_status(node, buf, cap);
        case PROC_KIND_TASK_CMDLINE: return proc_gen_task_cmdline(node, buf, cap);
        default: return 0;
    }
}

static bool proc_lookup_in_dir(const fs_node_t *dir, const char *name, fs_node_t *out) {
    uint32_t kind = proc_dir_kind(dir);

    if (kind == PROC_DIR_ROOT) {
        for (uint32_t i = 0; i < PROC_ROOT_FILE_COUNT; i++) {
            if (!proc_name_eq(proc_root_files[i], name)) continue;
            char *scratch = proc_scratch();
            uint32_t size = proc_generate(&(fs_node_t){ .cluster = proc_root_kinds[i] },
                                          scratch, PROC_BUF_MAX);
            proc_fill(out, proc_root_files[i], false, proc_root_kinds[i], 0, size, false);
            return true;
        }

        if (proc_name_eq(name, "self")) {
            struct task *t = task_current();
            uint32_t pid = t ? t->pid : 0;
            proc_fill(out, "self", true, PROC_KIND_NONE, pid, 0, true);
            return true;
        }

        uint32_t pid = 0;
        if (proc_parse_pid(name, &pid) && proc_find_task(pid)) {
            proc_fill(out, name, true, PROC_KIND_NONE, pid, 0, false);
            return true;
        }
        return false;
    }

    uint32_t pid = dir->dir_cluster;
    if (proc_name_eq(name, "exe")) {
        proc_fill(out, "exe", false, PROC_KIND_SELF_EXE, pid, 0, false);
        return true;
    }
    for (uint32_t i = 0; i < PROC_TASK_FILE_COUNT; i++) {
        if (!proc_name_eq(proc_task_files[i], name)) continue;
        char *scratch = proc_scratch();
        fs_node_t probe = { .cluster = proc_task_kinds[i], .dir_cluster = pid };
        uint32_t size = proc_generate(&probe, scratch, PROC_BUF_MAX);
        proc_fill(out, proc_task_files[i], false, proc_task_kinds[i], pid, size, false);
        return true;
    }
    return false;
}

static bool proc_iterate_root(uint32_t index, fs_node_t *out) {
    uint32_t pid = 0;
    uint32_t n = proc_nth_task(index, &pid);
    if (n) {
        char name[16];
        snprintf(name, sizeof name, "%u", pid);
        proc_fill(out, name, true, PROC_KIND_NONE, pid, 0, false);
        return true;
    }

    uint32_t tasks = proc_live_tasks();
    if (index == tasks) {
        struct task *t = task_current();
        proc_fill(out, "self", true, PROC_KIND_NONE, t ? t->pid : 0, 0, true);
        return true;
    }

    uint32_t fi = index - tasks - 1;
    if (fi >= PROC_ROOT_FILE_COUNT) return false;
    char *scratch = proc_scratch();
    fs_node_t probe = { .cluster = proc_root_kinds[fi] };
    uint32_t size = proc_generate(&probe, scratch, PROC_BUF_MAX);
    proc_fill(out, proc_root_files[fi], false, proc_root_kinds[fi], 0, size, false);
    return true;
}

static bool proc_iterate(const fs_node_t *dir, uint32_t index, fs_node_t *out) {
    if (!dir || !out) return false;

    uint32_t kind = proc_dir_kind(dir);
    if (kind == PROC_DIR_ROOT) return proc_iterate_root(index, out);

    if (index >= PROC_TASK_FILE_COUNT) return false;
    char *scratch = proc_scratch();
    fs_node_t probe = { .cluster = proc_task_kinds[index], .dir_cluster = dir->dir_cluster };
    uint32_t size = proc_generate(&probe, scratch, PROC_BUF_MAX);
    proc_fill(out, proc_task_files[index], false, proc_task_kinds[index],
              dir->dir_cluster, size, false);
    return true;
}

static bool proc_lookup(const char *path, fs_node_t *out) {
    if (!out) return false;

    fs_node_t cur;
    memset(&cur, 0, sizeof cur);
    cur.attr = 0x10;
    cur.is_dir = true;
    cur.is_root = true;
    cur.ops = proc_ops();

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        *out = cur;
        return true;
    }

    char comp[FS_MAX_NAME + 2];
    for (;;) {
        uint32_t n = 0;
        while (p[n] && p[n] != '/') n++;
        if (n == 0 || n >= FS_MAX_NAME + 1) return false;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;
        while (*p == '/') p++;

        fs_node_t found;
        if (!proc_lookup_in_dir(&cur, comp, &found)) return false;
        cur = found;

        if (!*p) {
            *out = cur;
            return true;
        }
        if (!cur.is_dir) return false;
    }
}

static bool proc_read(const fs_node_t *node, uint32_t offset, void *buf, uint32_t len) {
    if (!node || !buf) return false;
    if (node->is_dir) return false;
    if (len == 0) return true;

    char *scratch = proc_scratch();
    uint32_t total = proc_generate(node, scratch, PROC_BUF_MAX);
    if (offset >= total) {
        memset(buf, 0, len);
        return true;
    }

    uint32_t avail = total - offset;
    uint32_t copy = len < avail ? len : avail;
    memcpy(buf, scratch + offset, copy);
    if (copy < len) memset((uint8_t *)buf + copy, 0, len - copy);
    return true;
}

static int proc_write(fs_node_t *node, uint32_t offset, const void *buf, uint32_t len) {
    (void)offset;
    (void)buf;
    (void)len;
    if (!node) return -1;
    return -1;
}

static bool proc_readlink(const char *path, char *buf, uint32_t size) {
    if (!path || !buf || !size) return false;

    const char *p = path;
    while (*p == '/') p++;
    const char *slash = NULL;
    for (const char *q = p; *q; q++) {
        if (*q == '/') slash = q;
    }
    if (!slash) return false;

    size_t last_len = strlen(slash + 1);
    if (last_len != 3 || (slash[1] | 0x20) != 'e' || (slash[2] | 0x20) != 'x' ||
        (slash[3] | 0x20) != 'e')
        return false;

    size_t dir_len = (size_t)(slash - p);
    uint32_t pid = 0;
    if (dir_len == 4 && !strncasecmp(p, "self", 4)) {
        struct task *t = task_current();
        if (!t) return false;
        pid = t->pid;
    } else {
        char num[16];
        if (dir_len == 0 || dir_len >= sizeof num) return false;
        memcpy(num, p, dir_len);
        num[dir_len] = 0;
        if (!proc_parse_pid(num, &pid)) return false;
    }

    struct task *t = proc_find_task(pid);
    if (!t || !t->exe_path) return false;

    size_t n = strlen(t->exe_path);
    if (n >= size) n = size - 1;
    memcpy(buf, t->exe_path, n);
    buf[n] = 0;
    return true;
}

static const struct fs_ops proc_ops_table = {
    .iterate = proc_iterate,
    .lookup = proc_lookup,
    .read = proc_read,
    .write = proc_write,
    .readlink = proc_readlink,
};

const struct fs_ops *proc_ops(void) {
    return &proc_ops_table;
}

bool proc_init(void) {
    return vfs_mount("/PROC", &proc_ops_table);
}
