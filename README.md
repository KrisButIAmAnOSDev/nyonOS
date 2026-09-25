# nyonOS

A minimal x86_64 hobby OS kernel written from scratch. Boots via Limine on BIOS/UEFI with VGA text output and serial debug logging.

## Features

- **Limine boot protocol** (revision 6) - works on BIOS and UEFI
- **Higher-half kernel** at `0xffffffff80000000` with ELF64 entry point `kmain`
- **VGA framebuffer rendering** with 8x16 font (from `kbd` package, GPL-2.0)
- **Serial debug output** (COM1, 115200 8N1)
- **IDT with 256 entries** - 32 CPU exceptions + 16 IRQs remapped to vectors 0x20-0x2F
- **8259 PIC remapping** (master 0x20-0x27, slave 0x28-0x2F) before `sti`
- **PIT timer** (channel 0, mode 2 rate generator) at 1000 Hz driving IRQ0
- **Panic screen** with full register dump on both serial and framebuffer
- **No stdlib** - freestanding, `-nostdlib`, `-mcmodel=kernel`

## Building

```bash
make clean && make image
```

Output: `nyonOS.img` (GPT disk image with Limine bootloader)

## Running

```bash
# Serial only (headless)
qemu-system-x86_64 -drive format=raw,file=nyonOS.img -serial stdio -display none

# With display
qemu-system-x86_64 -drive format=raw,file=nyonOS.img -serial stdio
```

## Boot Output

```
nyonOS: Kernel loaded!
nyonOS: Limine revision OK!
nyonOS: Framebuffer found!
nyonOS: Framebuffer format OK!
PIC remapped to 032-040
PIT divisor: 0x04a9
PIC1 mask after unmask IRQ0: 0x00
PIT initialized at 1000Hz
IDT loaded!
nyonOS: Drawing complete!
```

## Architecture

```
src/
├── arch/x86_64/
│   ├── idt/          # Interrupt Descriptor Table
│   │   ├── idt.c/h   # IDT setup, descriptors
│   │   ├── isr.s     # Assembly ISR stubs (vectors 0-47)
│   │   ├── idt_load.s # lidt wrapper
│   │   └── get_stub_table.s # RIP-relative-free stub table access
│   ├── pic/          # 8259 PIC driver
│   │   ├── pic.c/h   # Remap, mask, EOI
│   └── pit/          # 8254 PIT driver
│       ├── pit.c/h   # Timer init, sleep, tick handler
├── kernel/
│   ├── panic.c/h     # Panic screen with register dump
├── io/serial/        # COM1 serial driver
├── video/            # Framebuffer text rendering
├── font/             # 8x16 font data
├── limine/           # Limine protocol headers
└── kernel.c          # Entry point (kmain)
```

## Key Implementation Details

- **No RIP-relative addressing for higher-half symbols** - uses `movabs` in assembly (`get_stub_table.s`, `idt_load.s`)
- **PIC remapped before `sti`** - avoids IRQ0-15 colliding with CPU exception vectors 0-15
- **PIT programmed before IDT** - timer ready when interrupts enabled
- **ISR reads vector from stack frame** (`[rsp + 16*8 + 8]`) not saved registers
- **Panic dumps all registers** (RAX-R15, RIP, CS, RFLAGS, RSP, CR0-CR4) to serial + framebuffer

## License

GPL-3.0-only (font data from `kbd` package: GPL-2.0-or-later).