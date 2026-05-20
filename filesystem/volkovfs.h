#ifndef _VOLKOVFS_H
#define _VOLKOVFS_H

#ifdef __KERNEL__
    #include <linux/ioctl.h>
    #include <linux/types.h>
#else
    #include <sys/ioctl.h>
    #include <stdint.h>
    typedef uint32_t __u32;
    typedef uint32_t __le32;
#endif

#define VOLKOVFS_MAGIC          0x564F4C4B
#define VOLKOVFS_VERSION        1
#define VOLKOVFS_SECTOR_SIZE    512
#define VOLKOVFS_MAX_HASHES     128

struct volkovfs_disk_sb 
{
    __le32 magic;
    __le32 version;
    __le32 total_files;
    __le32 max_file_sectors;
    __le32 max_name_len;
    __le32 data_start_sector;
    __le32 total_sectors;
    __le32 checksum;
};

struct volkovfs_hashes_arg 
{
    __u32 start;
    __u32 count;
    __u32 hashes[VOLKOVFS_MAX_HASHES];
};

struct volkovfs_mapping_arg 
{
    __u32 file_index;
    __u32 start_sector;
    __u32 num_sectors;
    __u32 data_start;
};

#define VOLKOVFS_IOC_MAGIC   'V'
#define VOLKOVFS_IOC_HASHES  _IOWR(VOLKOVFS_IOC_MAGIC, 1, struct volkovfs_hashes_arg)
#define VOLKOVFS_IOC_MAPPING _IOWR(VOLKOVFS_IOC_MAGIC, 2, struct volkovfs_mapping_arg)
#define VOLKOVFS_IOC_ZERO    _IO(VOLKOVFS_IOC_MAGIC, 3)
#define VOLKOVFS_IOC_ERASE   _IO(VOLKOVFS_IOC_MAGIC, 4)

#endif // _VOLKOVFS_H 