#!/bin/bash
# Clone fastfetch, apply the nyonOS patch, build it, and print the binary path.
# No external checkout or manual setup is needed: make image calls this.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
SRC="$ROOT/build/ports/fastfetch"
PATCH="$HERE/fastfetch-nyonos.patch"
BIN="$HERE/fastfetch.bin"
TOOLCHAIN="${NYONOS_TOOLCHAIN:-$ROOT/../nyonos-toolchain/toolchain16}"
SYSROOT="${NYONOS_SYSROOT:-$ROOT/../nyonos-toolchain/sysroot}"
LDSCRIPT="${NYONOS_LDSCRIPT:-$ROOT/../mlibc/sysdeps/nyonos/nyonos.ld}"

if [ ! -x "$TOOLCHAIN/bin/x86_64-nyonos-gcc" ]; then
    echo "fastfetch: no nyonOS toolchain at $TOOLCHAIN" >&2
    exit 1
fi

if [ ! -f "$PATCH" ]; then
    echo "fastfetch: missing patch $PATCH" >&2
    exit 1
fi

if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$ROOT/build/ports"
    rm -rf "$SRC"
    echo "fastfetch: cloning 2.33.0 into build/ports/fastfetch" >&2
    git clone --quiet --depth 1 --branch 2.33.0 \
        https://github.com/fastfetch-cli/fastfetch.git "$SRC"
fi

if [ ! -f "$SRC/.nyonos-patched" ]; then
    echo "fastfetch: applying nyonOS patch" >&2
    (cd "$SRC" && git apply --whitespace=nowarn "$PATCH")
    touch "$SRC/.nyonos-patched"
fi

if [ ! -f "$BIN" ] || [ "$PATCH" -nt "$BIN" ]; then
    export PATH="$TOOLCHAIN/bin:$PATH"
    rm -rf "$SRC/build"
    mkdir -p "$SRC/build"
    cd "$SRC/build"
    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$SRC/CMakeToolchain-nyonos.txt" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNYONOS_TOOLCHAIN="$TOOLCHAIN" \
        -DNYONOS_SYSROOT="$SYSROOT" \
        -DNYONOS_LDSCRIPT="$LDSCRIPT" \
        -DBUILD_TESTS=no \
        -DBUILD_FLASHFETCH=no \
        -DINSTALL_LICENSE=no \
        -DENABLE_VULKAN=no -DENABLE_WAYLAND=no -DENABLE_XCB_RANDR=no \
        -DENABLE_XRANDR=no -DENABLE_DRM=no -DENABLE_GIO=no -DENABLE_DCONF=no \
        -DENABLE_DBUS=no -DENABLE_DDCUTIL=no -DENABLE_SQLITE3=no \
        -DENABLE_EGL=no -DENABLE_GLX=no -DENABLE_OSMESA=no -DENABLE_OPENCL=no \
        -DENABLE_PULSE=no -DENABLE_IMAGEMAGICK6=no -DENABLE_IMAGEMAGICK7=no \
        -DENABLE_CHAFA=no -DENABLE_ELF=no -DENABLE_FREETYPE=no \
        -DENABLE_ZLIB=no -DENABLE_RPM=no -DENABLE_SYSTEM_YYJSON=no \
        >/dev/null 2>&1
    cmake --build . --target fastfetch -j"$(nproc)" >/dev/null 2>&1
    "$TOOLCHAIN/bin/x86_64-nyonos-strip" fastfetch -o "$BIN"
fi

echo "$BIN"
