#!/bin/bash
set -e

TC=/home/quan/kawkaw/nyonos-toolchain
export PATH="$TC/toolchain/usr/bin:$PATH"

cd "$TC/src/binutils-2.39/build"

make -j6 all

DESTDIR="$TC/toolchain" make install

echo
echo "=== installed ==="
ls "$TC/toolchain/usr/bin/"
echo
x86_64-nyonos-ld -V | grep -A2 "Supported emulations" | head -4