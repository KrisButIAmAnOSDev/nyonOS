MLIBC_CC = /home/quan/kawkaw/nyonos-toolchain/toolchain16/bin/x86_64-nyonos-gcc
MLIBC_LD = /home/quan/kawkaw/mlibc/sysdeps/nyonos/nyonos.ld

USER_CC = gcc
USER_CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check \
              -fno-builtin -fPIE -m64 -mno-red-zone -mcmodel=small \
              -fomit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables -I. -I$(CURDIR)/../abi
USER_LDFLAGS = -nostdlib -static-pie -pie -z noexecstack -z separate-code -T user.ld

BINS = bin/hello bin/fdtest bin/mmtest bin/mhello

-include $(BINS:=.o)

all: $(BINS)

bin/%: %.o crt0.o user.ld
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_LDFLAGS) $(filter %.o $%,$^) -o $@

bin/mhello: mlibc_hello.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

%.o: %.c
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

crt0.o: crt0.S
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

clean:
	rm -rf bin *.o *.d

.PHONY: all clean