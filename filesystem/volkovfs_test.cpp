#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "volkovfs.h"

static std::string file_path(const std::string &mount_point, unsigned int idx)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/file_%04u", mount_point.c_str(), idx);
    return std::string(buf);
}

static std::string generate_test_data(unsigned int idx)
{
    std::ostringstream oss;
    oss << "=== volkovfs test data ===\n";
    oss << "File index: " << idx << "\n";
    oss << "Pattern: ";
    for (int i = 0; i < 20; i++)
    {
        oss << static_cast<char>('A' + ((idx + i) % 26));
    }
    oss << "\nChecksum seed: " << (idx * 31337 + 12345) << "\n";
    oss << "=== end ===\n";
    return oss.str();
}

static unsigned int count_files(const std::string &mount_point)
{
    DIR *dir = opendir(mount_point.c_str());
    if (!dir)
    {
        return 0;
    }

    unsigned int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        count++;
    }

    closedir(dir);
    return count;
}

static int cmd_test(const std::string &mount_point)
{
    unsigned int total_files = count_files(mount_point);
    if (total_files == 0)
    {
        std::cerr << "Ошибка: файлы не найдены в " << mount_point << "\n";
        return 1;
    }

    int passed = 0;
    int failed = 0;

    std::cout << "=== volkovfs: тестирование записи/чтения ===\n";
    std::cout << "Файлов для проверки: " << total_files << "\n\n";

    for (unsigned int idx = 0; idx < total_files; idx++)
    {
        std::string path = file_path(mount_point, idx);
        std::string test_data = generate_test_data(idx);

        int fd_w = open(path.c_str(), O_WRONLY | O_TRUNC);
        if (fd_w < 0)
        {
            std::cerr << "  [FAIL] file_" << idx
                      << " — не удалось открыть для записи: "
                      << strerror(errno) << "\n";
            failed++;
            continue;
        }
        ssize_t written = write(fd_w, test_data.c_str(), test_data.size());
        close(fd_w);

        if (written != (ssize_t)test_data.size())
        {
            std::cerr << "  [FAIL] file_" << idx
                      << " — записано " << written
                      << " из " << test_data.size() << " байт\n";
            failed++;
            continue;
        }

        int fd_r = open(path.c_str(), O_RDONLY);
        if (fd_r < 0)
        {
            std::cerr << "  [FAIL] file_" << idx
                      << " — не удалось открыть для чтения: "
                      << strerror(errno) << "\n";
            failed++;
            continue;
        }

        char read_buf[4096];
        memset(read_buf, 0, sizeof(read_buf));
        ssize_t bytes_read = read(fd_r, read_buf, sizeof(read_buf));
        close(fd_r);

        if (bytes_read < 0)
        {
            std::cerr << "  [FAIL] file_" << idx
                      << " — ошибка чтения: " << strerror(errno) << "\n";
            failed++;
            continue;
        }

        std::string read_data(read_buf, bytes_read);
        if (read_data == test_data)
        {
            passed++;
        }
        else
        {
            std::cerr << "  [FAIL] file_" << idx
                      << " — данные не совпадают (ожидалось " << test_data.size()
                      << " байт, прочитано " << bytes_read << ")\n";
            failed++;
        }
    }

    std::cout << "\n=== Результат: " << passed << " PASS, "
              << failed << " FAIL из " << total_files << " ===\n";

    return (failed > 0) ? 1 : 0;
}

