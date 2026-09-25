CC = gcc
LD = ld
CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC -ffunction-sections -fdata-sections -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel -Isrc
LDFLAGS = -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T linker.ld -no-pie

SRCFILES = $(shell find -L src -type f 2>/dev/null | LC_ALL=C sort)
CFILES = $(filter %.c,$(SRCFILES))
SFILES = $(filter %.s,$(SRCFILES))
SFILES_S = $(filter %.S,$(SRCFILES))
OBJ = $(addprefix obj/,$(CFILES:.c=.c.o) $(SFILES:.s=.s.o) $(SFILES_S:.S=.S.o))
OUTPUT := nyonOS

.PHONY: all kernel image run clean

all: kernel

kernel: bin/$(OUTPUT)

bin/$(OUTPUT): $(OBJ) linker.ld
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

obj/%.c.o: %.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.c.o: %.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.s.o: %.s
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.S.o: %.S
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

image: kernel
	./build.sh

run: image

clean:
	rm -rf bin obj

.PHONY: all kernel image run clean
