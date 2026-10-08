CC = gcc
LD = ld
CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC -ffunction-sections -fdata-sections -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel -Isrc
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T linker.ld -no-pie

SRCFILES = $(filter-out $(EXCLUDE),$(shell find -L src -type f 2>/dev/null | LC_ALL=C sort))
CFILES = $(filter %.c,$(SRCFILES))
SFILES = $(filter %.s,$(SRCFILES))
SFILES_S = $(filter %.S,$(SRCFILES))
OBJ = $(addprefix obj/,$(CFILES:.c=.c.o) $(SFILES:.s=.s.o) $(SFILES_S:.S=.S.o))
DEP = $(OBJ:.o=.d)
DEPFLAGS = -MMD -MP
OUTPUT := nyonOS
QEMU_CPU = qemu64,+nx,+smep,+smap,+pcid,+rdrand,+rdseed,+fsgsbase,+invpcid

EXCLUDE = $(shell find -L src/user -type f 2>/dev/null)

ASM_ABI = obj/syscalls_asm.h

$(ASM_ABI): src/abi/syscalls.def src/abi/abi.h scripts/gen_asm_abi.py
	mkdir -p obj
	python3 scripts/gen_asm_abi.py src/abi/syscalls.def src/abi/abi.h $@

.PHONY: all kernel image run clean

all: kernel

kernel: bin/$(OUTPUT)

bin/$(OUTPUT): $(OBJ) linker.ld
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

obj/%.c.o: %.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

obj/%.s.o: %.s $(ASM_ABI)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -x assembler-with-cpp -Iobj -c $< -o $@

obj/%.S.o: %.S
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

image: kernel user $(ASM_ABI)
	./build.sh

user:
	$(MAKE) -C src/user

run:
	$(MAKE) clean
	$(MAKE) kernel
	./build.sh
	qemu-system-x86_64 -cpu $(QEMU_CPU) -drive format=raw,file=nyonOS.img -drive format=raw,file=fat16.img -d int,cpu_reset -D qemu.log -serial stdio -no-reboot -k en-us

run-ihatedisplay: image
	qemu-system-x86_64 -cpu $(QEMU_CPU) -drive format=raw,file=nyonOS.img -drive format=raw,file=fat16.img -d int,cpu_reset -D qemu.log -serial stdio -no-reboot -k en-us -display none

clean:
	rm -rf bin obj
	$(MAKE) -C src/user clean

.PHONY: all kernel image run run-ihatedisplay clean

-include $(DEP)
