#ifndef COWOBSIDIANFS_H
#define COWOBSIDIANFS_H

#include <linux/fs.h>
#include "inode.h"
#include "pageops.h"

#define OBSIDIANFS_MAX_BACKUPS 20

extern struct inode *obsidianfs_cow_inode(struct inode *old_inode, struct dentry *dentry);

#endif