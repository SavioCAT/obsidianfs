// GPL-2.0-only

#include <linux/fs.h>
//#include <linux/libfs.h>
#include <linux/mm.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"

const struct address_space_operations obsidianfs_page_ops = {
    .read_folio     = obsidian_read_folio, // Function pointer for reading a page from the page cache, used by the kernel for page-based read operations, return 0 if success, negative else
    .write_begin    = obsidian_write_begin, // Function pointer for preparing to write to a page in the page cache, used by the kernel for page-based write operations, return 0 if success, negative else
    .write_end      = obsidian_write_end, // Function pointer for finishing writing to a page in the page cache, used by the kernel for page-based write operations, return 0 if success, negative else
    .dirty_folio    = obsidian_dirty_folio, // Function pointer for marking a page in the page cache as dirty, used by the kernel for page-based write operations, return 0 if success, negative else
};

// Creation of file/directory
int obsidianfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev) {
    struct inode *inode = obsidianfs_get_inode(dir->i_sb, dir, mode, dev); // Allocate and initiate a new inode, return the pointer to the new inode if success, NULL else
    if (!inode){
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return -ENOSPC;
    }
    d_instantiate(dentry, inode); // Associate the dentry with the inode
    dget(dentry); // Increment the reference count of the dentry
    inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir)); // Update the modification time and the change time of the parent directory, happen if the mknod function succeed
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return 0;
}

struct dentry *obsidianfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
    int ret = obsidianfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0); // Create a directory by calling the mknod function with the S_IFDIR flag, return 0 if success, negative else
    if (!ret) {
        inc_nlink(dir); // Increment the link count of the parent directory, happen if the mknod function succeed
    }
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return NULL;
}

const struct file_operations obsidianfs_file_ops = {
    .read_iter   = generic_file_read_iter, // Function pointer for reading data from a file, used by the kernel for read system calls, return the number of bytes read if success, negative else
    .write_iter  = generic_file_write_iter, // Function pointer for writing data to a file, used by the kernel for write system calls, return the number of bytes written if success, negative else
    .mmap        = generic_file_mmap, // Function pointer for memory mapping a file, used by the kernel for mmap system calls, return 0 if success, negative else
    .fsync       = noop_fsync, // Function pointer for synchronizing a file's in-core state with storage, used by the kernel for fsync system calls, return 0 if success, negative else
    .llseek      = generic_file_llseek, // Function pointer for seeking within a file, used by the kernel for lseek system calls, return the new position if success, negative else
    .splice_read = filemap_splice_read, // Function pointer for reading data from a file using splice, used by the kernel for splice system calls, return the number of bytes read if success, negative else
};


/*
#include <linux/fs.h>
#include <linux/mm.h>
#include "ramobsidianfs.h"

static int obsidianfs_read_folio(struct file *file, struct folio *folio)
{
    folio_zero_range(folio, 0, folio_size(folio));
    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;
}

const struct address_space_operations obsidianfs_aops = {
    .read_folio  = obsidianfs_read_folio,
    .write_begin = simple_write_begin,
    .write_end   = simple_write_end_nolocking,
};

const struct file_operations obsidianfs_file_ops = {
    .read_iter   = generic_file_read_iter,
    .write_iter  = generic_file_write_iter,
    .mmap        = generic_file_mmap,
    .fsync       = noop_fsync,
    .llseek      = generic_file_llseek,
    .splice_read = filemap_splice_read,
};
*/
