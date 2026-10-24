#include "vmm.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/pmm/pmm.h"
#include "arch/x86_64/cpu/cpu.h"
#include "drivers/serial/serial.h"
#include "kernel/sync/sync.h"

static spinlock_t vmm_lock = SPINLOCK_INIT;

struct page_table *kernel_pml4 = NULL;
bool vmm_5level = false;
uint64_t vmm_hhdm_offset = 0;

static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline uint64_t read_cr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void invlpg(vaddr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

#define CR4_LA57 (1ULL << 12)

static bool cpu_has_5level(void) {
    return cpu_has_la57();
}

#define VMM_PT_ENTRIES 512
#define VMM_PT_INDEX_MASK 0x1FF
#define VMM_PT_ADDR_MASK 0x000FFFFFFFFFF000ULL

static struct page_table *alloc_page_table(void) {
    paddr_t phys;
    if (!pmm_alloc(&phys, 1)) return NULL;
    struct page_table *pt = (struct page_table *)(vmm_hhdm_offset + phys);
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) pt->entries[i] = 0;
    return pt;
}

static void free_page_table(struct page_table *pt) {
    pmm_free((paddr_t)((uint64_t)pt - vmm_hhdm_offset), 1);
}

static int level_shift(int level) {
    if (vmm_5level) {
        static const int shifts[5] = {48, 39, 30, 21, 12};
        return shifts[5 - level];
    }
    static const int shifts[4] = {39, 30, 21, 12};
    return shifts[4 - level];
}

static bool table_is_empty(struct page_table *pt) {
    const uint64_t *e = pt->entries;
    for (size_t i = 0; i < VMM_PT_ENTRIES; i += 8) {
        uint64_t any = e[i] | e[i + 1] | e[i + 2] | e[i + 3] |
                       e[i + 4] | e[i + 5] | e[i + 6] | e[i + 7];
        if (any) return false;
    }
    return true;
}

static uint64_t *walk_page_table(struct page_table *table, vaddr_t vaddr, int level, bool alloc, uint64_t flags) {
    if (level == 0) return NULL;

    int shift = level_shift(level);

    size_t index = (vaddr >> shift) & VMM_PT_INDEX_MASK;
    uint64_t entry = table->entries[index];

    if (level == 1) return &table->entries[index];

    struct page_table *next;
    if (entry & PAGE_PRESENT) {
        if (alloc && (flags & PAGE_USER) && !(entry & PAGE_USER)) {
            next = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
            if (!table_is_empty(next)) return NULL;
            table->entries[index] = entry | PAGE_USER;
        } else {
            next = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
        }
    } else if (alloc) {
        next = alloc_page_table();
        if (!next) return NULL;
        uint64_t iflags = PAGE_PRESENT | PAGE_WRITE;
        if (flags & PAGE_USER) iflags |= PAGE_USER;
        table->entries[index] = ((uint64_t)next - vmm_hhdm_offset) | iflags;
    } else {
        return NULL;
    }

    return walk_page_table(next, vaddr, level - 1, alloc, flags);
}

static void reclaim_ancestors(struct page_table *table, vaddr_t vaddr, int level) {
    if (level < 2) return;
    int shift = level_shift(level);
    size_t index = (vaddr >> shift) & VMM_PT_INDEX_MASK;
    uint64_t entry = table->entries[index];

    if (!(entry & PAGE_PRESENT)) return;

    struct page_table *child = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));

    if (level > 1) reclaim_ancestors(child, vaddr, level - 1);

    if (table == kernel_pml4) return;
    if (!table_is_empty(child)) return;

    table->entries[index] = 0;
    invlpg(vaddr);
    free_page_table(child);
}

void vmm_init(uint64_t hhdm_offset) {
    vmm_hhdm_offset = hhdm_offset;
    vmm_set_cow(true);

    bool cpu_supports_la57 = cpu_has_5level();
    bool paging_is_5level = (read_cr4() & CR4_LA57) != 0;
    vmm_5level = cpu_supports_la57 && paging_is_5level;

    uint64_t cr3 = read_cr3();
    kernel_pml4 = (struct page_table *)(vmm_hhdm_offset + (cr3 & 0x000FFFFFFFFFF000ULL));

    kprintf(PRINT_SERIAL, "VMM: Initialized, ");
    kprintf(PRINT_SERIAL, "%s", vmm_5level ? "5-level paging\n" : "4-level paging\n");
    if (cpu_supports_la57 && !paging_is_5level) {
        kprintf(PRINT_SERIAL, "VMM: LA57 available but CR4.LA57 clear, using 4-level walk\n");
    }
}

