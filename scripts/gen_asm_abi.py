#!/usr/bin/env python3
"""Generate assembler constants for the ring 3 tests from src/abi/syscalls.def
and src/abi/abi.h, so the asm tests can never drift from the C ABI."""

import re
import sys

ERRNOS = ('EPERM', 'ENOENT', 'ESRCH', 'EIO', 'EBADF', 'EAGAIN', 'EACCES',
          'EFAULT', 'ENOTDIR', 'EISDIR', 'EINVAL', 'EMFILE', 'ENOSPC', 'ENOSYS')

OPEN_FLAGS = ('O_RDONLY', 'O_WRONLY', 'O_RDWR', 'O_CREAT', 'O_TRUNC',
              'O_DIRECTORY', 'O_NONBLOCK')

FCNTL_CMDS = ('F_DUPFD', 'F_GETFD', 'F_SETFD', 'F_GETFL', 'F_SETFL')


def defines(path):
    out = {}
    for line in open(path):
        m = re.match(r'#define\s+(ABI_\w+)\s+\(?(-?(?:0x)?[0-9a-fA-F]+)\)?\s*$', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def rows(path):
    out = []
    for line in open(path):
        m = re.match(r'\s*X\(\s*(\w+)\s*,\s*(\d+)\s*,', line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


def main():
    def_path, abi_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    d = defines(abi_path)

    def v(name):
        return d.get('ABI_' + name)

    missing = [n for n in ERRNOS + OPEN_FLAGS + FCNTL_CMDS
               + ('SEEK_SET', 'SEEK_CUR', 'SEEK_END', 'MAX_NAME', 'FD_RIGHT_READ',
                  'KTTY_GETDEST', 'KTTY_SETDEST', 'KTTY_GETFG', 'KTTY_SETFG')
               if v(n) is None]
    if missing:
        sys.exit('gen_asm_abi: missing from abi.h: ' + ', '.join(missing))

    L = ['/* generated from syscalls.def and abi.h. do not edit. */', '']
    L += ['.set SYS_%s, %d' % (n.upper(), num) for n, num in rows(def_path)]
    L.append('')
    L += ['.set %s, %s' % (n, v(n)) for n in ERRNOS]
    L.append('')
    L += ['.set %s, %s' % (n, v(n)) for n in OPEN_FLAGS]
    L.append('')
    L += ['.set SEEK_SET, %s' % v('SEEK_SET'),
          '.set SEEK_CUR, %s' % v('SEEK_CUR'),
          '.set SEEK_END, %s' % v('SEEK_END')]
    L.append('')
    L += ['.set %s, %s' % (c, v(c)) for c in FCNTL_CMDS]
    L.append('')
    L += ['.set RIGHT_READ, %s' % v('FD_RIGHT_READ'),
          '.set KTTY_GETDEST, %s' % v('KTTY_GETDEST'),
          '.set KTTY_SETDEST, %s' % v('KTTY_SETDEST'),
          '.set KTTY_GETFG, %s' % v('KTTY_GETFG'),
          '.set KTTY_SETFG, %s' % v('KTTY_SETFG'),
          '.set SYS_MAX_NAME, %s' % v('MAX_NAME'),
          '']

    open(out_path, 'w').write('\n'.join(L))


main()
