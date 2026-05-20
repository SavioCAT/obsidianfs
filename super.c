#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"
#include "super.h"

static struct kmem_cache *obsidianfs_inode_cache;

/* ------------------------------------------------------------------ */
/* Inode slab                                                          */
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
/* Superblock operations                                               */
/* ------------------------------------------------------------------ */


// Write all the metadata of an inode from the memory to the disk
static int obsidianfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(inode->i_sb);
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	struct obsidianfs_inode *raw;
	struct buffer_head *bh;
	unsigned long block;
	unsigned long offset;
	unsigned int inode_size;
	unsigned int inodes_per_block;
	int ret = 0;

	inode_size = le32_to_cpu(sbi->s_es->s_inode_size);
	if (!inode_size)
		inode_size = sizeof(struct obsidianfs_inode);

	inodes_per_block = inode->i_sb->s_blocksize / inode_size;
	if (!inodes_per_block)
		return -EIO;

	block  = OBSIDIANFS_INODE_TABLE_BLOCK + (inode->i_ino - 1) / inodes_per_block;
	offset = ((inode->i_ino - 1) % inodes_per_block) * inode_size;

	bh = sb_bread(inode->i_sb, block);
	if (!bh) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot read block %lu for inode %lu\n",
		       __func__, block, inode->i_ino);
		return -EIO;
	}

	raw = (struct obsidianfs_inode *)(bh->b_data + offset);

	raw->i_mode        = cpu_to_le16(inode->i_mode);
	raw->i_uid         = cpu_to_le32(from_kuid(inode->i_sb->s_user_ns, inode->i_uid));
	raw->i_gid         = cpu_to_le32(from_kgid(inode->i_sb->s_user_ns, inode->i_gid));
	raw->i_links_count = cpu_to_le16(inode->i_nlink);
	raw->i_size        = cpu_to_le64(inode->i_size);
	raw->i_blocks      = cpu_to_le32(inode->i_blocks);
	raw->i_atime       = cpu_to_le32(inode_get_atime_sec(inode));
	raw->i_ctime       = cpu_to_le32(inode_get_ctime_sec(inode));
	raw->i_mtime       = cpu_to_le32(inode_get_mtime_sec(inode));
	memcpy(raw->i_block, oi->i_data, sizeof(raw->i_block));

	mark_buffer_dirty(bh);
	if (wbc->sync_mode == WB_SYNC_ALL) {
		sync_dirty_buffer(bh);
		if (!buffer_uptodate(bh))
			ret = -EIO;
	}
	brelse(bh);
	return ret;
}

static void obsidianfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static const struct super_operations obsidianfs_sb_ops = {
	.alloc_inode  = obsidianfs_alloc_inode,
	.free_inode   = obsidianfs_free_inode,
	.write_inode  = obsidianfs_write_inode,
	.evict_inode  = obsidianfs_evict_inode,
	.statfs       = simple_statfs,
};

/* ------------------------------------------------------------------ */
/* In-memory mount (OLD STYLE)                                        */
/* ------------------------------------------------------------------ */

/*
static int obsidianfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *root;

	sb->s_magic          = OBSIDIAN_MAGIC;
	sb->s_op             = &obsidianfs_sb_ops;
	sb->s_blocksize      = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_maxbytes       = MAX_LFS_FILESIZE;

	root = obsidianfs_create_inode_memory(sb, NULL, S_IFDIR | 0755, 0);
	if (!root) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot create root inode\n", __func__);
		return -ENOMEM;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot create root dentry\n", __func__);
		return -ENOMEM;
	}

	pr_info("[INFO OBSIDIANFS] superblock mounted (in-memory)\n");
	return 0;
}
*/

/* ------------------------------------------------------------------ */
/* Persistent (block-device) mount                                    */
/* ------------------------------------------------------------------ */

