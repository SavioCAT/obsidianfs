/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PAGEOPSOBSIDIANFS_H
#define PAGEOPSOBSIDIANFS_H

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/log2.h>
#include <linux/rbtree.h>
#include "inode.h"

/* block addressing constants */
#define NDIR_BLOCKS          12
#define OBSIDIAN_IND_BLOCK   (NDIR_BLOCKS)
#define OBSIDIAN_DIND_BLOCK  (NDIR_BLOCKS + 1)
#define OBSIDIAN_TIND_BLOCK  (NDIR_BLOCKS + 2)

#define FGP_WRITEBEGIN          (FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)
#define OBSIDIAN_BLOCK_SIZE(s)      ((s)->s_blocksize)
#define ADDR_PER_BLOCK(s)           (OBSIDIAN_BLOCK_SIZE(s) / sizeof(__u32))
#define OBSIDIAN_BLOCK_SIZE_BITS(s) ((s)->s_blocksize_bits)
#define ADDR_PER_BLOCK_BITS(s)      (ilog2(ADDR_PER_BLOCK(s)))

extern int obsidian_read_folio(struct file *file, struct folio *folio);
extern int obsidian_write_begin(const struct kiocb *iocb,
				struct address_space *mapping,
				loff_t pos, unsigned len,
				struct folio **foliop, void **fsdata);
extern int obsidian_write_end(const struct kiocb *iocb,
			      struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct folio *folio, void *fsdata);

typedef struct {
	__le32             *p;
	__le32              key;
	struct buffer_head *bh;
} Indirect;

struct obsidianfs_reserve_window {
	unsigned long _rsv_start;
	unsigned long _rsv_end;
};

struct obsidianfs_reserve_window_node {
	struct rb_node                   rsv_node;
	__u32                            rsv_goal_size;
	__u32                            rsv_alloc_hit;
	struct obsidianfs_reserve_window rsv_window;
};

struct obsidianfs_block_alloc_info {
	struct obsidianfs_reserve_window_node rsv_window_node;
	__u32              last_alloc_logical_block;
	obsidianfs_fsblk_t last_alloc_physical_block;
};

extern void obsidianfs_init_block_alloc_info(struct inode *inode);

#endif /* PAGEOPSOBSIDIANFS_H */
