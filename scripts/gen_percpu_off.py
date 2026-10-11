#!/usr/bin/env python3
"""Emit the offset of struct percpu::syscall_kstack_top for syscall_entry.s.

The syscall fast path reads this field straight out of the per-cpu block in
assembly, so a stale constant would silently read the wrong field. Compiling a
probe instead of hardcoding it means the number cannot drift."""

import subprocess
import sys
import tempfile
import os

PROBE = '''
#include <stdio.h>
#include <stddef.h>
#include "arch/x86_64/cpu/cpu.h"
int main(void) {{
    printf("%zu %zu\\n", offsetof(struct percpu, syscall_kstack_top), sizeof(struct percpu));
    return 0;
}}
'''


def main():
    root, out_path = sys.argv[1], sys.argv[2]

    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'probe.c')
        exe = os.path.join(d, 'probe')
        with open(src, 'w') as f:
            f.write(PROBE)
        subprocess.run(['gcc', '-I' + root, '-o', exe, src], check=True)
        out = subprocess.check_output([exe]).decode().split()

    off, size = int(out[0]), int(out[1])

    if size % 64 != 0:
        sys.exit('gen_percpu_off: sizeof(struct percpu)=%d is not a cache line '
                 'multiple, per-cpu structs would false share' % size)

    with open(out_path, 'w') as f:
        f.write('/* generated from struct percpu. do not edit. */\n')
        f.write('.set PERCPU_SYSCALL_KSTACK_OFF, %d\n' % off)
        f.write('.set PERCPU_STRUCT_SIZE, %d\n' % size)


main()