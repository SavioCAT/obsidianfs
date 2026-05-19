/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef INODEOBSIDIANFS_H
#define INODEOBSIDIANFS_H

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/rbtree.h>

#define OBSIDIAN_MAGIC 0x6C854200

typedef unsigned long obsidianfs_fsblk_t;

/* forward declaration — full definition in pageops.h */
struct obsidianfs_block_alloc_info;

struct obsidianfs_inode_meta {
	struct inode  vfs_inode;          /* VFS inode — MUST be first */
	struct mutex  i_lock;             /* protects block allocation and valid_size */
	rwlock_t      readwritelock;
	loff_t        valid_size;         /* highest byte written */
	bool          flagsProtected;
	struct obsidianfs_block_alloc_info *i_block_alloc_info;
	__le32        i_data[15];         /* 12 direct + 1 ind + 1 dind + 1 tind */
	__u32         i_block_group;
};

static inline struct obsidianfs_inode_meta *OBSIDIANFS_INODE(struct inode *inode)
{
	return container_of(inode, struct obsidianfs_inode_meta, vfs_inode);
}

extern struct inode *obsidianfs_get_inode(struct super_block *sb,
					   const struct inode *dir,
					   umode_t mode, dev_t dev);

#endif /* INODEOBSIDIANFS_H */
