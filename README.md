# nyonOS

A minimal x86_64 hobby OS kernel written from scratch. Boots via Limine on BIOS/UEFI with idk

## Features

- **Limine boot protocol** (revision 6) - works on BIOS and UEFI
- **Mutitasking (premitive)** with synchronization too
- **Fat 16 filesystem** read only for now
- **VGA framebuffer rendering** with 8x16 font (from `kbd` package, GPL-3.0)
- **elf loader (working!!!)** yea its working
- **mlibc ruuning (mlibc port ig?)** its running 
- **Working pmm and vmm** it works,at least (and 5lvl paging)
- **"bash and busybox "** running

## Building

```bash
make run
```

Output: `nyonOS.img` (GPT disk image with Limine bootloader) (also auto run qemu with the rights flag)
`

## Disclaimer

This project uses LLM assistance. Some of the code in this repository was
written by an LLM (an AI coding assistant) rather than by a human.

Roughly 70% of the code is human-written and 30% is LLM-generated.

LLM-generated code has not necessarily been reviewed line by line by a human.
Read it before you rely on it. The project is provided as-is, with no warranty
of any kind.

also sometime stupid llm i try touch my code and make my code stupid too so expect werid things,i did check my code after "stupid" llm touch it but still,be aware of it

and if you think this project is vibecoded,tag me in osdev discord server and ask me about an random function,i will explain it myself

## License

GPL-3.0-only (font data from `kbd` package: GPL-2.0-or-later; limine: BSD-2-Clause)

The license covers the LLM-generated portions as well as the human-written ones.
