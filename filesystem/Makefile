# ============================================================
# Makefile для проекта volkovfs
# Автор: Волков
#
# Цели:
#   make module      — сборка модуля ядра (volkovfs.ko)
#   make test        — сборка тестовой программы
#   make all         — всё вместе
#   make clean       — очистка
# ============================================================

# Путь до исходников ядра (можно переопределить: make KDIR=...)
KDIR ?= /home/1nflutorm/Downloads/Linux/linux-6.12.90

# Имя модуля
obj-m := volkovfs.o

# Компилятор для userspace
CXX := g++
CXXFLAGS := -static -Wall -std=c++17 -O2

# ===== Цели =====

.PHONY: all module test clean

all: module test

# Сборка модуля ядра
module:
	@echo "=== Сборка модуля volkovfs.ko ==="
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "=== Готово: volkovfs.ko ==="

# Сборка тестовой программы (статическая линковка для переносимости)
test: volkovfs_test
	@echo "=== Готово: volkovfs_test ==="

volkovfs_test: volkovfs_test.cpp volkovfs.h
	@echo "=== Сборка volkovfs_test ==="
	$(CXX) $(CXXFLAGS) -o $@ $<

# Очистка
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f volkovfs_test
	@echo "=== Очищено ==="