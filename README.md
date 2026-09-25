# nyonOS

A minimal x86_64 hobby OS kernel written from scratch. Boots via Limine on BIOS/UEFI with VGA text output and serial debug logging.

## Features

- **Limine boot protocol** (revision 6) - works on BIOS and UEFI
- **Higher-half kernel** at `0xffffffff80000000` with ELF64 entry point `kmain`
- **VGA framebuffer rendering** with 8x16 font (from `kbd` package, GPL-2.0)
- **Serial debug output** (COM1, 115200 8N1)
- **PIT timer** (channel 0, mode 2 rate generator) at 1000 Hz driving IRQ0`

## Building

```bash
make clean && make image
```

Output: `nyonOS.img` (GPT disk image with Limine bootloader)

## Running

```bash
# With display
qemu-system-x86_64 -drive format=raw,file=nyonOS.img -serial stdio
```

## License

GPL-3.0-only (font data from `kbd` package: GPL-2.0-or-later).
