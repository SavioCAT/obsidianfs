// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"

static struct kmem_cache *obsidianfs_inode_cache;

/* ------------------------------------------------------------------ */
/* Inode slab                                                           */
/* ------------------------------------------------------------------ */

static struct inode *obsidianfs_alloc_inode(struct super_block *sb)
{
	struct obsidianfs_inode_meta *oi;

	oi = kmem_cache_alloc(obsidianfs_inode_cache, GFP_KERNEL);
	if (!oi)
		return NULL;
	return &oi->vfs_inode;
}

static void obsidianfs_free_inode(struct inode *inode)
{
	kmem_cache_free(obsidianfs_inode_cache, OBSIDIANFS_INODE(inode));
}

static void obsidianfs_i_init_once(void *foo)
{
	struct obsidianfs_inode_meta *oi = foo;

	mutex_init(&oi->i_lock);
	rwlock_init(&oi->readwritelock);
	inode_init_once(&oi->vfs_inode);
}

/* ------------------------------------------------------------------ */
/* Superblock                                                           */
/* ------------------------------------------------------------------ */

static const struct super_operations obsidianfs_sb_ops = {
	.statfs      = simple_statfs,
	.alloc_inode = obsidianfs_alloc_inode,
	.free_inode  = obsidianfs_free_inode,
};

static int obsidianfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *root;

	sb->s_magic          = OBSIDIAN_MAGIC;
	sb->s_op             = &obsidianfs_sb_ops;
	sb->s_blocksize      = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_maxbytes       = MAX_LFS_FILESIZE;

	root = obsidianfs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
	if (!root) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot create root inode\n", __func__);
		return -ENOMEM;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot create root dentry\n", __func__);
		return -ENOMEM;
	}

	pr_info("[INFO OBSIDIANFS] superblock mounted\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* fs_context                                                           */
/* ------------------------------------------------------------------ */

static int obsidianfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, obsidianfs_fill_super);
}

static const struct fs_context_operations obsidianfs_context_ops = {
	.get_tree = obsidianfs_get_tree,
};

static int obsidianfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &obsidianfs_context_ops;
	return 0;
}

static struct file_system_type obsidianfs_type = {
	.owner           = THIS_MODULE,
	.name            = "obsidianfs",
	.init_fs_context = obsidianfs_init_fs_context,
	.kill_sb         = kill_anon_super,
};

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init obsidianfs_init(void)
{
	int err;

	obsidianfs_inode_cache = kmem_cache_create("obsidian_inode_cache",
		sizeof(struct obsidianfs_inode_meta), 0,
		SLAB_RECLAIM_ACCOUNT, obsidianfs_i_init_once);
	if (!obsidianfs_inode_cache) {
		pr_err("[ERROR OBSIDIANFS] failed to create inode cache\n");
		return -ENOMEM;
	}

	err = register_filesystem(&obsidianfs_type);
	if (err) {
		kmem_cache_destroy(obsidianfs_inode_cache);
		return err;
	}

	pr_info("[INFO OBSIDIANFS] module loaded\n");
	return 0;
}

static void __exit obsidianfs_exit(void)
{
	unregister_filesystem(&obsidianfs_type);
	kmem_cache_destroy(obsidianfs_inode_cache);
	pr_info("[INFO OBSIDIANFS] module unloaded\n");
}

module_init(obsidianfs_init);
module_exit(obsidianfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ObsidianFS - filesystem module");
