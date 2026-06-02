#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/blkdev.h>
#include <linux/time.h>
#include <linux/dcache.h>

#include "volkovfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Volkov Ivan 1303");
MODULE_DESCRIPTION("Simple File System (volkovfs)");
MODULE_VERSION("1.0");

static int sb_offset1 = 0;
static int sb_offset2 = 10;
static int max_file_sectors = 4;
static int max_name_len = 32;

module_param(sb_offset1, int, 0444);
module_param(sb_offset2, int, 0444);
module_param(max_file_sectors, int, 0444);
module_param(max_name_len, int, 0444);

MODULE_PARM_DESC(sb_offset1, "Смещение первой копии суперблока (в секторах)");
MODULE_PARM_DESC(sb_offset2, "Смещение второй копии суперблока (в секторах)");
MODULE_PARM_DESC(max_file_sectors, "Количество секторов, отведенных под каждый файл");
MODULE_PARM_DESC(max_name_len, "Максимальная длина имени файла");

#define VOLKOVFS_ROOT_INO       1
#define VOLKOVFS_FILE_INO_BASE  2

struct volkovfs_sb_info 
{
    uint32_t total_files;
    uint32_t max_file_sectors;
    uint32_t max_name_len;
    uint32_t data_start;
    uint32_t total_sectors;
    uint32_t *file_sizes;
};

static int volkovfs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry *volkovfs_mount(struct file_system_type *fs_type, int flags,
                                     const char *dev_name, void *data);
static void volkovfs_kill_sb(struct super_block *sb);
static struct inode *volkovfs_get_inode(struct super_block *sb, unsigned long ino);
static int volkovfs_iterate(struct file *filp, struct dir_context *ctx);
static struct dentry *volkovfs_lookup(struct inode *dir, struct dentry *dentry,
                                      unsigned int flags);
static ssize_t volkovfs_file_read(struct file *filp, char __user *buf,
                                   size_t len, loff_t *ppos);
static ssize_t volkovfs_file_write(struct file *filp, const char __user *buf,
                                    size_t len, loff_t *ppos);
static long volkovfs_dir_ioctl(struct file *filp, unsigned int cmd,
                                unsigned long arg);

static uint32_t volkovfs_compute_checksum(struct volkovfs_disk_sb *dsb)
{
    uint32_t sum = 0;
    sum ^= le32_to_cpu(dsb->magic);
    sum ^= le32_to_cpu(dsb->version);
    sum ^= le32_to_cpu(dsb->total_files);
    sum ^= le32_to_cpu(dsb->max_file_sectors);
    sum ^= le32_to_cpu(dsb->max_name_len);
    sum ^= le32_to_cpu(dsb->data_start_sector);
    sum ^= le32_to_cpu(dsb->total_sectors);

    return sum;
}

static int volkovfs_sb_is_blank(const struct volkovfs_disk_sb *dsb)
{
    const unsigned char *p = (const unsigned char *)dsb;
    size_t i;

    for (i = 0; i < sizeof(*dsb); i++)
    {
        if (p[i] != 0)
        {
            return 0;
        }
    }

    return 1;
}

static uint32_t volkovfs_djb2_hash(const unsigned char *data, size_t len)
{
    uint32_t hash = 5381;
    size_t i;
    for (i = 0; i < len; i++)
    {
        hash = ((hash << 5) + hash) + data[i];
    }

    return hash;
}

static int volkovfs_write_superblock(struct super_block *sb, sector_t sector,
                                      struct volkovfs_disk_sb *dsb)
{
    struct buffer_head *bh;

    bh = sb_bread(sb, sector);
    if (!bh)
    {
        return -EIO;
    }

    memcpy(bh->b_data, dsb, sizeof(*dsb));
    memset(bh->b_data + sizeof(*dsb), 0, VOLKOVFS_SECTOR_SIZE - sizeof(*dsb));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    return 0;
}

