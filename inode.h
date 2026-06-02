#ifndef INODEOBSIDIANFS_H
#define INODEOBSIDIANFS_H

#define OBSIDIAN_MAGIC    0x6C854200u
#define OBSIDIAN_N_BLOCKS 15u   /* 12 direct + 1 indirect + 1 double indirect + 1 tripe indirect */

struct obsidianfs_inode {
	__le64	i_size;			/* Size in bytes Using 64 bits for the size allow 2^63 bytes files (really huge)*/
	__le32	i_atime;		/* Last access time (Unix seconds) */
	__le32	i_ctime;		/* Inode change time */
	__le32	i_mtime;		/* Last modification time */
	__le32	i_dtime;		/* Deletion time */
	__le32	i_uid;			/* Owner UID */
	__le32	i_gid;			/* Owner GID */
	__le32	i_blocks;		/* Block count (512-byte units) */
	__le32	i_flags;		/* File flags */
	__le16	i_mode;			/* File mode */
	__le16	i_links_count;		/* Hard link count */
	__le32	i_generation;		/* File version (NFS) */
	__le32	i_block[OBSIDIAN_N_BLOCKS]; /* Block pointers */
	__u8	i_flagsProtected;	/* Flag for the file protection */
	__u8	i_reserved[19];		/* Reserved — pad to 128 bytes */
};

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/rbtree.h>
#include "pageops.h"

typedef unsigned long obsidianfs_fsblk_t;

struct obsidianfs_inode_meta {
	struct inode  vfs_inode;          /* VFS inode — MUST be first */
	struct mutex  i_lock;             /* protects block allocation and valid_size */
	rwlock_t      readwritelock;
	loff_t        valid_size;         /* highest byte written */
	bool          flagsProtected;
	struct obsidianfs_block_alloc_info *i_block_alloc_info;
	__le32        i_data[OBSIDIAN_N_BLOCKS]; /* mirrors obsidianfs_inode.i_block */
	__u32         i_block_group;
};

static inline struct obsidianfs_inode_meta *OBSIDIANFS_INODE(struct inode *inode)
{
	return container_of(inode, struct obsidianfs_inode_meta, vfs_inode);
}

extern struct inode *obsidianfs_create_inode_memory(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev);
extern struct inode *obsidianfs_iget(struct super_block *sb, unsigned long ino);
extern unsigned long obsidianfs_alloc_ino(struct super_block *sb);
extern void          obsidianfs_free_ino(struct super_block *sb, unsigned long ino);
extern int           obsidianfs_add_dir_entry(struct inode *dir, const struct qstr *qstr, unsigned long ino);
extern int           obsidianfs_remove_dir_entry(struct inode *dir, const struct qstr *qstr);
extern void          obsidianfs_truncate_blocks(struct inode *inode);

/* on-disk directory entry */
#define OBSIDIANFS_MAX_NAME_LEN    255
#define OBSIDIANFS_DIR_ENTRY_HSIZE 8   /* ino(4) + rec_len(2) + name_len(1) + pad(1) */

struct obsidianfs_dir_entry {
	__le32  ino;
	__le16  rec_len;
	__u8    name_len;
	__u8    pad;
	char    name[];
};

static inline unsigned int obsidianfs_dir_rec_len(unsigned int name_len)
{
	return (OBSIDIANFS_DIR_ENTRY_HSIZE + name_len + 3) & ~3u;
}

#endif

#endif
