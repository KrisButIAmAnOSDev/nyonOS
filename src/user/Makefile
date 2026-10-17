MLIBC_CC = $(NYONOS_MLIBC_CC)
MLIBC_LD = $(NYONOS_MLIBC_LD)

USER_CC = gcc
USER_CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check \
              -fno-builtin -fPIE -m64 -mno-red-zone -mcmodel=small \
              -fomit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables -I. -I$(CURDIR)/../abi
USER_LDFLAGS = -nostdlib -static-pie -pie -z noexecstack -T user.ld

BINS = bin/hello bin/fdtest bin/mmtest bin/forktest

ifneq ($(NYONOS_MLIBC_CC),)
BINS += bin/mhello bin/sdtest bin/thrtest bin/clktest bin/proctest bin/featest bin/mandel
endif

-include $(BINS:=.o)

all: $(BINS)

bin/%: %.o crt0.o user.ld
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_LDFLAGS) $(filter %.o $%,$^) -o $@

bin/mhello: mlibc_hello.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

bin/sdtest: sdtest.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

bin/thrtest: thrtest.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

bin/clktest: clktest.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

bin/proctest: proctest.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

bin/featest: featest.c $(MLIBC_LD)
	mkdir -p "$(dir $@)"
	$(MLIBC_CC) -static -T$(MLIBC_LD) -z max-page-size=0x1000 -s $< -o $@

bin/mandel: mandel.c $(MLIBC_LD)
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