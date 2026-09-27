# nyonOS

A minimal x86_64 hobby OS kernel written from scratch. Boots via Limine on BIOS/UEFI with VGA text output and serial debug logging.

## Features

- **Limine boot protocol** (revision 6) - works on BIOS and UEFI
- **Higher-half kernel** at `0xffffffff80000000` with ELF64 entry point `kmain`
- **VGA framebuffer rendering** with 8x16 font (from `kbd` package, GPL-2.0)
- **Serial debug output** (COM1, 115200 8N1)
- **PIT timer** (channel 0, mode 2 rate generator) at 1000 Hz driving IRQ0` (didnt work yet)
- **Working pmm and vmm** it works,at least
- **"working"** keyboard dirver (ps/2)

## Building

```bash
make clean && make run
```

Output: `nyonOS.img` (GPT disk image with Limine bootloader)

## Running

```bash
# With display
qemu-system-x86_64 -drive format=raw,file=nyonOS.img -serial stdio
```

## Disclaimer

This project uses LLM assistance. Some of the code in this repository was
written by an LLM (an AI coding assistant) rather than by a human.

Roughly 70% of the code is human-written and 30% is LLM-generated.

LLM-generated code has not necessarily been reviewed line by line by a human.
Read it before you rely on it. The project is provided as-is, with no warranty
of any kind.

## License

GPL-3.0-only (font data from `kbd` package: GPL-2.0-or-later; limine: BSD-2-Clause)

The license covers the LLM-generated portions as well as the human-written ones.
