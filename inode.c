// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"

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
/* Inode allocation                                                     */
/* ------------------------------------------------------------------ */

struct inode *obsidianfs_get_inode(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev)
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

	inode = obsidianfs_get_inode(dir->i_sb, dir, S_IFLNK | 0700, 0);
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