static int volkovfs_format_device(struct super_block *sb)
{
    struct volkovfs_disk_sb dsb;
    uint32_t data_start;
    uint32_t total_secs;
    uint32_t total_files;
    int ret;

    total_secs = (uint32_t)bdev_nr_sectors(sb->s_bdev);

    data_start = (sb_offset1 > sb_offset2 ? sb_offset1 : sb_offset2) + 1;

    if (total_secs <= data_start || max_file_sectors <= 0) 
    {
        pr_err("volkovfs: устройство слишком мало для форматирования\n");
        return -ENOSPC;
    }
    total_files = (total_secs - data_start) / max_file_sectors;

    memset(&dsb, 0, sizeof(dsb));
    dsb.magic = cpu_to_le32(VOLKOVFS_MAGIC);
    dsb.version = cpu_to_le32(VOLKOVFS_VERSION);
    dsb.total_files = cpu_to_le32(total_files);
    dsb.max_file_sectors = cpu_to_le32(max_file_sectors);
    dsb.max_name_len = cpu_to_le32(max_name_len);
    dsb.data_start_sector = cpu_to_le32(data_start);
    dsb.total_sectors = cpu_to_le32(total_secs);
    dsb.checksum = cpu_to_le32(volkovfs_compute_checksum(&dsb));

    pr_info("volkovfs: форматирование устройства — %u файлов, "
            "data_start=%u, total_sectors=%u\n",
            total_files, data_start, total_secs);

    ret = volkovfs_write_superblock(sb, sb_offset1, &dsb);
    if (ret)
    {
        return ret;
    }

    ret = volkovfs_write_superblock(sb, sb_offset2, &dsb);
    if (ret)
    {
        return ret;
    }

    return 0;
}

static const struct super_operations volkovfs_sb_ops = 
{
    .statfs     = simple_statfs,
    .drop_inode = generic_delete_inode,
};

static const struct file_operations volkovfs_file_ops = 
{
    .owner = THIS_MODULE,
    .read  = volkovfs_file_read,
    .write = volkovfs_file_write,
    .llseek = default_llseek,
};

static const struct file_operations volkovfs_dir_ops = 
{
    .owner          = THIS_MODULE,
    .iterate_shared = volkovfs_iterate,
    .read           = generic_read_dir,
    .llseek         = generic_file_llseek,
    .unlocked_ioctl = volkovfs_dir_ioctl,
};

static const struct inode_operations volkovfs_dir_inode_ops = 
{
    .lookup = volkovfs_lookup,
};

static struct inode *volkovfs_get_inode(struct super_block *sb, unsigned long ino)
{
    struct inode *inode;
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    struct timespec64 current_time;

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    if (!(inode->i_state & I_NEW))
        return inode;

    current_time = inode_set_ctime_current(inode);
    inode_set_atime_to_ts(inode, current_time);
    inode_set_mtime_to_ts(inode, current_time);

    if (ino == VOLKOVFS_ROOT_INO) 
    {
        inode->i_mode = S_IFDIR | 0755;
        set_nlink(inode, 2);
        inode->i_op = &volkovfs_dir_inode_ops;
        inode->i_fop = &volkovfs_dir_ops;
        inode->i_size = 0;
    } 
    else 
    {
        uint32_t file_idx = ino - VOLKOVFS_FILE_INO_BASE;
        inode->i_mode = S_IFREG | 0666;
        set_nlink(inode, 1);
        inode->i_fop = &volkovfs_file_ops;

        if (file_idx < sbi->total_files)
        {
            inode->i_size = sbi->file_sizes[file_idx];
        }
        else
        {
            inode->i_size = 0;
        }
    }

    unlock_new_inode(inode);
    return inode;
}

static int volkovfs_iterate(struct file *filp, struct dir_context *ctx)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    uint32_t file_idx;
    char name_buf[64];

    if (!dir_emit_dots(filp, ctx))
    {
        return 0;
    }

    file_idx = ctx->pos - 2;

    while (file_idx < sbi->total_files) 
    {
        int name_len;

        if (sbi->file_sizes[file_idx] > 0)
        {
            name_len = snprintf(name_buf, sizeof(name_buf), "file_%04u", file_idx);

            if (!dir_emit(ctx, name_buf, name_len, VOLKOVFS_FILE_INO_BASE + file_idx, DT_REG))
            {
                return 0;
            }
        }

        file_idx++;
        ctx->pos++;
    }

    return 0;
}

