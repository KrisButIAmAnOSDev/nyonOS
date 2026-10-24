#!/bin/sh
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${BUSYBOX_SRC:-$ROOT/../busybox}
TOOLCHAIN=${NYONOS_TOOLCHAIN:-$ROOT/../nyonos-toolchain}
CROSS=$TOOLCHAIN/toolchain16/bin/x86_64-nyonos-
COMPAT=$ROOT/tools/busybox-compat/include

if [ ! -d "$SRC/.git" ]; then
    echo "busybox: cloning into $SRC"
    git clone --depth 1 https://github.com/mirror/busybox.git "$SRC"
fi

cd "$SRC"

if [ ! -f .config ] || [ "$ROOT/tools/busybox.config" -nt .config ]; then
    cp "$ROOT/tools/busybox.config" .config
fi

PATH=$TOOLCHAIN/toolchain16/bin:$PATH
export PATH

make ARCH=x86_64 CROSS_COMPILE="$CROSS" oldconfig < /dev/null > /dev/null

if [ -f busybox ] && [ busybox -nt .config ]; then
    echo "busybox: up to date"
    exit 0
fi

rm -f busybox busybox_unstripped
make ARCH=x86_64 CROSS_COMPILE="$CROSS" \
     EXTRA_CFLAGS="-I$COMPAT" -j4
