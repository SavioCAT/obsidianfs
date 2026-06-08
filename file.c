// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/splice.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"
#include "ioctlops.h"

const struct address_space_operations obsidianfs_page_ops = {
	.read_folio       = obsidian_read_folio,
	.writepages       = obsidianfs_writepages,
	.write_begin      = obsidian_write_begin,
	.write_end        = obsidian_write_end,
	.dirty_folio      = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
};

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
	mark_inode_dirty(inode);
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

static int obsidianfs_file_release(struct inode *inode, struct file *file)
{
	/*
	 * After CoW, file->f_inode points to the new inode while
	 * file->f_path.dentry still anchors the original inode.
	 * We hold the creation reference on every CoW inode we redirect to,
	 * so release it here to avoid busy-inode warnings on umount.
	 */
	if (inode != file->f_path.dentry->d_inode)
		iput(inode);
	return 0;
}

static ssize_t obsidian_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file   = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	struct dentry *dentry = file_dentry(file);
	ssize_t ret;

	if (oi->flagsProtected) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return -EPERM;
	}

	// CoW, If there is something in the file, write the modification in a new inode
	if (inode->i_size > 0) {
		struct inode *prev_inode = file->f_inode;
		bool prev_was_cow = (prev_inode != file->f_path.dentry->d_inode);
		struct inode *new_inode = obsidianfs_cow_inode(inode, dentry);

		if (IS_ERR(new_inode)) {
			return PTR_ERR(new_inode);
		}

		oi->flagsProtected = true; // Protect the old inode
		mark_inode_dirty(inode);

		get_write_access(new_inode);
		put_write_access(inode);

		// Redirect to the new inode
		file->f_inode   = new_inode;
		file->f_mapping = &new_inode->i_data;
		inode = new_inode;

		/* Release the creation ref held on the previous CoW inode. */
		if (prev_was_cow)
			iput(prev_inode);
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

static ssize_t obsidianfs_copy_file_range(struct file *file_in, loff_t pos_in, struct file *file_out, loff_t pos_out, size_t len, unsigned int flags)
{
	return splice_file_range(file_in, &pos_in, file_out, &pos_out, len);
}

const struct file_operations obsidianfs_file_ops = {
	.read_iter       = generic_file_read_iter,
	.write_iter      = obsidian_write_iter,
	.release         = obsidianfs_file_release,
	.mmap            = generic_file_mmap,
	.fsync           = generic_file_fsync,
	.llseek          = generic_file_llseek,
	.splice_read     = filemap_splice_read,
	.splice_write    = iter_file_splice_write,
	.unlocked_ioctl  = obsidianfs_ioctl,
	.copy_file_range = obsidianfs_copy_file_range,
};
