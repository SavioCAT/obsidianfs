// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"
#include "ioctlops.h"

/* ------------------------------------------------------------------ */
/* address_space_operations                                             */
/* ------------------------------------------------------------------ */

const struct address_space_operations obsidianfs_page_ops = {
	.read_folio       = obsidian_read_folio,
	.write_begin      = obsidian_write_begin,
	.write_end        = obsidian_write_end,
	.dirty_folio      = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
};

/* ------------------------------------------------------------------ */
/* Directory operations                                                 */
/* ------------------------------------------------------------------ */

int obsidianfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = obsidianfs_create_inode_memory(dir->i_sb, dir, mode, dev);
	int err;

	if (!inode) {
		pr_err("[ERROR OBSIDIANFS] %s: get_inode failed\n", __func__);
		return -ENOSPC;
	}

	err = obsidianfs_add_dir_entry(dir, &dentry->d_name, inode->i_ino);
	if (err) {
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);
	dget(dentry);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	return 0;
}

struct dentry *obsidianfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int ret = obsidianfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);

	if (ret)
		return ERR_PTR(ret);
	inc_nlink(dir);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* File operations                                                      */
/* ------------------------------------------------------------------ */

static ssize_t obsidian_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file   = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	ssize_t ret;

	if (oi->flagsProtected) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return -EPERM;
	}

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);

	return ret;
}

const struct file_operations obsidianfs_file_ops = {
	.read_iter      = generic_file_read_iter,
	.write_iter     = obsidian_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = noop_fsync,
	.llseek         = generic_file_llseek,
	.splice_read    = filemap_splice_read,
	.unlocked_ioctl = obsidianfs_ioctl,
};
