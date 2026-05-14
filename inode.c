// GPL-2.0-only

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/fs_context.h>
#include <linux/string.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"
#include <linux/ramfs.h>
//#include <linux/libfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/err.h>

static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
static struct inode *obsidianfs_alloc_inode(struct super_block *sb);
static void obsidianfs_free_inode(struct inode *inode);
static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname);
static int obsidianfs_fill_super(struct super_block *sb, struct fs_context *fc);
static int obsidianfs_get_tree(struct fs_context *fc);
static int obsidianfs_init_fs_context(struct fs_context *fc);
static int obsidianfs_hardlink(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
static int obsidianfs_unlink(struct inode *dir, struct dentry *dentry);
static int obsidian_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr);

static const struct inode_operations obsidianfs_file_inode_ops = {
    .setattr = obsidian_setattr,
    .getattr = simple_getattr,
};

// Super block operation
static const struct super_operations obsidianfs_sb_ops = {
    .statfs     = simple_statfs,
    .alloc_inode = obsidianfs_alloc_inode,
    .free_inode = obsidianfs_free_inode,
};

static const struct inode_operations obsidianfs_dir_inode_ops = {
    .create  = obsidianfs_create, // Function pointer to create a simple file
    .lookup  = simple_lookup, // Function pointer for file name resolution 
    .link    = obsidianfs_hardlink, // Function pointer for physical link creation (ln src dst)
    .unlink  = obsidianfs_unlink, // Function pointer for physical link deletion
    .symlink = obsidianfs_symlink, // Function pointer for symbolic link creation (ln -s)
    .mkdir   = obsidianfs_mkdir, // Function pointer for directory creation
    .rmdir   = simple_rmdir, // Function pointer for directory deletion
    .mknod   = obsidianfs_mknod, // Function pointer for special file creation
    .rename  = simple_rename, // Function pointer for renaming a file
};

static struct file_system_type obsidianfs_type = {
    .owner           = THIS_MODULE,
    .name            = "obsidianfs",
    .init_fs_context = obsidianfs_init_fs_context,
    .kill_sb         = kill_anon_super,
    .fs_flags		= FS_REQUIRES_DEV,
};

static const struct fs_context_operations obsidianfs_context_ops = {
    .get_tree = obsidianfs_get_tree,
};

static struct kmem_cache *obsidianfs_inode_cache;

static int obsidian_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	int error;

    struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);

    if (oi->flagsProtected) {
        pr_err("[ERROR OBSIDIANFS] error while calling %s: file is protected\n", __func__);
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

static struct inode *obsidianfs_alloc_inode(struct super_block *sb) {
    struct obsidianfs_inode_meta *oi;
    oi = kmem_cache_alloc(obsidianfs_inode_cache, GFP_KERNEL);
    if (!oi)
        return NULL;
    return &oi->vfs_inode;
}

//Inode allocation
struct inode *obsidianfs_get_inode(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev) {
    struct inode *inode = new_inode(sb);
    if (!inode) {
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
	    return NULL;
    }
    struct obsidianfs_inode_meta *obsidian_inode;
    obsidian_inode = OBSIDIANFS_INODE(inode);
    obsidian_inode->flagsProtected = false;
    obsidian_inode->valid_size     = 0;
    inode->i_ino            = get_next_ino();
    inode->i_mapping->a_ops = &obsidianfs_page_ops;
    inode->i_mode	    = mode;
    simple_inode_init_ts(inode);

    switch (mode & S_IFMT) { //Binary mask to isolate type bits from permission bits
    case S_IFREG: // Normal file
        inode->i_op  = &obsidianfs_file_inode_ops; // Pointer toward inode operation (file type)
        inode->i_fop = &obsidianfs_file_ops; // Pointer toward file operation
        break;
    case S_IFDIR: // Directory
        inode->i_op  = &obsidianfs_dir_inode_ops; // Pointer toward inode operation (directory type)
        inode->i_fop = &simple_dir_operations; // Pointer toward directory operation
        inc_nlink(inode);
        break;
    case S_IFLNK: // Symbolic link
        inode->i_op = &page_symlink_inode_operations;
        inode_nohighmem(inode); // Force the symlink in low memory mode
        break;
    default: // Any other case 
        init_special_inode(inode, mode, dev);
        break;
    }
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return inode;
}

static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return obsidianfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static void obsidianfs_free_inode(struct inode *inode) {
    kmem_cache_free(obsidianfs_inode_cache, OBSIDIANFS_INODE(inode));
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
}

static int obsidianfs_hardlink(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry); // Get the inode associated with the dentry

	inode_set_mtime_to_ts(dir, inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	inc_nlink(inode);
	igrab(inode);
	d_make_persistent(dentry, inode);
    mark_inode_dirty(dir);
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    pr_info("[INFO OBSIDIANFS] count of hardlink = %u\n", inode->i_nlink);
	return 0;
}

static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname) {
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    struct inode *inode = obsidianfs_get_inode(dir->i_sb, dir, S_IFLNK | 0700, 0);
    if (!inode){
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return -ENOSPC;
    }

    int error = page_symlink(inode, symname, strlen(symname) + 1); // Create a symbolic link by writing the target path into the page cache of the inode, return 0 if success, negative else 
    if (error) {
        iput(inode); // Decrement the reference count of the inode and free it if it reaches zero, happen if the page_symlink function fail
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return error;
    }

    d_instantiate(dentry, inode); // Associate the dentry with the inode
    dget(dentry); // Increment the reference count of the dentry
    return 0;
}

