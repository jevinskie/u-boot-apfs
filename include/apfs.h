#pragma once

struct blk_desc;
struct disk_partition;
struct fs_dirent;
struct fs_dir_stream;


int apfs_probe(struct blk_desc *fs_dev_desc,
         struct disk_partition *fs_partition);
int apfs_exists(const char *filename);
int apfs_size(const char *filename, loff_t *size);
int apfs_read(const char *filename, void *buf, loff_t offset,
        loff_t len, loff_t *actread);
int apfs_write(const char *filename, void *buf, loff_t offset,
         loff_t len, loff_t *actwrite);
void apfs_close(void);
int apfs_uuid(char *uuid_str);
int apfs_opendir(const char *filename, struct fs_dir_stream **dirsp);
int apfs_readdir(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
void apfs_closedir(struct fs_dir_stream *dirs);
