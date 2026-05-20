// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"
#include "super.h"

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
static int obsidianfs_hardlink(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname);
static int obsidianfs_unlink(struct inode *dir, struct dentry *dentry);
static int obsidian_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr);

/* ------------------------------------------------------------------ */
/* inode_operations tables                                              */
/* ------------------------------------------------------------------ */

static const struct inode_operations obsidianfs_file_inode_ops = {
	.setattr = obsidian_setattr,
	.getattr = simple_getattr,
};

static const struct inode_operations obsidianfs_dir_inode_ops = {
	.create  = obsidianfs_create,
	.lookup  = simple_lookup,
	.link    = obsidianfs_hardlink,
	.unlink  = obsidianfs_unlink,
	.symlink = obsidianfs_symlink,
	.mkdir   = obsidianfs_mkdir,
	.rmdir   = simple_rmdir,
	.mknod   = obsidianfs_mknod,
	.rename  = simple_rename,
};

/* ------------------------------------------------------------------ */
/* Read inode from disk                                               */
/* ------------------------------------------------------------------ */


// Recover and read an inode from the disk
struct inode *obsidianfs_iget(struct super_block *sb, unsigned long ino)
{
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);
	struct obsidianfs_inode_meta *oi;
	struct obsidianfs_inode *raw;
	struct buffer_head *bh;
	struct inode *inode;
	unsigned long block;
	unsigned long offset;
	unsigned int inode_size;
	unsigned int inodes_per_block;

	if (ino < OBSIDIANFS_ROOT_INO || ino > sbi->s_inodes_count) {
		pr_err("[ERROR OBSIDIANFS] %s: inode %lu out of range (max %lu)\n",
		       __func__, ino, sbi->s_inodes_count);
		return ERR_PTR(-EINVAL);
	}

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode; // Return the inode if it's already in cache. 

	oi = OBSIDIANFS_INODE(inode);

	// Compute position of this inode in the inode table
	inode_size = le32_to_cpu(sbi->s_es->s_inode_size);
	if (!inode_size)
		inode_size = sizeof(struct obsidianfs_inode);

	inodes_per_block = sb->s_blocksize / inode_size;
	if (!inodes_per_block) {
		pr_err("[ERROR OBSIDIANFS] %s: inode size %u > block size %lu\n",
		       __func__, inode_size, sb->s_blocksize);
		goto bad_inode;
	}

	block  = OBSIDIANFS_INODE_TABLE_BLOCK + (ino - 1) / inodes_per_block;
	offset = ((ino - 1) % inodes_per_block) * inode_size;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot read block %lu\n",
		       __func__, block);
		goto bad_inode;
	}

	raw = (struct obsidianfs_inode *)(bh->b_data + offset);

	/* Populate VFS inode from on-disk fields */
	inode->i_mode   = le16_to_cpu(raw->i_mode);
	inode->i_uid    = make_kuid(sb->s_user_ns, le32_to_cpu(raw->i_uid));
	inode->i_gid    = make_kgid(sb->s_user_ns, le32_to_cpu(raw->i_gid));
	set_nlink(inode,  le16_to_cpu(raw->i_links_count));
	inode->i_size   = le64_to_cpu(raw->i_size);
	inode->i_blocks = le32_to_cpu(raw->i_blocks);
	inode_set_atime(inode, le32_to_cpu(raw->i_atime), 0);
	inode_set_ctime(inode, le32_to_cpu(raw->i_ctime), 0);
	inode_set_mtime(inode, le32_to_cpu(raw->i_mtime), 0);

	
	memcpy(oi->i_data, raw->i_block, sizeof(oi->i_data));

	oi->valid_size      = inode->i_size;
	oi->flagsProtected  = false;
	oi->i_block_group   = 0;	

	brelse(bh);

	/* Wire up operations by file type */
	inode->i_mapping->a_ops = &obsidianfs_page_ops;
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op  = &obsidianfs_file_inode_ops;
		inode->i_fop = &obsidianfs_file_ops;
		break;
	case S_IFDIR:
		inode->i_op  = &obsidianfs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	default:
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}

	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(-EIO);
}

/* ------------------------------------------------------------------ */
/* Inode allocation                                                     */
/* ------------------------------------------------------------------ */

// Create a very new inode in memory => And after it should be saved on the disk with the function obsidianfs_write_inode.
struct inode *obsidianfs_create_inode_memory(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	struct obsidianfs_inode_meta *oi;

	if (!inode) {
		pr_err("[ERROR OBSIDIANFS] %s: new_inode failed\n", __func__);
		return NULL;
	}

	oi = OBSIDIANFS_INODE(inode);
	oi->flagsProtected  = false;
	oi->valid_size      = 0;

	inode->i_ino            = get_next_ino();
	inode->i_mapping->a_ops = &obsidianfs_page_ops;
	inode->i_mode           = mode;
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFREG:
		inode->i_op  = &obsidianfs_file_inode_ops;
		inode->i_fop = &obsidianfs_file_ops;
		break;
	case S_IFDIR:
		inode->i_op  = &obsidianfs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	default:
		init_special_inode(inode, mode, dev);
		break;
	}

	return inode;
}

/* ------------------------------------------------------------------ */
/* Directory inode operations                                           */
/* ------------------------------------------------------------------ */

static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return obsidianfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int obsidianfs_hardlink(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);

	inode_set_mtime_to_ts(dir,
		inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	inc_nlink(inode);
	igrab(inode);
	d_instantiate(dentry, inode);
	mark_inode_dirty(dir);
	pr_info("[INFO OBSIDIANFS] hardlink count = %u\n", inode->i_nlink);
	return 0;
}

static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error;

	inode = obsidianfs_create_inode_memory(dir->i_sb, dir, S_IFLNK | 0700, 0);
	if (!inode) {
		pr_err("[ERROR OBSIDIANFS] %s: get_inode failed\n", __func__);
		return -ENOSPC;
	}

	error = page_symlink(inode, symname, strlen(symname) + 1);
	if (error) {
		iput(inode);
		pr_err("[ERROR OBSIDIANFS] %s: page_symlink failed\n", __func__);
		return error;
	}

	d_instantiate(dentry, inode);
	dget(dentry);
	return 0;
}

static int obsidianfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);

	if (oi->flagsProtected) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return -EPERM;
	}

	inode_set_mtime_to_ts(dir,
		inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	drop_nlink(inode);
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	return 0;
}

static int obsidian_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	int error;

	if (oi->flagsProtected) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return -EPERM;
	}

	error = setattr_prepare(idmap, dentry, iattr);
	if (error)
		return error;

	if (iattr->ia_valid & ATTR_SIZE)
		truncate_setsize(inode, iattr->ia_size);
	setattr_copy(idmap, inode, iattr);
	mark_inode_dirty(inode);
	return 0;
}