static int obsidianfs_unlink(struct inode *dir, struct dentry *dentry) {
    struct inode *inode = d_inode(dentry);
    struct obsidianfs_inode_meta *obsidian_inode = OBSIDIANFS_INODE(inode);
    if (obsidian_inode->flagsProtected) {
        pr_err("[ERROR OBSIDIANFS] error while calling %s: file is protected\n", __func__);
        return -EPERM;
    }
    inode_set_mtime_to_ts(dir, inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
    drop_nlink(inode);
    if (S_ISDIR(d_inode(dentry)->i_mode)) {
        drop_nlink(inode); // If the file is a directory, we need to drop the link count twice: once for the directory itself and once for the parent directory's link to it
    }
    mark_inode_dirty(inode);
    mark_inode_dirty(dir);
    return 0;
}

static int obsidianfs_fill_super(struct super_block *sb, struct fs_context *fc) {
    struct inode *root;

    sb->s_magic          = OBSIDIAN_MAGIC;
    sb->s_op             = &obsidianfs_sb_ops;
    sb->s_blocksize      = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_maxbytes       = MAX_LFS_FILESIZE;

    root = obsidianfs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
    if (!root){
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return -ENOMEM;
    }

    sb->s_root = d_make_root(root);
    if (!sb->s_root){
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return -ENOMEM;
    }

    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return 0;
}

static int obsidianfs_get_tree(struct fs_context *fc)
{
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return get_tree_nodev(fc, obsidianfs_fill_super);
}

static int obsidianfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &obsidianfs_context_ops;
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return 0;
}

static void obsidianfs_i_init_once(void *foo)
{
    struct obsidianfs_inode_meta *oi = foo;
    mutex_init(&oi->i_lock);
    inode_init_once(&oi->vfs_inode);
}

//Registration of the file system
static int __init obsidianfs_init(void) {
    obsidianfs_inode_cache = kmem_cache_create("obsidian_inode_cache", sizeof(struct obsidianfs_inode_meta), 0, SLAB_RECLAIM_ACCOUNT, obsidianfs_i_init_once);
    if (!obsidianfs_inode_cache) {
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
	    return -ENOMEM;
    }
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return register_filesystem(&obsidianfs_type);
}

//Unregister the file system
static void __exit obsidianfs_exit(void) {
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    kmem_cache_destroy(obsidianfs_inode_cache);
    unregister_filesystem(&obsidianfs_type);
}

module_init(obsidianfs_init); //Call of the registration of the module
module_exit(obsidianfs_exit); //Call of the unregistration of the module

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ObsidianFS - simple RAM filesystem");
