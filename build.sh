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
echo "Boot with: qemu-system-x86_64 -drive format=raw,file=nyonOS.img"