static int cmd_hashes(const std::string &mount_point)
{
    int fd = open(mount_point.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        std::cerr << "Ошибка: не удалось открыть " << mount_point
                  << ": " << strerror(errno) << "\n";
        return 1;
    }

    struct volkovfs_hashes_arg ha;
    memset(&ha, 0, sizeof(ha));
    ha.start = 0;
    ha.count = VOLKOVFS_MAX_HASHES;

    int ret = ioctl(fd, VOLKOVFS_IOC_HASHES, &ha);
    if (ret < 0)
    {
        std::cerr << "Ошибка IOCTL HASHES: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    std::cout << "=== volkovfs: хеши файлов ===\n\n";
    std::cout << "Получено " << ha.count << " хешей:\n\n";

    for (uint32_t i = 0; i < ha.count && i < 50; i++)
    {
        printf("  file_%04u: 0x%08X\n", ha.start + i, ha.hashes[i]);
    }

    if (ha.count > 50)
    {
        std::cout << "  ... (показаны первые 50)\n";
    }

    close(fd);
    return 0;
}

static int cmd_mapping(const std::string &mount_point, unsigned int file_index)
{
    int fd = open(mount_point.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        std::cerr << "Ошибка: не удалось открыть " << mount_point
                  << ": " << strerror(errno) << "\n";
        return 1;
    }

    struct volkovfs_mapping_arg ma;
    memset(&ma, 0, sizeof(ma));
    ma.file_index = file_index;

    int ret = ioctl(fd, VOLKOVFS_IOC_MAPPING, &ma);
    if (ret < 0)
    {
        std::cerr << "Ошибка IOCTL MAPPING: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    std::cout << "=== volkovfs: маппинг файла ===\n\n";
    printf("  Файл:             file_%04u\n", ma.file_index);
    printf("  Начальный сектор:  %u\n", ma.start_sector);
    printf("  Кол-во секторов:   %u\n", ma.num_sectors);
    printf("  Начало данных:     сектор %u\n", ma.data_start);
    printf("  Размер данных:     %u байт\n", ma.num_sectors * VOLKOVFS_SECTOR_SIZE);
    printf("  Смещение на диске: %u байт (0x%X)\n",
           ma.start_sector * VOLKOVFS_SECTOR_SIZE,
           ma.start_sector * VOLKOVFS_SECTOR_SIZE);

    close(fd);
    return 0;
}

static int cmd_zero(const std::string &mount_point)
{
    int fd = open(mount_point.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        std::cerr << "Ошибка: не удалось открыть " << mount_point
                  << ": " << strerror(errno) << "\n";
        return 1;
    }

    int ret = ioctl(fd, VOLKOVFS_IOC_ZERO);
    if (ret < 0)
    {
        std::cerr << "Ошибка IOCTL ZERO: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    std::cout << "volkovfs: все файлы обнулены\n";

    close(fd);
    return 0;
}

static int cmd_erase(const std::string &mount_point)
{
    int fd = open(mount_point.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        std::cerr << "Ошибка: не удалось открыть " << mount_point
                  << ": " << strerror(errno) << "\n";
        return 1;
    }

    int ret = ioctl(fd, VOLKOVFS_IOC_ERASE);
    if (ret < 0)
    {
        std::cerr << "Ошибка IOCTL ERASE: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    std::cout << "volkovfs: ФС полностью сброшена\n";

    close(fd);
    return 0;
}

static int cmd_info(const std::string &mount_point)
{
    DIR *dir = opendir(mount_point.c_str());
    if (!dir)
    {
        std::cerr << "Ошибка: не удалось открыть каталог "
                  << mount_point << ": " << strerror(errno) << "\n";
        return 1;
    }

    unsigned int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        count++;
    }

    std::cout << "=== volkovfs: информация о ФС ===\n\n";
    std::cout << "Точка монтирования: " << mount_point << "\n";
    std::cout << "Файлов в ФС: " << count << "\n";

    rewinddir(dir);
    unsigned int shown = 0;
    while ((entry = readdir(dir)) != nullptr && shown < 5)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        std::cout << "  " << entry->d_name << "\n";
        shown++;
    }
    if (count > 5)
    {
        std::cout << "  ... (и еще " << (count - 5) << " файлов)\n";
    }

    closedir(dir);
    return 0;
}

static void print_usage(const char *prog)
{
    std::cout << "volkovfs_test — тестовая утилита для volkovfs\n\n";
    std::cout << "Использование:\n";
    std::cout << "  " << prog << " <точка_монтирования> <команда> [аргументы]\n\n";
    std::cout << "Команды:\n";
    std::cout << "  info           — информация о ФС\n";
    std::cout << "  test           — тест записи/чтения\n";
    std::cout << "  hashes         — вывод хешей файлов\n";
    std::cout << "  mapping <N>    — маппинг файла N\n";
    std::cout << "  zero           — обнуление всех файлов\n";
    std::cout << "  erase          — полный сброс ФС\n";
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        print_usage(argv[0]);
        return 1;
    }

    std::string mount_point = argv[1];
    std::string command = argv[2];

    if (command == "test")
    {
        return cmd_test(mount_point);
    }
    else if (command == "info")
    {
        return cmd_info(mount_point);
    }
    else if (command == "hashes")
    {
        return cmd_hashes(mount_point);
    }
    else if (command == "mapping")
    {
        if (argc < 4)
        {
            std::cerr << "Ошибка: команда mapping требует номер файла\n";
            std::cerr << "Пример: " << argv[0] << " /mnt mapping 0\n";
            return 1;
        }
        unsigned int idx = (unsigned int)atoi(argv[3]);
        return cmd_mapping(mount_point, idx);
    }
    else if (command == "zero")
    {
        return cmd_zero(mount_point);
    }
    else if (command == "erase")
    {
        return cmd_erase(mount_point);
    }
    else
    {
        std::cerr << "Неизвестная команда: " << command << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
}