static struct dentry *volkovfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    const char *name = dentry->d_name.name;
    unsigned int file_idx;
    struct inode *inode;
    int ret;

    if (strncmp(name, "file_", 5) != 0)
    {
        goto not_found;
    }

    ret = kstrtouint(name + 5, 10, &file_idx);
    if (ret != 0)
    {
        goto not_found;
    }

    if (file_idx >= sbi->total_files)
    {
        goto not_found;
    }

    inode = volkovfs_get_inode(sb, VOLKOVFS_FILE_INO_BASE + file_idx);
    if (IS_ERR(inode))
    {
        return ERR_CAST(inode);
    }

    d_add(dentry, inode);
    return NULL;

not_found:
    d_add(dentry, NULL);
    return NULL;
}

static ssize_t volkovfs_file_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    uint32_t file_idx = inode->i_ino - VOLKOVFS_FILE_INO_BASE;
    uint32_t file_capacity = sbi->max_file_sectors * VOLKOVFS_SECTOR_SIZE;
    uint32_t file_size;
    sector_t base_sector;
    loff_t pos = *ppos;
    size_t bytes_read = 0;

    if (file_idx >= sbi->total_files)
    {
        return -EINVAL;
    }

    file_size = sbi->file_sizes[file_idx];

    if (pos >= file_size)
        return 0;

    if (pos + len > file_size)
    {
        len = file_size - pos;
    }

    if (len > file_capacity)
    {
        len = file_capacity;
    }

    base_sector = sbi->data_start + (sector_t)file_idx * sbi->max_file_sectors;

    while (bytes_read < len) 
    {
        struct buffer_head *bh;
        sector_t sector;
        size_t offset_in_sector;
        size_t to_copy;

        sector = base_sector + (pos / VOLKOVFS_SECTOR_SIZE);
        offset_in_sector = pos % VOLKOVFS_SECTOR_SIZE;
        to_copy = VOLKOVFS_SECTOR_SIZE - offset_in_sector;

        if (to_copy > len - bytes_read)
        {
            to_copy = len - bytes_read;
        }

        bh = sb_bread(sb, sector);
        if (!bh) 
        {
            pr_err("volkovfs: ошибка чтения сектора %llu\n", (unsigned long long)sector);
            return bytes_read > 0 ? bytes_read : -EIO;
        }

        if (copy_to_user(buf + bytes_read, bh->b_data + offset_in_sector, to_copy)) 
        {
            brelse(bh);
            return -EFAULT;
        }

        brelse(bh);
        bytes_read += to_copy;
        pos += to_copy;
    }

    *ppos = pos;
    return bytes_read;
}

static ssize_t volkovfs_file_write(struct file *filp, const char __user *buf,
                                    size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    uint32_t file_idx = inode->i_ino - VOLKOVFS_FILE_INO_BASE;
    uint32_t file_capacity = sbi->max_file_sectors * VOLKOVFS_SECTOR_SIZE;
    sector_t base_sector;
    loff_t pos = *ppos;
    size_t bytes_written = 0;
    struct timespec64 ts;

    if (file_idx >= sbi->total_files)
    {
        return -EINVAL;
    }

    if (pos >= file_capacity)
    {
        return -ENOSPC;
    }

    if (pos + len > file_capacity)
    {
        len = file_capacity - pos;
    }

    base_sector = sbi->data_start + (sector_t)file_idx * sbi->max_file_sectors;

    while (bytes_written < len) 
    {
        struct buffer_head *bh;
        sector_t sector;
        size_t offset_in_sector;
        size_t to_copy;

        sector = base_sector + (pos / VOLKOVFS_SECTOR_SIZE);
        offset_in_sector = pos % VOLKOVFS_SECTOR_SIZE;
        to_copy = VOLKOVFS_SECTOR_SIZE - offset_in_sector;

        if (to_copy > len - bytes_written)
        {
            to_copy = len - bytes_written;
        }

        bh = sb_bread(sb, sector);
        if (!bh) 
        {
            pr_err("volkovfs: ошибка чтения сектора %llu при записи\n", (unsigned long long)sector);
            return bytes_written > 0 ? bytes_written : -EIO;
        }

        if (copy_from_user(bh->b_data + offset_in_sector, buf + bytes_written, to_copy))
        {
            brelse(bh);
            return -EFAULT;
        }

        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);

        bytes_written += to_copy;
        pos += to_copy;
    }

    if (pos > sbi->file_sizes[file_idx]) 
    {
        sbi->file_sizes[file_idx] = pos;
        inode->i_size = pos;
    }

    *ppos = pos;

    ts = inode_set_ctime_current(inode);
    inode_set_mtime_to_ts(inode, ts);

    return bytes_written;
}