static void reclaim_range(struct page_table *root, vaddr_t vaddr, size_t pages, int levels) {
    for (size_t i = 0; i < pages; i++) {
        reclaim_ancestors(root, vaddr + i * PAGE_SIZE, levels);
    }
}

static bool vmm_map_in(struct page_table *root, vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    if (pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1) || paddr & (PAGE_SIZE - 1)) return false;

    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t *entry = walk_page_table(root, vaddr + i * PAGE_SIZE, levels, true, flags);
        if (!entry) {
            reclaim_range(root, vaddr, i + 1, levels);
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
        if (*entry & PAGE_PRESENT) {
            reclaim_range(root, vaddr, i + 1, levels);
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;
        paddr_t p = paddr + i * PAGE_SIZE;

        uint64_t *entry = walk_page_table(root, v, levels, true, flags);
        *entry = p | flags | PAGE_PRESENT;
    }

    lock_release(LOCK_VMM, &vmm_lock);
    return true;
}

bool vmm_map(vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    return vmm_map_in(kernel_pml4, vaddr, paddr, pages, flags);
}

static bool vmm_unmap_in(struct page_table *root, vaddr_t vaddr, size_t pages) {
    if (pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1)) return false;

    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t *entry = walk_page_table(root, vaddr + i * PAGE_SIZE, levels, false, 0);
        if (!entry || !(*entry & PAGE_PRESENT)) {
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;

        uint64_t *entry = walk_page_table(root, v, levels, false, 0);
        *entry = 0;
        invlpg(v);
    }

    reclaim_range(root, vaddr, pages, levels);

    lock_release(LOCK_VMM, &vmm_lock);
    return true;
}

bool vmm_unmap(vaddr_t vaddr, size_t pages) {
    return vmm_unmap_in(kernel_pml4, vaddr, pages);
}

static bool clone_user_level(struct page_table *src, struct page_table *dst,
                            vaddr_t base, int level, size_t *copied) {
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        uint64_t e = src->entries[i];
        if (!(e & PAGE_PRESENT)) continue;
        if (!(e & PAGE_USER)) continue;

        vaddr_t v = base + ((vaddr_t)i << (12 + 9 * (level - 1)));

        if (level == 1) {
            paddr_t np;
            if (!pmm_alloc(&np, 1)) return false;
            uint8_t *dp = (uint8_t *)(vmm_hhdm_offset + np);
            const uint8_t *sp2 =
                (const uint8_t *)(vmm_hhdm_offset + (e & VMM_PT_ADDR_MASK));
            for (size_t k = 0; k < PAGE_SIZE; k++)
                dp[k] = sp2[k];
            uint64_t nf = e & (PAGE_PRESENT | PAGE_WRITE | PAGE_PWT | PAGE_PCD | PAGE_NX);
            if (!vmm_map_frames(dst, v, np, 1, nf, !(e & PAGE_NX), true)) {
                pmm_free(np, 1);
                return false;
            }
            (*copied)++;
        } else {
            struct page_table *child =
                (struct page_table *)(vmm_hhdm_offset + (e & VMM_PT_ADDR_MASK));
            if (!clone_user_level(child, dst, v, level - 1, copied)) return false;
        }
    }
    return true;
}

static bool cow_on;
static size_t cow_shared_total;

size_t vmm_cow_shared(void) { return cow_shared_total; }

bool vmm_cow_enabled(void) {
    return cow_on;
}

bool vmm_set_cow(bool on) {
    if (on && !pmm_share_ready()) return false;
    cow_on = on;
    return true;
}

static bool share_user_level(struct page_table *src, struct page_table *dst,
                             vaddr_t base, int level, size_t *shared) {
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        uint64_t e = src->entries[i];
        if (!(e & PAGE_PRESENT)) continue;
        if (!(e & PAGE_USER)) continue;

        vaddr_t v = base + ((vaddr_t)i << (12 + 9 * (level - 1)));

        if (level == 1) {
            uint64_t phys = e & VMM_PT_ADDR_MASK;
            bool owned = (e & PAGE_OWNED) != 0;

            uint64_t se = e;
            uint64_t nf = e & (PAGE_PRESENT | PAGE_USER | PAGE_WRITE |
                               PAGE_PWT | PAGE_PCD | PAGE_NX);

            if (e & PAGE_PINNED) {
                nf = e & (PAGE_PRESENT | PAGE_USER | PAGE_WRITE |
                          PAGE_PWT | PAGE_PCD | PAGE_NX);
                if (!vmm_map_frames(dst, v, phys, 1, nf, !(e & PAGE_NX), false))
                    return false;
                (*shared)++;
                continue;
            }

            se = (e & ~PAGE_WRITE) | PAGE_COW;
            nf = (se & (PAGE_PRESENT | PAGE_USER | PAGE_WRITE |
                        PAGE_PWT | PAGE_PCD | PAGE_NX)) | PAGE_COW;
            bool was_shared = (e & PAGE_SHAREDREF) != 0;
            if (owned) {
                se |= PAGE_SHAREDREF;
                nf |= PAGE_SHAREDREF;
            }
            src->entries[i] = se;
            invlpg(v);
            if (!vmm_map_frames(dst, v, phys, 1, nf, !(se & PAGE_NX), false))
                return false;
            if (owned) {
                if (!was_shared) pmm_share_ref(phys);
                pmm_share_ref(phys);
            }
            (*shared)++;
        } else {
            struct page_table *child =
                (struct page_table *)(vmm_hhdm_offset + (e & VMM_PT_ADDR_MASK));
            if (!share_user_level(child, dst, v, level - 1, shared)) return false;
        }
    }
    return true;
}

