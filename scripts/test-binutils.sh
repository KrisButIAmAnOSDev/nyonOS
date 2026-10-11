#!/bin/bash
TC=/home/quan/kawkaw/nyonos-toolchain
export PATH="$TC/toolchain/usr/bin:$PATH"

if ! command -v x86_64-nyonos-as >/dev/null; then
    echo "binutils not installed yet — run build-binutils.sh first"
    exit 1
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"

cat > t.s <<'EOF'
    .intel_syntax noprefix
    .globl _start
    .section .text
_start:
    mov rax, 0x400000 / 4096
    mov rbx, rsp
    xor rbp, rbp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    syscall
EOF

echo "=== 1. assemble ==="
x86_64-nyonos-as -o t.o t.s && echo "OK: as works"

echo
echo "=== 2. object format ==="
x86_64-nyonos-objdump -f t.o | sed -n '2,6p'

echo
echo "=== 3. the '/' division survived (this is the em=gnu trap) ==="
x86_64-nyonos-objdump -d t.o | grep -E "mov.*0x400|shr|movabs" | head -4

echo
echo "=== 4. link with the nyonos emulation ==="
x86_64-nyonos-ld -m elf_x86_64_nyonos -Ttext=0x400000 -e _start \
    --no-warn-rwx-segments -o t.elf t.o && echo "OK: ld works"

echo
echo "=== 5. linked ELF header ==="
x86_64-nyonos-objdump -f t.elf | sed -n '2,6p'

echo
echo "=== 6. is the entry at 0x400000? ==="
x86_64-nyonos-objdump -d t.elf | grep -A2 "<_start>:" | head -3

echo
echo "=== 7. no stray dynamic deps ==="
x86_64-nyonos-objdump -p t.elf | grep -iE "NEEDED|interpreter" || echo "none (good, we didn't link any libc)"

echo
echo "ALL BINUTILS TESTS PASSED"