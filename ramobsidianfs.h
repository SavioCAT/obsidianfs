#ifndef OBSIDIANFS_H
#define OBSIDIANFS_H

#include <linux/fs.h>
#include <linux/pagemap.h>

#define OBSIDIAN_MAGIC 0x6C854200

struct obsidianfs_inode_meta {
	struct	inode vfs_inode; // VFS inode structure
	u32	flags; // Value to contain flags if needed
	/* 
	 * Can add value if more metadata value is needed in the future
	 * vfs_inode MUST BE the first value in the structure
	 */
};

static inline struct obsidianfs_inode_meta *OBSIDIANFS_INODE(struct inode *inode) {
	return container_of(inode, struct obsidianfs_inode_meta, vfs_inode);
}

/*
 * Declared in inode.c, Allocate and initiate a new inode
 * sb 	: superblock to which the inode must be attached
 * dir 	: the parent inode (directory containing the new inode), can be NULL
 * mode : type + permission, for exemple 0644 for a regular file
 * dev 	: device number (only used for S_IFBLK / S_IFCHR)
 */
extern struct inode *obsidianfs_get_inode(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev);

/*
 * Declared in file.c, used in inode.c while creating an inode (inode->i_fop = &obsidianfs_file_ops)
 * obsidianfs_file_ops	: open, read, write, llseek… on regular files
 * obsidianfs_aops	: interaction with the page cache (readpage, writepage…) used by the kernel for page-based read/write operations
 */
extern const struct file_operations obsidianfs_file_ops;
#endif
