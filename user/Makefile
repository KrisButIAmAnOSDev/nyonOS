USER_CC = gcc
USER_CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check \
              -fno-builtin -fPIE -m64 -mno-red-zone -mcmodel=small \
              -fno-asynchronous-unwind-tables -fno-unwind-tables -Isrc/.. -I.
USER_LDFLAGS = -nostdlib -static-pie -pie -z noexecstack -z separate-code -T user.ld

BINS = bin/hello

all: $(BINS)

bin/hello: hello.o lib/sys.o crt0.o user.ld
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_LDFLAGS) $(filter %.o $%,$^) -o $@

%.o: %.c
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

crt0.o: crt0.S
	mkdir -p "$(dir $@)"
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

clean:
	rm -f bin/hello hello.o lib/sys.o crt0.o

.PHONY: all clean
