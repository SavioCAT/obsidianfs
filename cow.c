#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include "cow.h"

struct inode *obsidianfs_cow_inode(struct inode *old_inode, struct dentry *dentry);
static int obsidianfs_copy_blocks(struct inode *old_inode, struct inode *new_inode);
static int obsidianfs_max_backup(struct inode *inode);

// Create a new inode, exactly like the previous inode. Useful for CoW
struct inode *obsidianfs_cow_inode(struct inode *old_inode, struct dentry *dentry)
{
	struct super_block *sb = old_inode->i_sb;
	struct obsidianfs_inode_meta *old_oi = OBSIDIANFS_INODE(old_inode);
    
	struct inode *new_inode;
	struct obsidianfs_inode_meta *new_oi;
	int ret;

	ret = filemap_write_and_wait(old_inode->i_mapping);
	if (ret) {
		return ERR_PTR(ret);
	}

	new_inode = obsidianfs_create_inode_memory(sb, old_inode, old_inode->i_mode, 0);
	if (!new_inode) {
		return ERR_PTR(-ENOSPC);
	}

	new_oi = OBSIDIANFS_INODE(new_inode);
	new_inode->i_uid       = old_inode->i_uid;
	new_inode->i_gid       = old_inode->i_gid;
	new_inode->i_size      = old_inode->i_size;
	new_oi->valid_size     = old_oi->valid_size;
	new_oi->flagsProtected = true;

	ret = obsidianfs_copy_blocks(old_inode, new_inode);
	if (ret) {
		iput(new_inode);
		return ERR_PTR(ret);
	}

	new_inode->i_blocks      = old_inode->i_blocks;
	new_oi->i_previous_inode = old_oi->i_previous_inode;

	new_oi->i_next_inode     = old_inode->i_ino;

	if (old_oi->i_previous_inode) {
		struct inode *old_old_inode = obsidianfs_iget(sb, old_oi->i_previous_inode);
		if (!IS_ERR(old_old_inode)) {
			struct obsidianfs_inode_meta *old_old_oi = OBSIDIANFS_INODE(old_old_inode);
			old_old_oi->i_next_inode = new_inode->i_ino;
			mark_inode_dirty(old_old_inode);
			iput(old_old_inode);
		}
	}

	set_nlink(new_inode, old_inode->i_nlink);
	mark_inode_dirty(new_inode);

	old_oi->i_previous_inode = new_inode->i_ino;

	d_drop(dentry);

	mark_inode_dirty(old_inode);
	mark_inode_dirty(new_inode);

	iput(new_inode);

	obsidianfs_max_backup(old_inode);

	return old_inode;
}

// Copy every block from an old inode to a new inode
static int obsidianfs_copy_blocks(struct inode *old_inode, struct inode *new_inode)
{
	struct super_block *sb = old_inode->i_sb;
	unsigned long blocksize = sb->s_blocksize;
	unsigned long nblocks;
	unsigned long lblock;

	if (old_inode->i_size == 0)
		return 0;

	nblocks = (old_inode->i_size + blocksize - 1) >> sb->s_blocksize_bits;

	for (lblock = 0; lblock < nblocks; lblock++) {
		struct buffer_head *bh_tmp;
		struct buffer_head *bh_dst;
		struct folio *src_folio;
		void *src_kaddr;
		sector_t new_pblock;
		int err;

		bh_tmp = kzalloc(sizeof(*bh_tmp), GFP_NOFS);
		if (!bh_tmp)
			return -ENOMEM;
		bh_tmp->b_size = blocksize;
		err = obsidianfs_get_block(old_inode, lblock, bh_tmp, 0);
		if (err || !buffer_mapped(bh_tmp)) {
			kfree(bh_tmp);
			continue;
		}
		kfree(bh_tmp);

		src_folio = read_mapping_folio(old_inode->i_mapping, lblock, NULL);
		if (IS_ERR(src_folio)) {
			return PTR_ERR(src_folio);
		}

		bh_tmp = kzalloc(sizeof(*bh_tmp), GFP_NOFS);
		if (!bh_tmp) {
			folio_put(src_folio);
			return -ENOMEM;
		}

		bh_tmp->b_size = blocksize;
		err = obsidianfs_get_block(new_inode, lblock, bh_tmp, 1);
		if (err || !buffer_mapped(bh_tmp)) {
			kfree(bh_tmp);
			folio_put(src_folio);
			return err ? err : -EIO;
		}

		new_pblock = bh_tmp->b_blocknr;
		kfree(bh_tmp);

		bh_dst = sb_getblk(sb, new_pblock);
		if (!bh_dst) {
			folio_put(src_folio);
			return -ENOMEM;
		}

		src_kaddr = kmap_local_folio(src_folio, 0);
		lock_buffer(bh_dst);
		memcpy(bh_dst->b_data, src_kaddr, blocksize);
		set_buffer_uptodate(bh_dst);
		unlock_buffer(bh_dst);
		kunmap_local(src_kaddr);
		mark_buffer_dirty(bh_dst);
		brelse(bh_dst);
		folio_put(src_folio);
	}
	return sync_blockdev(sb->s_bdev);
}

static int obsidianfs_max_backup(struct inode *inode) {
	int number_of_backup = 0;
	struct super_block *sb = inode->i_sb;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	unsigned long previous_ino = oi->i_previous_inode;
	struct obsidianfs_inode_meta *previous_oi;

	while(previous_ino && number_of_backup < OBSIDIANFS_MAX_BACKUPS) {
		struct inode *previous_inode = obsidianfs_iget(sb, previous_ino);
		if (IS_ERR(previous_inode)) {
			return PTR_ERR(previous_inode);
		}

		previous_oi = OBSIDIANFS_INODE(previous_inode);
		previous_ino = previous_oi->i_previous_inode;
		number_of_backup++;

		if(number_of_backup == OBSIDIANFS_MAX_BACKUPS) {
			previous_oi->i_previous_inode = 0;
			mark_inode_dirty(previous_inode);
		}

		iput(previous_inode);
	}

	while(previous_ino != 0) {
		struct inode *previous_inode = obsidianfs_iget(sb, previous_ino);
		if (IS_ERR(previous_inode)) {
			return PTR_ERR(previous_inode);
		}
		previous_oi = OBSIDIANFS_INODE(previous_inode);
		previous_ino = previous_oi->i_previous_inode;

		previous_oi->i_previous_inode = 0;
		previous_oi->i_next_inode     = 0;
		clear_nlink(previous_inode);
		mark_inode_dirty(previous_inode);

		iput(previous_inode);
	}

	return 0;
}