static long volkovfs_ioctl_hashes(struct super_block *sb, unsigned long arg)
{
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    struct volkovfs_hashes_arg ha;
    uint32_t i;
    uint32_t count;

    if (copy_from_user(&ha, (void __user *)arg, sizeof(ha)))
    {
        return -EFAULT;
    }

    count = ha.count;
    if (count > VOLKOVFS_MAX_HASHES)
    {
        count = VOLKOVFS_MAX_HASHES;
    }

    if (ha.start >= sbi->total_files)
    {
        count = 0;
    }
    else if (ha.start + count > sbi->total_files)
    {
        count = sbi->total_files - ha.start;
    }

    ha.count = count;

    for (i = 0; i < count; i++) 
    {
        uint32_t file_idx = ha.start + i;
        sector_t base = sbi->data_start + (sector_t)file_idx * sbi->max_file_sectors;
        uint32_t hash = 5381;
        uint32_t s;

        for (s = 0; s < sbi->max_file_sectors; s++) 
        {
            struct buffer_head *bh = sb_bread(sb, base + s);
            if (!bh) 
            {
                ha.hashes[i] = 0;
                goto next_file;
            }
            hash = volkovfs_djb2_hash((unsigned char *)bh->b_data, VOLKOVFS_SECTOR_SIZE);
            if (s > 0)
            {
                ha.hashes[i] ^= hash;
            }
            else
            {
                ha.hashes[i] = hash;
            }
            brelse(bh);
        }
next_file:
        continue;
    }

    if (copy_to_user((void __user *)arg, &ha, sizeof(ha)))
        return -EFAULT;

    return 0;
}

static long volkovfs_ioctl_mapping(struct super_block *sb, unsigned long arg)
{
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    struct volkovfs_mapping_arg ma;

    if (copy_from_user(&ma, (void __user *)arg, sizeof(ma)))
    {
        return -EFAULT;
    }

    if (ma.file_index >= sbi->total_files)
    {
        return -EINVAL;
    }

    ma.start_sector = sbi->data_start + ma.file_index * sbi->max_file_sectors;
    ma.num_sectors = sbi->max_file_sectors;
    ma.data_start = sbi->data_start;

    if (copy_to_user((void __user *)arg, &ma, sizeof(ma)))
    {
        return -EFAULT;
    }

    return 0;
}

static long volkovfs_ioctl_zero(struct super_block *sb)
{
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    sector_t total_data_sectors = (sector_t)sbi->total_files * sbi->max_file_sectors;
    sector_t s;

    pr_info("volkovfs: обнуление %llu секторов данных...\n", (unsigned long long)total_data_sectors);

    for (s = 0; s < total_data_sectors; s++) 
    {
        struct buffer_head *bh = sb_bread(sb, sbi->data_start + s);
        if (!bh)
        {
            continue;
        }

        memset(bh->b_data, 0, VOLKOVFS_SECTOR_SIZE);
        mark_buffer_dirty(bh);
        brelse(bh);
    }

    sync_blockdev(sb->s_bdev);

    memset(sbi->file_sizes, 0, sbi->total_files * sizeof(uint32_t));

    if (sb->s_root)
    {
        shrink_dcache_parent(sb->s_root);
    }

    pr_info("volkovfs: обнуление завершено\n");
    return 0;
}

