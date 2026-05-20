#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FS_DIR="$PROJECT_DIR/filesystem"
KERNEL_DIR="$PROJECT_DIR/linux-6.12.90"
WORK_DIR="$FS_DIR/initramfs_root"
OUTPUT="$FS_DIR/initramfs.cpio.gz"

if [ ! -f "$FS_DIR/volkovfs.ko" ]; then
    echo "ОШИБКА: volkovfs.ko не найден в $FS_DIR"
    exit 1
fi

if [ ! -f "$FS_DIR/volkovfs_test" ]; then
    echo "ОШИБКА: volkovfs_test не найден в $FS_DIR"
    exit 1
fi

BUSYBOX=$(which busybox 2>/dev/null || echo "")
if [ -z "$BUSYBOX" ] || [ ! -f "$BUSYBOX" ]; then
    echo "ОШИБКА: busybox не найден."
    exit 1
fi

if ! file "$BUSYBOX" | grep -q "statically linked"; then
    if [ -f "/usr/bin/busybox" ] && file /usr/bin/busybox | grep -q "statically linked"; then
        BUSYBOX="/usr/bin/busybox"
    elif [ -f "/bin/busybox" ] && file /bin/busybox | grep -q "statically linked"; then
        BUSYBOX="/bin/busybox"
    else
        echo "ОШИБКА: нужен статически слинкованный busybox."
        exit 1
    fi
fi

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"/{bin,sbin,etc,proc,sys,dev,mnt/volkovfs,tmp,lib/modules}

cp "$BUSYBOX" "$WORK_DIR/bin/busybox"
chmod +x "$WORK_DIR/bin/busybox"

cd "$WORK_DIR/bin"
for cmd in sh ls cat echo mkdir mount umount insmod rmmod lsmod \
           dmesg mknod losetup dd cp mv rm sleep head tail wc \
           grep sed awk vi poweroff reboot; do
    ln -sf busybox "$cmd"
done
cd "$WORK_DIR/sbin"
ln -sf ../bin/busybox poweroff
ln -sf ../bin/busybox reboot
cd -

cp "$FS_DIR/volkovfs.ko" "$WORK_DIR/lib/modules/"
cp "$FS_DIR/volkovfs_test" "$WORK_DIR/bin/"
chmod +x "$WORK_DIR/bin/volkovfs_test"

cat > "$WORK_DIR/init" << 'INIT_EOF'
#!/bin/sh

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null

[ -e /dev/null ]    || mknod /dev/null c 1 3
[ -e /dev/zero ]    || mknod /dev/zero c 1 5
[ -e /dev/console ] || mknod /dev/console c 5 1
[ -e /dev/loop0 ]   || mknod /dev/loop0 b 7 0
[ -e /dev/loop1 ]   || mknod /dev/loop1 b 7 1

insmod /lib/modules/volkovfs.ko
if [ $? -ne 0 ]; then
    echo "ОШИБКА загрузки модуля volkovfs"
    dmesg | tail -20
    exec /bin/sh
fi

dd if=/dev/zero of=/tmp/volkovfs.img bs=1M count=8 2>/dev/null

losetup /dev/loop0 /tmp/volkovfs.img
if [ $? -ne 0 ]; then
    echo "ОШИБКА: losetup не удался. Проверьте CONFIG_BLK_DEV_LOOP в конфигурации ядра"
    exec /bin/sh
fi

mount -t volkovfs /dev/loop0 /mnt/volkovfs
if [ $? -ne 0 ]; then
    echo "ОШИБКА монтирования volkovfs"
    dmesg | tail -20
    exec /bin/sh
fi

echo ""
echo "volkovfs смонтирована в /mnt/volkovfs"
echo ""

exec /bin/sh
INIT_EOF

chmod +x "$WORK_DIR/init"

cd "$WORK_DIR"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$OUTPUT"
cd -

echo "initramfs создан: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"