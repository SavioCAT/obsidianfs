#ifndef IOCTLOPSOBSIDIANFS_H
#define IOCTLOPSOBSIDIANFS_H

#include <linux/ioctl.h>
#include <linux/types.h>


#define OBSIDIAN_IOC_MAGIC  'O'

#define OBSIDIAN_IOC_PROTECT    _IO(OBSIDIAN_IOC_MAGIC, 1)
#define OBSIDIAN_IOC_GET_FLAGS  _IOR(OBSIDIAN_IOC_MAGIC, 2, char)
#define OBSIDIAN_TEST           _IO(OBSIDIAN_IOC_MAGIC, 3)

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/pagemap.h>

extern long obsidianfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif

#endif