static long volkovfs_ioctl_erase(struct super_block *sb)
{
    struct volkovfs_sb_info *sbi = sb->s_fs_info;
    struct volkovfs_disk_sb dsb;
    int ret;

    pr_info("volkovfs: полный сброс ФС...\n");

    ret = volkovfs_ioctl_zero(sb);
    if (ret)
    {
        return ret;
    }

    memset(&dsb, 0, sizeof(dsb));
    dsb.magic = cpu_to_le32(VOLKOVFS_MAGIC);
    dsb.version = cpu_to_le32(VOLKOVFS_VERSION);
    dsb.total_files = cpu_to_le32(sbi->total_files);
    dsb.max_file_sectors = cpu_to_le32(sbi->max_file_sectors);
    dsb.max_name_len = cpu_to_le32(sbi->max_name_len);
    dsb.data_start_sector = cpu_to_le32(sbi->data_start);
    dsb.total_sectors = cpu_to_le32(sbi->total_sectors);
    dsb.checksum = cpu_to_le32(volkovfs_compute_checksum(&dsb));

    volkovfs_write_superblock(sb, sb_offset1, &dsb);
    volkovfs_write_superblock(sb, sb_offset2, &dsb);

    pr_info("volkovfs: сброс завершен\n");
    return 0;
}

static long volkovfs_dir_ioctl(struct file *filp, unsigned int cmd,
                                unsigned long arg)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;

    switch (cmd) 
    {
    case VOLKOVFS_IOC_HASHES:
    {
        return volkovfs_ioctl_hashes(sb, arg);
    }
    case VOLKOVFS_IOC_MAPPING:
    {
        return volkovfs_ioctl_mapping(sb, arg);
    }
    case VOLKOVFS_IOC_ZERO:
    {
        return volkovfs_ioctl_zero(sb);
    }
    case VOLKOVFS_IOC_ERASE:
    {
        return volkovfs_ioctl_erase(sb);
    }
    default:
    {
        return -ENOTTY;
    }
    }
}

static int volkovfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct volkovfs_sb_info *sbi;
    struct volkovfs_disk_sb *dsb;
    struct buffer_head *bh;
    struct inode *root_inode;

    if (!sb_set_blocksize(sb, VOLKOVFS_SECTOR_SIZE)) 
    {
        pr_err("volkovfs: не удалось установить размер блока %d\n", VOLKOVFS_SECTOR_SIZE);
        return -EINVAL;
    }

    sbi = kzalloc(sizeof(struct volkovfs_sb_info), GFP_KERNEL);
    if (!sbi)
    {
        return -ENOMEM;
    }

    sb->s_fs_info = sbi;
    sb->s_op = &volkovfs_sb_ops;
    sb->s_magic = VOLKOVFS_MAGIC;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;

    bh = sb_bread(sb, sb_offset1);
    if (!bh) 
    {
        pr_err("volkovfs: не удалось прочитать сектор %d\n", sb_offset1);
        goto fail_sbi;
    }

    dsb = (struct volkovfs_disk_sb *)bh->b_data;

    if (le32_to_cpu(dsb->magic) == VOLKOVFS_MAGIC &&
        le32_to_cpu(dsb->checksum) == volkovfs_compute_checksum(dsb))
    {
        goto sb_ready;
    }
    else
    {
        int sb1_blank = volkovfs_sb_is_blank(dsb);

        brelse(bh);

        bh = sb_bread(sb, sb_offset2);
        if (!bh)
        {
            pr_err("volkovfs: не удалось прочитать сектор %d\n", sb_offset2);
            goto fail_sbi;
        }
        dsb = (struct volkovfs_disk_sb *)bh->b_data;

        if (le32_to_cpu(dsb->magic) == VOLKOVFS_MAGIC &&
            le32_to_cpu(dsb->checksum) == volkovfs_compute_checksum(dsb))
        {
            pr_info("volkovfs: первая копия суперблока повреждена, "
                    "использована вторая копия\n");
            goto sb_ready;
        }

        if (sb1_blank && volkovfs_sb_is_blank(dsb))
        {
            brelse(bh);

            if (!silent)
                pr_info("volkovfs: суперблок не найден, форматирование...\n");

            if (volkovfs_format_device(sb) != 0)
                goto fail_sbi;

            bh = sb_bread(sb, sb_offset1);
            if (!bh)
            {
                goto fail_sbi;
            }
            dsb = (struct volkovfs_disk_sb *)bh->b_data;
            goto sb_ready;
        }

        pr_err("volkovfs: обе копии суперблока повреждены, "
               "монтирование невозможно\n");
        brelse(bh);
        goto fail_corrupt;
    }

