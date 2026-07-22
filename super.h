#ifndef SUPEROBSIDIANFS_H
#define SUPEROBSIDIANFS_H

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "addressbitmap.h"


struct obsidianfs_super_block {
	__le16	s_magic;			/* Must equal OBSIDIAN_MAGIC */
	__le32	s_blocks_count;		/* Total blocks on device */
	__le32	s_inodes_count;		/* Total inodes */
	__le32	s_free_blocks_count;
	__le32	s_free_inodes_count;
	__le32	s_first_data_block;	/* First block available for data */
	__le32	s_log_block_size;	/* Block size = 1024 << s_log_block_size */
	__le32	s_inode_size;		/* Size of one on-disk inode struct */
	__le32	s_mtime;			/* Last mount time */
	__le32	s_wtime;			/* Last write time */
	__u8	s_uuid[16];			/* Volume UUID */
	char	s_volume_name[16];
	__u8	s_reserved[458];	/* Pad to 512 bytes */
};

struct obsidianfs_sb_info {
	struct buffer_head				*s_sbh;		/* buffer holding s_es */
	struct obsidianfs_super_block	*s_es;		/* pointer into s_sbh->b_data */
	unsigned long			 		s_sb_block;
	unsigned long			 		s_blocks_count;
	unsigned long			 		s_inodes_count;
	unsigned long			 		s_first_data_block;
	struct mutex		 			s_lock;
};

/*
 * Mount-time options parsed by fs_context.
 */
struct obsidianfs_fs_context {
	unsigned long	s_sb_block;
};

static inline struct obsidianfs_sb_info *OBSIDIANFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

#endif
