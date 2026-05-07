#ifndef FILEOBSIDIANFS_H
#define FILEOBSIDIANFS_H

#include <linux/fs.h>
#include <linux/pagemap.h>

extern struct dentry *obsidianfs_mkdir(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
extern int obsidianfs_mknod(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, dev_t);
extern const struct file_operations obsidianfs_file_ops;
extern const struct address_space_operations obsidianfs_page_ops;

#endif