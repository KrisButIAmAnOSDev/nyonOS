#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/fs/fs.h"
typedef fs_node_t fs_node;

#define ELF_NIDENT 16

typedef struct {
    uint8_t  ident[ELF_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_header;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr;

#define ELF_ET_EXEC 2
#define ELF_ET_DYN  3

#define ELF_PT_LOAD     1
#define ELF_PT_INTERP   3
#define ELF_PT_GNU_STACK 0x6474e551

#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

#define ELF_MAX_PHNUM   64
#define ELF_MAX_IMAGE   (64ULL << 20)

#define ELF_EBADMAGIC   (-1)
#define ELF_EBADCLASS   (-2)
#define ELF_EBADDATA    (-3)
#define ELF_EBADMACHINE (-4)
#define ELF_EBADVERS    (-5)
#define ELF_EBADTYPE    (-6)
#define ELF_EPHDRSIZE   (-7)
#define ELF_EPHNUM      (-8)
#define ELF_EOFFSET     (-9)
#define ELF_EMEMSZ      (-10)
#define ELF_EALIGN      (-11)
#define ELF_EADDR       (-12)
#define ELF_EOVERLAP    (-13)
#define ELF_EWX         (-14)
#define ELF_EENTRY      (-15)
#define ELF_ETOOLARGE   (-16)
#define ELF_EINTERP     (-17)
#define ELF_EOMAGIC     (-18)

struct vmspace;

struct elf_image {
    struct vmspace *vs;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t argc;
    uint64_t argv;
    uint64_t auxv;
    bool is_pie;
};

int elf_validate(const uint8_t *hdr, size_t hdr_len, size_t file_size, const elf64_phdr **out_ph, size_t *out_n, bool *out_pie, uint64_t *out_entry);
const char *elf_error_text(int err);

int elf_load(const fs_node *file, struct elf_image *out, const char **err);
int elf_load_argv(const fs_node *file, struct elf_image *out, const char **err, const char **argv, size_t argc);

void elf_destroy(struct elf_image *img);

#endif
