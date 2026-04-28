#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/fs_context.h>
#include <linux/string.h>
#include "ramobsidianfs.h"

static const struct inode_operations obsidianfs_file_inode_ops = {
    .setattr = simple_setattr,
    .getattr = simple_getattr,
};

//Directory operation
static int obsidianfs_mknod(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, dev_t);
static int obsidianfs_create(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
static struct dentry *obsidianfs_mkdir(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname)
{
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return page_symlink(dir, symname, strlen(symname) + 1);
}


static const struct inode_operations obsidianfs_dir_inode_ops = {
    .create  = obsidianfs_create, // Function pointer to create a simple file
    .lookup  = simple_lookup, // Function pointer for file name resolution 
    .link    = simple_link, // Function pointer for physical link creation (ln src dst)
    .unlink  = simple_unlink, // Function pointer for physical link deletion
    .symlink = obsidianfs_symlink, // Function pointer for symbolic link creation (ln -s)
    .mkdir   = obsidianfs_mkdir, // Function pointer for directory creation
    .rmdir   = simple_rmdir, // Function pointer for directory deletion
    .mknod   = obsidianfs_mknod, // Function pointer for special file creation
    .rename  = simple_rename, // Function pointer for renaming a file
};

//Inode allocation
struct inode *obsidianfs_get_inode(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev) {
    struct inode *inode = new_inode(sb);
    if (!inode) {
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
	return NULL;
    }
    inode->i_ino            = get_next_ino();
    inode->i_mapping->a_ops = &empty_aops;
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

// Creation of file/directory
static int obsidianfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev) {
    struct inode *inode = obsidianfs_get_inode(dir->i_sb, dir, mode, dev);
    if (!inode){
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return -ENOSPC;
    }
    d_instantiate(dentry, inode);
    dget(dentry);
    inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return 0;
}

static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return obsidianfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static struct dentry *obsidianfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
    int ret = obsidianfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);
    if (!ret)
        inc_nlink(dir);
    return NULL;
}

// Super block operation
static const struct super_operations obsidianfs_sb_ops = {
    .statfs     = simple_statfs,
};

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

static const struct fs_context_operations obsidianfs_context_ops = {
    .get_tree = obsidianfs_get_tree,
};

static int obsidianfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &obsidianfs_context_ops;
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return 0;
}

static struct file_system_type obsidianfs_type = {
    .owner           = THIS_MODULE,
    .name            = "obsidianfs",
    .init_fs_context = obsidianfs_init_fs_context,
    .kill_sb         = kill_anon_super,
};

//Registration of the file system
static int __init obsidianfs_init(void) {
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return register_filesystem(&obsidianfs_type);
}

//Unregister the file system
static void __exit obsidianfs_exit(void) {
    pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    unregister_filesystem(&obsidianfs_type);
}

module_init(obsidianfs_init); //Call of the registration of the module
module_exit(obsidianfs_exit); //Call of the unregistration of the module

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ObsidianFS - simple RAM filesystem");
