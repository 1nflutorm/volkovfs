# volkovfs

Модуль файловой системы для ядра Linux 6.12.90.

Студент: Волков Иван Андреевич

Группа: 1303

## Структура проекта

```
Linux/
├── filesystem/
│   ├── volkovfs.h
│   ├── volkovfs.c
│   ├── volkovfs_test.cpp
│   ├── Makefile
│   ├── build_initramfs.sh
│   └── start.sh
└── linux-6.12.90/
```

## Зависимости

```
sudo pacman -S base-devel bc qemu-full cpio busybox
```

## Сборка ядра

```
cd linux-6.12.90
make defconfig
make menuconfig
make -j 6
cd ..
```

## Сборка модуля и тестовой программы

```
cd filesystem
make all KDIR=$(pwd)/../linux-6.12.90
```

## Создание initramfs

```
chmod +x build_initramfs.sh
./build_initramfs.sh
```

## Запуск

```
chmod +x start.sh
./start.sh
```

## Тестирование файловой системы

```
volkovfs_test /mnt/volkovfs test
volkovfs_test /mnt/volkovfs info
volkovfs_test /mnt/volkovfs hashes
volkovfs_test /mnt/volkovfs mapping 0
volkovfs_test /mnt/volkovfs zero
volkovfs_test /mnt/volkovfs erase
```

## Логи модуля

```
dmesg | grep volkovfs
```

## Исправления в соответствии с замечаниями

### Ошибка монтирования при повреждении суперблоков

После запуска фс сразу смонтирована
```
umount /mnt/volkovfs
```

Повреждаем оба суперблока

```
dd if=/dev/urandom of=/tmp/volkovfs.img bs=512 seek=0  count=1 conv=notrunc
dd if=/dev/urandom of=/tmp/volkovfs.img bs=512 seek=10 count=1 conv=notrunc
```

Пересоздаем loop
```
losetup -d /dev/loop0
losetup /dev/loop0 /tmp/volkovfs.img
```

Пытаемся смонироваться после поврежения суперблоков
```
mount -t volkovfs /dev/loop0 /mnt/volkovfs
```

Получаем ошибку монтирования. При повреждении одного суперблока идет воостановление из второго

### Не удаляются файлы после erase в точке монтирования

Проверяем файлы сразу после запуска

```
ls /mnt/volkovfs //пусто
```

Для пример запишем в два файла

```
echo "hello" > /mnt/volkovfs/file_0000
echo "world" > /mnt/volkovfs/file_0003
```

Проверяем

```
ls /mnt/volkovfs //file_0000  file_0003
```

Делаем erase 

```
volkovfs_test /mnt/volkovfs erase
```

Проверяем после erase

```
ls /mnt/volkovfs //пусто
```
