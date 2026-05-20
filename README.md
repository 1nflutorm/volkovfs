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