bool vmm_share_user_space(struct page_table *src, struct page_table *dst, size_t *shared) {
    size_t n = 0;
    int levels = vmm_5level ? 5 : 4;
    bool ok = share_user_level(src, dst, 0, levels, &n);
    cow_shared_total += n;
    if (shared) *shared = n;
    return ok;
}

static void cow_privatise_level(struct page_table *root, struct page_table *table,
                               vaddr_t base, int level) {
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        uint64_t e = table->entries[i];
        if (!(e & PAGE_PRESENT)) continue;
        if (!(e & PAGE_USER)) continue;

        vaddr_t v = base + ((vaddr_t)i << (12 + 9 * (level - 1)));

        if (level == 1) {
            if ((e & PAGE_COW) && (e & PAGE_OWNED))
                vmm_cow_resolve(root, v);
            continue;
        }

        struct page_table *child =
            (struct page_table *)(vmm_hhdm_offset + (e & VMM_PT_ADDR_MASK));
        cow_privatise_level(root, child, v, level - 1);
    }
}

bool vmm_pin(struct page_table *root, vaddr_t vaddr) {
    if (!root) return false;
    uint64_t *entry = walk_page_table(root, vaddr, vmm_5level ? 5 : 4, false, 0);
    if (!entry || !(*entry & PAGE_PRESENT)) return false;
    *entry |= PAGE_PINNED;
    return true;
}

bool vmm_cow_privatise(struct page_table *root) {
    if (!root || !cow_on) return false;
    cow_privatise_level(root, root, 0, vmm_5level ? 5 : 4);
    return true;
}

bool vmm_cow_resolve(struct page_table *root, vaddr_t vaddr) {
    if (!root) return false;

    int levels = vmm_5level ? 5 : 4;
    uint64_t *entry = walk_page_table(root, vaddr, levels, false, 0);
    if (!entry || !(*entry & PAGE_PRESENT)) return false;
    if (!(*entry & PAGE_COW)) return false;
    if (*entry & PAGE_PINNED) return false;
    if (*entry & PAGE_WRITE) return false;

    paddr_t old = *entry & VMM_PT_ADDR_MASK;
    paddr_t np;

    if (!pmm_alloc(&np, 1)) return false;

    const uint8_t *src = (const uint8_t *)(vmm_hhdm_offset + old);
    uint8_t *dst = (uint8_t *)(vmm_hhdm_offset + np);
    for (size_t i = 0; i < PAGE_SIZE; i++) dst[i] = src[i];

    if (*entry & PAGE_SHAREDREF) {
        if (pmm_unshare_ref(old)) pmm_free(old, 1);
    }

    uint64_t nf = *entry & (PAGE_PRESENT | PAGE_USER | PAGE_PWT |
                             PAGE_PCD | PAGE_NX);
    nf |= PAGE_WRITE;
    nf &= ~(PAGE_COW | PAGE_SHAREDREF);
    nf |= PAGE_OWNED;
    *entry = nf | np;
    invlpg(vaddr);
    return true;
}

bool vmm_clone_user_space(struct page_table *src, struct page_table *dst, size_t *copied) {
    size_t n = 0;
    int levels = vmm_5level ? 5 : 4;
    bool ok = clone_user_level(src, dst, 0, levels, &n);
    if (copied) *copied = n;
    return ok;
}

