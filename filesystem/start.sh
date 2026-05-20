#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL="$PROJECT_DIR/linux-6.12.90/arch/x86/boot/bzImage"
INITRAMFS="$SCRIPT_DIR/initramfs.cpio.gz"

if [ ! -f "$KERNEL" ]; then
    echo "ОШИБКА: bzImage не найден: $KERNEL"
    exit 1
fi

if [ ! -f "$INITRAMFS" ]; then
    echo "ОШИБКА: initramfs не найден: $INITRAMFS"
    exit 1
fi

qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 rdinit=/init panic=1" \
    -nographic \
    -m 256M \
    -smp 2 \
    -no-reboot