sb_ready:
    sbi->total_files = le32_to_cpu(dsb->total_files);
    sbi->max_file_sectors = le32_to_cpu(dsb->max_file_sectors);
    sbi->max_name_len = le32_to_cpu(dsb->max_name_len);
    sbi->data_start = le32_to_cpu(dsb->data_start_sector);
    sbi->total_sectors = le32_to_cpu(dsb->total_sectors);

    brelse(bh);

    pr_info("volkovfs: смонтировано — %u файлов, %u секторов/файл, data_start=%u\n",
            sbi->total_files, sbi->max_file_sectors, sbi->data_start);

    sbi->file_sizes = kvcalloc(sbi->total_files, sizeof(uint32_t), GFP_KERNEL);
    if (!sbi->file_sizes) 
    {
        pr_err("volkovfs: не удалось выделить массив размеров\n");
        goto fail_sbi;
    }

    root_inode = volkovfs_get_inode(sb, VOLKOVFS_ROOT_INO);
    if (IS_ERR(root_inode)) 
    {
        pr_err("volkovfs: не удалось создать корневой inode\n");
        goto fail_sizes;
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) 
    {
        pr_err("volkovfs: не удалось создать корневой dentry\n");
        goto fail_sizes;
    }

    return 0;

fail_sizes:
    kvfree(sbi->file_sizes);
fail_sbi:
    kfree(sbi);
    sb->s_fs_info = NULL;
    return -ENOMEM;

fail_corrupt:
    kfree(sbi);
    sb->s_fs_info = NULL;
    return -EUCLEAN;
}

static struct dentry *volkovfs_mount(struct file_system_type *fs_type, int flags,
                                     const char *dev_name, void *data)
{
    struct dentry *entry;

    entry = mount_bdev(fs_type, flags, dev_name, data, volkovfs_fill_super);

    if (IS_ERR(entry))
    {
        pr_err("volkovfs: ошибка монтирования устройства %s\n", dev_name);
    }
    else
    {
        pr_info("volkovfs: смонтировано устройство %s\n", dev_name);
    }

    return entry;
}

static void volkovfs_kill_sb(struct super_block *sb)
{
    struct volkovfs_sb_info *sbi = sb->s_fs_info;

    if (sbi) 
    {
        pr_info("volkovfs: размонтирование, освобождение ресурсов\n");
        if (sbi->file_sizes)
        {
            kvfree(sbi->file_sizes);
        }
        kfree(sbi);
        sb->s_fs_info = NULL;
    }

    kill_block_super(sb);
}

static struct file_system_type volkovfs_type = 
{
    .owner    = THIS_MODULE,
    .name     = "volkovfs",
    .mount    = volkovfs_mount,
    .kill_sb  = volkovfs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init volkovfs_init(void)
{
    int ret;

    pr_info("volkovfs: загрузка модуля\n");
    pr_info("volkovfs: параметры — sb_offset1=%d, sb_offset2=%d, "
            "max_file_sectors=%d, max_name_len=%d\n",
            sb_offset1, sb_offset2, max_file_sectors, max_name_len);

    if (sb_offset1 < 0 || sb_offset2 < 0) 
    {
        pr_err("volkovfs: смещения суперблоков не могут быть отрицательными\n");
        return -EINVAL;
    }
    if (sb_offset1 == sb_offset2) 
    {
        pr_err("volkovfs: смещения суперблоков должны различаться\n");
        return -EINVAL;
    }
    if (max_file_sectors <= 0) 
    {
        pr_err("volkovfs: max_file_sectors должен быть > 0\n");
        return -EINVAL;
    }
    if (max_name_len <= 5) 
    {
        pr_err("volkovfs: max_name_len слишком мал\n");
        return -EINVAL;
    }

    ret = register_filesystem(&volkovfs_type);
    if (ret) 
    {
        pr_err("volkovfs: не удалось зарегистрировать ФС (код %d)\n", ret);
        return ret;
    }

    pr_info("volkovfs: модуль успешно загружен\n");
    return 0;
}

static void __exit volkovfs_exit(void)
{
    unregister_filesystem(&volkovfs_type);
    pr_info("volkovfs: модуль выгружен\n");
}

module_init(volkovfs_init);
module_exit(volkovfs_exit);