static int obsidianfs_fill_super_persistent(struct super_block *sb, struct fs_context *fc)
{
	struct obsidianfs_fs_context *ctx = fc->fs_private;
	struct obsidianfs_super_block *es;
	struct obsidianfs_sb_info *sbi;
	struct buffer_head *bh;
	struct inode *root;
	unsigned long blocksize;
	int ret = -EINVAL;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info  = sbi;

	if (ctx) {
    	sbi->s_sb_block = ctx->s_sb_block;
	} else {
    	sbi->s_sb_block = OBSIDIANFS_SB_BLOCK;
	}

	spin_lock_init(&sbi->s_lock); // Set the lock 

	// Step 1: set a safe minimum blocksize before the first sb_bread
	blocksize = sb_min_blocksize(sb, BLOCK_SIZE);
	if (!blocksize) {
		pr_err("[ERROR OBSIDIANFS] %s: device does not support block I/O\n", __func__);
		goto err_sbi;
	}

	// Step 2: read the on-disk superblock
	bh = sb_bread(sb, sbi->s_sb_block);
	if (!bh) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot read superblock at block %lu\n",
		       __func__, sbi->s_sb_block);
		ret = -EIO;
		goto err_sbi;
	}
	es = (struct obsidianfs_super_block *)bh->b_data;

	// Step 3: populate sbi from disk
	sbi->s_sbh             = bh;
	sbi->s_es              = es;
	sbi->s_blocks_count    = le32_to_cpu(es->s_blocks_count);
	sbi->s_inodes_count    = le32_to_cpu(es->s_inodes_count);
	sbi->s_first_data_block = le32_to_cpu(es->s_first_data_block);

	/*
	 * Step 4: switch to the block size stored on disk.
	 * sb_set_blocksize re-reads the device with the correct size.
	 * If it changed we must re-read the superblock buffer.
	 */
	blocksize = BLOCK_SIZE << le32_to_cpu(es->s_log_block_size);
	if (sb->s_blocksize != blocksize) {
		brelse(bh);
		if (!sb_set_blocksize(sb, blocksize)) {
			pr_err("[ERROR OBSIDIANFS] %s: unsupported blocksize %lu\n",
			       __func__, blocksize);
			goto err_sbi;
		}
		bh = sb_bread(sb, sbi->s_sb_block);
		if (!bh) {
			pr_err("[ERROR OBSIDIANFS] %s: cannot re-read superblock\n", __func__);
			ret = -EIO;
			goto err_sbi;
		}
		sbi->s_sbh = bh;
		sbi->s_es  = (struct obsidianfs_super_block *)bh->b_data;
	}

	// Step 5: wire up the VFS superblock
	sb->s_magic    = OBSIDIAN_MAGIC;
	sb->s_op       = &obsidianfs_sb_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	// Step 6: read root inode from disk 
	root = obsidianfs_iget(sb, OBSIDIANFS_ROOT_INO);
	if (IS_ERR(root)) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot read root inode\n", __func__);
		ret = PTR_ERR(root);
		goto err_bh;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		pr_err("[ERROR OBSIDIANFS] %s: d_make_root failed\n", __func__);
		ret = -ENOMEM;
		goto err_bh;
	}

	pr_info("[INFO OBSIDIANFS] persistent superblock mounted (%lu blocks, %lu inodes)\n",
		sbi->s_blocks_count, sbi->s_inodes_count);
	return 0;

err_bh:
	brelse(bh);
err_sbi:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

/* ------------------------------------------------------------------ */
/* fs_context — in-memory fs (OLD STYLE)                              */
/* ------------------------------------------------------------------ */

/*
static int obsidianfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, obsidianfs_fill_super);
}
*/

/*
static const struct fs_context_operations obsidianfs_context_ops = {
	.get_tree = obsidianfs_get_tree,
};
*/

/*
static int obsidianfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &obsidianfs_context_ops;
	return 0;
}
*/

/*
static struct file_system_type obsidianfs_type = {
	.owner           = THIS_MODULE,
	.name            = "obsidianfs",
	.init_fs_context = obsidianfs_init_fs_context,
	.kill_sb         = kill_anon_super,
};
*/

/* ------------------------------------------------------------------ */
/* fs_context — persistent fs (block device)                           */
/* ------------------------------------------------------------------ */

static int obsidianfs_persistent_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, obsidianfs_fill_super_persistent);
}

static const struct fs_context_operations obsidianfs_persistent_context_ops = {
	.get_tree = obsidianfs_persistent_get_tree,
};

static int obsidianfs_persistent_init_fs_context(struct fs_context *fc)
{
	struct obsidianfs_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->s_sb_block = OBSIDIANFS_SB_BLOCK;
	fc->fs_private  = ctx;
	fc->ops         = &obsidianfs_persistent_context_ops;
	return 0;
}

static void obsidianfs_persistent_kill_sb(struct super_block *sb)
{
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);

	if (sbi) {
		brelse(sbi->s_sbh);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
	kill_block_super(sb);
}

static struct file_system_type obsidianfs_persistent_type = {
	.owner           = THIS_MODULE,
	.name            = "obsidianfs_persistent",
	.init_fs_context = obsidianfs_persistent_init_fs_context,
	.kill_sb         = obsidianfs_persistent_kill_sb,
};

/* ------------------------------------------------------------------ */
/* Module init / exit                                                 */
/* ------------------------------------------------------------------ */

static int __init obsidianfs_init(void)
{
	int err;

	obsidianfs_inode_cache = kmem_cache_create("obsidian_inode_cache", sizeof(struct obsidianfs_inode_meta), 0, SLAB_RECLAIM_ACCOUNT, obsidianfs_i_init_once);
	if (!obsidianfs_inode_cache) {
		pr_err("[ERROR OBSIDIANFS] failed to create inode cache\n");
		return -ENOMEM;
	}

	/*
	err = register_filesystem(&obsidianfs_type);
	if (err)
		goto err_cache;
	*/

	err = register_filesystem(&obsidianfs_persistent_type);
	if (err)
		goto err_cache;

	pr_info("[INFO OBSIDIANFS] module loaded\n");
	return 0;

err_cache:
	kmem_cache_destroy(obsidianfs_inode_cache);
	return err;
}

static void __exit obsidianfs_exit(void)
{
	unregister_filesystem(&obsidianfs_persistent_type);
	//unregister_filesystem(&obsidianfs_type);
	kmem_cache_destroy(obsidianfs_inode_cache);
	pr_info("[INFO OBSIDIANFS] module unloaded\n");
}

module_init(obsidianfs_init);
module_exit(obsidianfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ObsidianFS - filesystem module");
