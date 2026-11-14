#!/bin/bash
set -e
cd "$(dirname "$0")"

# Download and build Limine only if not already available
if [ ! -x "/tmp/limine-binary/limine" ]; then
    echo "Downloading and building Limine..."
    cd /tmp
    rm -rf limine-binary limine-binary.tar.gz
    curl -fL -o limine-binary.tar.gz https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.gz
    gunzip < limine-binary.tar.gz | tar -xf -
    make -C /tmp/limine-binary 2>/dev/null
    cd -
fi

echo "Creating bootable disk image..."
dd if=/dev/zero bs=1M count=64 of=nyonOS.img

awk 'BEGIN{printf "that my jarona"; for(i=0;i<2986;i++) printf "."}' > /tmp/nyonos-fs-test.txt
printf 'inner payload\n' > /tmp/nyonos-fs-inner.txt

EXT2_MB=${EXT2_MB:-64}
EXT2_STAGE=build/ext2-stage

rm -rf "$EXT2_STAGE" build/ext2.img
mkdir -p "$EXT2_STAGE/BIN" "$EXT2_STAGE/usr/share/terminfo" "$EXT2_STAGE/SUB" "$EXT2_STAGE/docs" "$EXT2_STAGE/empty"

cp /tmp/nyonos-fs-test.txt "$EXT2_STAGE/TEST.TXT"
cp /tmp/nyonos-fs-inner.txt "$EXT2_STAGE/SUB/INNER.TXT"
printf 'hello from ext2\nsecond line\n' > "$EXT2_STAGE/hello.txt"
printf 'nested file content\n' > "$EXT2_STAGE/docs/nested.txt"
printf 'deep\n' > "$EXT2_STAGE/docs/deep.txt"
head -c 200000 /dev/urandom > "$EXT2_STAGE/big.bin"
ln -s hello.txt "$EXT2_STAGE/link.txt"
ln -s docs/nested.txt "$EXT2_STAGE/docs/dlink.txt"

make -C src/user >/dev/null

PROGRAMS="hello fdtest mmtest forktest pipetest fstest fd2test evtest fbtest devtest"
if [ -f src/user/bin/systest ]; then
    PROGRAMS="$PROGRAMS systest"
fi
if [ -f src/user/bin/asynctest ]; then
    PROGRAMS="$PROGRAMS asynctest"
fi
if [ -f src/user/bin/mhello ]; then
    PROGRAMS="$PROGRAMS mhello"
fi
if [ -f src/user/bin/sdtest ]; then
    PROGRAMS="$PROGRAMS sdtest"
fi
if [ -f src/user/bin/thrtest ]; then
    PROGRAMS="$PROGRAMS thrtest"
fi
if [ -f src/user/bin/exectest ]; then
    PROGRAMS="$PROGRAMS exectest"
fi

if [ -f src/user/bin/fdprobe ]; then
    PROGRAMS="$PROGRAMS fdprobe"
fi

if [ -f src/user/bin/mandel ]; then
    PROGRAMS="$PROGRAMS mandel"
fi

if [ -f src/user/bin/featest ]; then
    PROGRAMS="$PROGRAMS featest"
fi

if [ -f src/user/bin/tlstest ]; then
    PROGRAMS="$PROGRAMS tlstest"
fi

if [ -f src/user/bin/proctest ]; then
    PROGRAMS="$PROGRAMS proctest"
fi

if [ -f src/user/bin/clktest ]; then
    PROGRAMS="$PROGRAMS clktest"
fi

if [ -f src/user/bin/sigtest ]; then
    PROGRAMS="$PROGRAMS sigtest"
fi

for prog in $PROGRAMS; do
    if [ ! -f "src/user/bin/$prog" ]; then
        echo "src/user/bin/$prog missing, cannot stage user programs" >&2
        exit 1
    fi
done

for prog in $PROGRAMS; do
    cp "src/user/bin/$prog" "$EXT2_STAGE/BIN/$prog"