bool vmm_range_present_in(struct page_table *root, vaddr_t addr, size_t len) {
    if (len == 0) return false;
    if (addr > VMM_HIGHER_HALF) return false;
    if (len > VMM_HIGHER_HALF - addr) return false;

    vaddr_t first = addr & ~(vaddr_t)(PAGE_SIZE - 1);
    vaddr_t last = (addr + len - 1) & ~(vaddr_t)(PAGE_SIZE - 1);

    int levels = vmm_5level ? 5 : 4;

    for (vaddr_t v = first; v <= last; v += PAGE_SIZE) {
        uint64_t *entry = walk_page_table(root, v, levels, false, 0);
        if (!entry) return false;
        if (!(*entry & PAGE_PRESENT)) return false;
    }

    return true;
}

bool vmm_range_writable_in(struct page_table *root, vaddr_t addr, size_t len) {
    if (len == 0) return false;
    if (addr > VMM_HIGHER_HALF) return false;
    if (len > VMM_HIGHER_HALF - addr) return false;

    vaddr_t first = addr & ~(vaddr_t)(PAGE_SIZE - 1);
    vaddr_t last = (addr + len - 1) & ~(vaddr_t)(PAGE_SIZE - 1);

    int levels = vmm_5level ? 5 : 4;

    for (vaddr_t v = first; v <= last; v += PAGE_SIZE) {
        uint64_t *entry = walk_page_table(root, v, levels, false, 0);
        if (!entry) return false;
        if (!(*entry & PAGE_PRESENT)) return false;
        if (!(*entry & PAGE_WRITE)) return false;
    }

    return true;
}

uint64_t vmm_query_in(struct page_table *root, vaddr_t vaddr) {
    if (!root) return 0;
    uint64_t *entry = walk_page_table(root, vaddr, vmm_5level ? 5 : 4, false, 0);
    if (!entry) return 0;
    return *entry;
}

bool vmm_unmap_keep(struct page_table *root, vaddr_t vaddr, size_t pages) {
    if (!root || pages == 0) return false;
    if (vaddr & (PAGE_SIZE - 1)) return false;

    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t *entry = walk_page_table(root, vaddr + i * PAGE_SIZE, levels, false, 0);
        if (!entry || !(*entry & PAGE_PRESENT)) {
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        vaddr_t v = vaddr + i * PAGE_SIZE;
        uint64_t *entry = walk_page_table(root, v, levels, false, 0);
        *entry = 0;
        invlpg(v);
    }

    reclaim_range(root, vaddr, pages, levels);

    lock_release(LOCK_VMM, &vmm_lock);
    return true;
}

bool vmm_unmap_keep_frames(struct page_table *root, vaddr_t vaddr, size_t pages, paddr_t *freed, size_t max_freed) {
    if (!vmm_unmap_keep(root, vaddr, pages)) return false;
    for (size_t i = 0; i < pages; i++) {
        if (freed && i < max_freed) freed[i] = 0;
    }
    return true;
}

bool vmm_map_frames(struct page_table *root, vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags, bool exec, bool owned) {
    if (cpu_has_nx()) {
        if (exec) flags &= ~PAGE_NX;
        else flags |= PAGE_NX;
    }
    flags |= PAGE_USER;
    if (owned) flags |= PAGE_OWNED;
    return vmm_map_in(root, vaddr, paddr, pages, flags);
}

bool vmm_protect(struct page_table *root, vaddr_t addr, size_t pages, uint8_t prot) {
    if (pages == 0) return false;
    if (addr & (PAGE_SIZE - 1)) return false;
    if (prot & ~(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) return false;
    if ((prot & VM_PROT_WRITE) && !(prot & VM_PROT_READ)) return false;
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC)) return false;

    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t *entry = walk_page_table(root, addr + i * PAGE_SIZE, levels, false, 0);
        if (!entry || !(*entry & PAGE_PRESENT)) {
            lock_release(LOCK_VMM, &vmm_lock);
            return false;
        }

        uint64_t f = *entry;
        if (prot & VM_PROT_WRITE) f |= PAGE_WRITE;
        else f &= ~PAGE_WRITE;
        if (cpu_has_nx()) {
            if (prot & VM_PROT_EXEC) f &= ~PAGE_NX;
            else f |= PAGE_NX;
        }
        *entry = f;
    }

    lock_release(LOCK_VMM, &vmm_lock);

    for (size_t i = 0; i < pages; i++)
        invlpg(addr + i * PAGE_SIZE);

    return true;
}

uint64_t vmm_query(vaddr_t vaddr) {
    uint64_t *entry = walk_page_table(kernel_pml4, vaddr, vmm_5level ? 5 : 4, false, 0);
    if (!entry) return 0;
    return *entry;
}


static bool table_freeable(struct page_table *table, int level) {
    if (table_is_empty(table)) return true;
    if (level == 1) return false;

    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        uint64_t entry = table->entries[i];
        if (!(entry & PAGE_PRESENT)) continue;

        struct page_table *child = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
        if (!table_freeable(child, level - 1)) return false;
    }

    return true;
}

