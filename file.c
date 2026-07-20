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

	if (inode->i_size > 0 && !file->private_data) {
		struct inode *cur_inode = obsidianfs_cow_inode(inode, dentry);

		if (IS_ERR(cur_inode)) {
			return PTR_ERR(cur_inode);
		}
		file->private_data = (void *)1;
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

// Take the CoW snapshot for a write that comes through a shared mapping.
// Same contract as in obsidian_write_iter: one snapshot per open file,
// private_data is the "already snapshotted" marker.
static int obsidianfs_mmap_cow(struct file *file)
{
	struct inode *inode = file->f_mapping->host;

	if (inode->i_size > 0 && !file->private_data) {
		struct inode *cur_inode = obsidianfs_cow_inode(inode, file_dentry(file));

		if (IS_ERR(cur_inode))
			return PTR_ERR(cur_inode);
		file->private_data = (void *)1;
	}
	return 0;
}

// First write fault on a shared mapping. This also catches mappings created
// read-only and upgraded with mprotect(PROT_WRITE) afterwards, which never
// go through obsidianfs_file_mmap with VM_WRITE set.
static vm_fault_t obsidianfs_page_mkwrite(struct vm_fault *vmf)
{
	struct file *file   = vmf->vma->vm_file;
	struct inode *inode = file->f_mapping->host;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);

	if (oi->flagsProtected) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	if (obsidianfs_mmap_cow(file))
		return VM_FAULT_SIGBUS;

	return filemap_page_mkwrite(vmf);
}

static const struct vm_operations_struct obsidianfs_file_vm_ops = {
	.fault        = filemap_fault,
	.map_pages    = filemap_map_pages,
	.page_mkwrite = obsidianfs_page_mkwrite,
};

static int obsidianfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_mapping->host;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);

	// VM_MAYWRITE (not just VM_WRITE) so a PROT_READ/MAP_SHARED mapping on a
	// fd opened O_RDWR cannot be upgraded to writable later with mprotect.
	// MAP_PRIVATE stays allowed: its writes never reach the file.
	if (oi->flagsProtected &&
	    (vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_MAYWRITE)) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return -EPERM;
	}

	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE)) {
		int err = obsidianfs_mmap_cow(file);

		if (err)
			return err;
	}

	file_accessed(file);
	vma->vm_ops = &obsidianfs_file_vm_ops;
	return 0;
}

const struct file_operations obsidianfs_file_ops = {
	.read_iter       = generic_file_read_iter,
	.write_iter      = obsidian_write_iter,
	.release         = obsidianfs_file_release,
	.mmap            = obsidianfs_file_mmap,
	.fsync           = generic_file_fsync,
	.llseek          = generic_file_llseek,
	.splice_read     = filemap_splice_read,
	.splice_write    = iter_file_splice_write,
	.unlocked_ioctl  = obsidianfs_ioctl,
	.copy_file_range = obsidianfs_copy_file_range,
};