done

BB_BIN=$NYONOS_BUSYBOX
if [ -z "$BB_BIN" ] && [ -f ../busybox/busybox ]; then
    BB_BIN=../busybox/busybox
fi

if [ -n "$BB_BIN" ] && [ -f "$BB_BIN" ]; then
    cp "$BB_BIN" "$EXT2_STAGE/BIN/busybox"
    BB_APPLETS="basename cat clear cmp comm cp cut dirname du echo env expand expr false find grep head hexdump hostname id install ln ls md5sum mkdir mktemp mv nl od printf readlink rev rm rmdir seq sha256sum shred sleep sort split stat strings sync tac tail tee test touch tr true uname unexpand uniq wc which xargs yes"
    for applet in $BB_APPLETS; do
        ln -sf busybox "$EXT2_STAGE/BIN/$applet"
    done
fi

if [ -f ../bash/bash ]; then
    cp ../bash/bash "$EXT2_STAGE/BIN/BASH"
fi

NANO_BIN=$NYONOS_NANO
if [ -z "$NANO_BIN" ] && [ -f tools/nano.bin ]; then
    NANO_BIN=tools/nano.bin
fi

if [ -n "$NANO_BIN" ] && [ -f "$NANO_BIN" ]; then
    cp "$NANO_BIN" "$EXT2_STAGE/BIN/NANO"
fi

TERMINFO_DIR=$NYONOS_TERMINFO
if [ -z "$TERMINFO_DIR" ] && [ -d tools/terminfo ]; then
    TERMINFO_DIR=tools/terminfo
fi

if [ -n "$TERMINFO_DIR" ] && [ -d "$TERMINFO_DIR" ]; then
    cp -r "$TERMINFO_DIR"/. "$EXT2_STAGE/usr/share/terminfo/" 2>/dev/null || true
fi

dd if=/dev/zero of=build/ext2.img bs=1M count=$EXT2_MB 2>/dev/null
mke2fs -q -t ext2 -b 1024 -I 128 -d "$EXT2_STAGE" \
    -O ^resize_inode,^dir_index,^filetype,^ext_attr,^sparse_super,^large_file,^huge_file,^dir_nlink,^extra_isize,^metadata_csum,^orphan_file,^64bit,^has_journal \
    build/ext2.img 2>/dev/null || mke2fs -q -t ext2 -b 1024 -I 128 -d "$EXT2_STAGE" build/ext2.img

PATH=$PATH:/usr/sbin:/sbin sgdisk nyonOS.img -n 1:2048 -t 1:ef00 -m 1

mformat -i nyonOS.img@@1M -T 126976 -h 64 -s 32 ::
mmd -i nyonOS.img@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine ::/limine 2>/dev/null

mcopy -i nyonOS.img@@1M bin/nyonOS ::/boot/
mcopy -i nyonOS.img@@1M limine.cfg ::/boot/limine/limine.conf
mcopy -i nyonOS.img@@1M limine.cfg ::/limine/limine.conf
mcopy -i nyonOS.img@@1M limine.cfg ::/boot/limine.conf
mcopy -i nyonOS.img@@1M /tmp/limine-binary/limine-bios.sys ::/boot/limine/
mcopy -i nyonOS.img@@1M /tmp/limine-binary/limine-bios.sys ::/boot/
mcopy -i nyonOS.img@@1M /tmp/limine-binary/limine-bios.sys ::/limine/
mcopy -i nyonOS.img@@1M /tmp/limine-binary/limine-bios.sys ::/
mcopy -i nyonOS.img@@1M /tmp/limine-binary/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i nyonOS.img@@1M /tmp/limine-binary/BOOTIA32.EFI ::/EFI/BOOT/

/tmp/limine-binary/limine bios-install nyonOS.img

echo "=== Build Complete ==="
echo "Boot with: qemu-system-x86_64 -drive format=raw,file=nyonOS.img -drive format=raw,file=build/ext2.img"