static void table_free(struct page_table *table, int level) {
    if (level > 1) {
        for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
            uint64_t entry = table->entries[i];
            if (!(entry & PAGE_PRESENT)) continue;

            struct page_table *child = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
            table_free(child, level - 1);
        }
    }
    free_page_table(table);
}

struct page_table *vmm_create_address_space(void) {
    lock_acquire(LOCK_VMM, &vmm_lock);

    struct page_table *new_pml4 = alloc_page_table();
    if (!new_pml4) {
        lock_release(LOCK_VMM, &vmm_lock);
        return NULL;
    }

    for (size_t i = 256; i < 512; i++) {
        new_pml4->entries[i] = kernel_pml4->entries[i];
    }

    lock_release(LOCK_VMM, &vmm_lock);
    return new_pml4;
}

bool vmm_map_user(struct page_table *root, vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    return vmm_map_in(root, vaddr, paddr, pages, flags | PAGE_USER | PAGE_OWNED);
}

bool vmm_map_user_exec(struct page_table *root, vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    if (cpu_has_nx()) flags &= ~PAGE_NX;
    return vmm_map_in(root, vaddr, paddr, pages, flags | PAGE_USER | PAGE_OWNED);
}

bool vmm_map_user_lent(struct page_table *root, vaddr_t vaddr, paddr_t paddr, size_t pages, uint64_t flags) {
    return vmm_map_in(root, vaddr, paddr, pages, flags | PAGE_USER);
}

bool vmm_unmap_from(struct page_table *root, vaddr_t vaddr, size_t pages) {
    return vmm_unmap_in(root, vaddr, pages);
}

void vmm_switch_address_space(struct page_table *pml4) {
    uint64_t cr3 = (uint64_t)pml4 - vmm_hhdm_offset;
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    cpu_flush_pcid();
}

static void release_shared_frames(struct page_table *table, int level);

static void reclaim_owned_frames(struct page_table *table, int level, uint32_t *freed) {
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        uint64_t entry = table->entries[i];
        if (!(entry & PAGE_PRESENT)) continue;

        if (level == 1) {
            if (entry & PAGE_COW) {
                table->entries[i] = 0;
                continue;
            }
            if (entry & PAGE_OWNED) {
                pmm_free(entry & VMM_PT_ADDR_MASK, 1);
                (*freed)++;
            }
            table->entries[i] = 0;
            continue;
        }

        struct page_table *child = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
        reclaim_owned_frames(child, level - 1, freed);
    }
}

static void release_shared_frames(struct page_table *table, int level) {
    for (size_t i = 0; i < VMM_PT_ENTRIES; i++) {
        uint64_t entry = table->entries[i];
        if (!(entry & PAGE_PRESENT)) continue;

        if (level == 1) {
            if (entry & PAGE_SHAREDREF) {
                if (pmm_unshare_ref(entry & VMM_PT_ADDR_MASK))
                    pmm_free(entry & VMM_PT_ADDR_MASK, 1);
            }
            continue;
        }

        struct page_table *child =
            (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));
        release_shared_frames(child, level - 1);
    }
}

void vmm_destroy_address_space(struct page_table *pml4) {
    if (!pml4) return;

    int levels = vmm_5level ? 5 : 4;

    lock_acquire(LOCK_VMM, &vmm_lock);

    size_t stranded = 0;
    uint32_t freed = 0;

    for (size_t i = 0; i < 256; i++) {
        uint64_t entry = pml4->entries[i];
        if (!(entry & PAGE_PRESENT)) continue;

        struct page_table *table = (struct page_table *)(vmm_hhdm_offset + (entry & VMM_PT_ADDR_MASK));

        release_shared_frames(table, levels - 1);
        reclaim_owned_frames(table, levels - 1, &freed);

        if (!table_freeable(table, levels - 1)) {
            stranded++;
            continue;
        }

        table_free(table, levels - 1);

        pml4->entries[i] = 0;
        invlpg((vaddr_t)i << level_shift(levels));
    }

    if (stranded) {
        kprintf(PRINT_SERIAL, "VMM: %u subtree(s) still referenced, address space not released\n", (unsigned)stranded);
    } else {
        pmm_free((paddr_t)((uint64_t)pml4 - vmm_hhdm_offset), 1);
    }

    lock_release(LOCK_VMM, &vmm_lock);

    if (freed) kprintf(PRINT_SERIAL, "VMM: address space released, %u frame(s) reclaimed\n", (unsigned)freed);
}
