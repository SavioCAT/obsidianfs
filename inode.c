// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include "inode.h"
#include "file.h"
#include "pageops.h"
#include "super.h"

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
static int obsidianfs_hardlink(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname);
static int obsidianfs_unlink(struct inode *dir, struct dentry *dentry);
static int obsidian_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr);
static struct dentry *obsidianfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);
static int obsidianfs_rmdir(struct inode *dir, struct dentry *dentry);
static int obsidianfs_readdir(struct file *file, struct dir_context *ctx);
static int obsidianfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);

/* ------------------------------------------------------------------ */
/* inode_operations and file_operations tables                          */
/* ------------------------------------------------------------------ */

static const struct inode_operations obsidianfs_file_inode_ops = {
	.setattr = obsidian_setattr,
	.getattr = simple_getattr,
};

static const struct inode_operations obsidianfs_dir_inode_ops = {
	.create  = obsidianfs_create,
	.lookup  = obsidianfs_lookup,
	.link    = obsidianfs_hardlink,
	.unlink  = obsidianfs_unlink,
	.symlink = obsidianfs_symlink,
	.mkdir   = obsidianfs_mkdir,
	.rmdir   = obsidianfs_rmdir,
	.mknod   = obsidianfs_mknod,
	.rename  = obsidianfs_rename,
};

static const struct file_operations obsidianfs_dir_ops = {
	.iterate_shared = obsidianfs_readdir,
	.read           = generic_read_dir,
	.llseek         = generic_file_llseek,
	.fsync          = noop_fsync,
};

/* ------------------------------------------------------------------ */
/* Read inode from disk                                               */
/* ------------------------------------------------------------------ */

/**
 * obsidianfs_iget - Read an inode from disk
 * @sb: the super block
 * @ino: the inode number
 * Returns the inode, or an error pointer on failure.
 */

// DISK --> MEMORY
struct inode *obsidianfs_iget(struct super_block *sb, unsigned long ino)
{
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);
	struct obsidianfs_inode_meta *oi;
	struct obsidianfs_inode *raw;
	struct buffer_head *bh;
	struct inode *inode;
	unsigned long block;
	unsigned long offset;
	unsigned int inode_size;
	unsigned int inodes_per_block;

	if (ino < OBSIDIANFS_ROOT_INO || ino > sbi->s_inodes_count) {
		pr_err("[ERROR OBSIDIANFS] %s: inode %lu out of range (max %lu)\n", __func__, ino, sbi->s_inodes_count);
		return ERR_PTR(-EINVAL);
	}

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("[ERROR OBSIDIANFS] %s: iget_locked failed for inode %lu\n", __func__, ino);
		return ERR_PTR(-ENOMEM);
	}
		
	if (!(inode_state_read_once(inode) & I_NEW)) {
		return inode;
	}
		
	oi = OBSIDIANFS_INODE(inode);

	inode_size = le32_to_cpu(sbi->s_es->s_inode_size);
	if (!inode_size)
		inode_size = sizeof(struct obsidianfs_inode);

	inodes_per_block = sb->s_blocksize / inode_size;
	if (!inodes_per_block) {
		pr_err("[ERROR OBSIDIANFS] %s: inode size %u > block size %lu\n", __func__, inode_size, sb->s_blocksize);
		goto bad_inode;
	}

	block  = OBSIDIANFS_INODE_TABLE_BLOCK + (ino - 1) / inodes_per_block;
	offset = ((ino - 1) % inodes_per_block) * inode_size;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot read block %lu\n", __func__, block);
		goto bad_inode;
	}

	raw = (struct obsidianfs_inode *)(bh->b_data + offset);

	inode->i_mode   = le16_to_cpu(raw->i_mode);
	inode->i_uid    = make_kuid(sb->s_user_ns, le32_to_cpu(raw->i_uid));
	inode->i_gid    = make_kgid(sb->s_user_ns, le32_to_cpu(raw->i_gid));
	set_nlink(inode,  le16_to_cpu(raw->i_links_count));
	inode->i_size   = le64_to_cpu(raw->i_size);
	inode->i_blocks = le32_to_cpu(raw->i_blocks);
	inode_set_atime(inode, le32_to_cpu(raw->i_atime), 0);
	inode_set_ctime(inode, le32_to_cpu(raw->i_ctime), 0);
	inode_set_mtime(inode, le32_to_cpu(raw->i_mtime), 0);

	memcpy(oi->i_data, raw->i_block, sizeof(oi->i_data));

	oi->valid_size     = inode->i_size;
	oi->flagsProtected = raw->i_flagsProtected ? true : false;
	oi->i_block_group  = 0;

	brelse(bh);

	inode->i_mapping->a_ops = &obsidianfs_page_ops;
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op  = &obsidianfs_file_inode_ops;
		inode->i_fop = &obsidianfs_file_ops;
		break;
	case S_IFDIR:
		inode->i_op  = &obsidianfs_dir_inode_ops;
		inode->i_fop = &obsidianfs_dir_ops;
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	default:
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}

	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(-EIO);
}

/* ------------------------------------------------------------------ */
/* Inode bitmap allocation                                              */
/* ------------------------------------------------------------------ */

/**
 * obsidianfs_alloc_ino - Allocate an inode number
 * @sb: the super block
 * Returns the allocated inode number, or 0 on failure.
 */
unsigned long obsidianfs_alloc_ino(struct super_block *sb)
{
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);
	struct obsidianfs_super_block *es = sbi->s_es;
	unsigned long bits_per_bmap = sb->s_blocksize * 8UL;
	unsigned long total_inodes  = (unsigned long)OBSIDIANFS_INODE_BITMAP_SIZE * bits_per_bmap;
	unsigned long ino = 0;
	unsigned long cur;

	mutex_lock(&sbi->s_lock);

	if (le32_to_cpu(es->s_free_inodes_count) == 0)
		goto out;

	for (cur = 0; cur < total_inodes; ) {
		unsigned long bmap_idx = cur / bits_per_bmap;
		unsigned long bit_end  = min(bits_per_bmap, total_inodes - bmap_idx * bits_per_bmap);
		struct buffer_head *bmap_bh;
		unsigned long bit;

		bmap_bh = sb_bread(sb, OBSIDIANFS_INODE_BITMAP_BLOCK + bmap_idx);
		if (!bmap_bh) {
			pr_err("[ERROR OBSIDIANFS] %s: cannot read inode bitmap block %lu\n",
			       __func__, bmap_idx);
			goto out;
		}

		bit = find_next_zero_bit_le(bmap_bh->b_data, bit_end, 0);
		if (bit < bit_end) {
			__set_bit_le(bit, bmap_bh->b_data);
			le32_add_cpu(&es->s_free_inodes_count, -1);
			ino = bmap_idx * bits_per_bmap + bit + 1;
			mark_buffer_dirty(bmap_bh);
			brelse(bmap_bh);
			mark_buffer_dirty(sbi->s_sbh);
			break;
		}
		brelse(bmap_bh);
		cur = (bmap_idx + 1) * bits_per_bmap;
	}

out:
	mutex_unlock(&sbi->s_lock);
	return ino;
}

/*
 * obsidianfs_free_ino - Free an inode number
 * @sb: the super block
 * @ino: the inode number to free
 */
void obsidianfs_free_ino(struct super_block *sb, unsigned long ino)
{
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);
	struct obsidianfs_super_block *es = sbi->s_es;
	unsigned long bits_per_bmap = sb->s_blocksize * 8UL;
	unsigned long bit        = ino - 1;
	unsigned long bmap_idx   = bit / bits_per_bmap;
	unsigned long bit_in_blk = bit % bits_per_bmap;
	struct buffer_head *bmap_bh;

	if (ino < OBSIDIANFS_ROOT_INO || bmap_idx >= OBSIDIANFS_INODE_BITMAP_SIZE) {
		pr_err("[ERROR OBSIDIANFS] %s: inode %lu out of range\n", __func__, ino);
		return;
	}

	mutex_lock(&sbi->s_lock);

	bmap_bh = sb_bread(sb, OBSIDIANFS_INODE_BITMAP_BLOCK + bmap_idx);
	if (!bmap_bh) {
		pr_err("[ERROR OBSIDIANFS] %s: cannot read inode bitmap block %lu\n",
		       __func__, bmap_idx);
		mutex_unlock(&sbi->s_lock);
		return;
	}

	if (test_bit_le(bit_in_blk, bmap_bh->b_data)) {
		__clear_bit_le(bit_in_blk, bmap_bh->b_data);
		le32_add_cpu(&es->s_free_inodes_count, 1);
		mark_buffer_dirty(bmap_bh);
		mark_buffer_dirty(sbi->s_sbh);
	} else {
		pr_err("[ERROR OBSIDIANFS] %s: inode %lu already free\n", __func__, ino);
	}

	brelse(bmap_bh);
	mutex_unlock(&sbi->s_lock);
}

/* ------------------------------------------------------------------ */
/* Inode allocation                                                     */
/* ------------------------------------------------------------------ */

/*
 * obsidianfs_create_inode_memory - Create an inode in memory
 * @sb: the super block
 * @dir: the parent directory inode
 * @mode: the file mode
 * @dev: the device number
 * Returns the new inode, or NULL on failure.
 */
struct inode *obsidianfs_create_inode_memory(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	struct obsidianfs_inode_meta *oi;

	if (!inode) {
		pr_err("[ERROR OBSIDIANFS] %s: new_inode failed\n", __func__);
		return NULL;
	}

	oi = OBSIDIANFS_INODE(inode);
	oi->flagsProtected = false;
	oi->valid_size     = 0;

	inode->i_ino = obsidianfs_alloc_ino(sb);
	if (!inode->i_ino) {
		pr_err("[ERROR OBSIDIANFS] %s: inode bitmap full\n", __func__);
		iput(inode);
		return NULL;
	}
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
		inode->i_fop = &obsidianfs_dir_ops;
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
/* Directory block I/O helper                                          */
/* ------------------------------------------------------------------ */

/*
 * obsidianfs_dir_getblk - Get a directory block
 * @dir: the directory inode
 * @lblock: the logical block number
 * @create: whether to create the block if it doesn't exist
 * Returns the buffer head for the block, or NULL on failure.
 */
static struct buffer_head *obsidianfs_dir_getblk(struct inode *dir, sector_t lblock, int create)
{
	struct buffer_head bh_tmp = {};
	struct buffer_head *bh;
	int err;

	bh_tmp.b_size = dir->i_sb->s_blocksize;
	err = obsidianfs_get_block(dir, lblock, &bh_tmp, create);
	if (err || !buffer_mapped(&bh_tmp))
		return NULL;

	bh = sb_bread(dir->i_sb, bh_tmp.b_blocknr);
	if (!bh)
		return NULL;

	if (buffer_new(&bh_tmp)) {
		memset(bh->b_data, 0, dir->i_sb->s_blocksize);
		mark_buffer_dirty(bh);
	}
	return bh;
}

/* ------------------------------------------------------------------ */
/* Directory entry operations                                          */
/* ------------------------------------------------------------------ */

/*
 * obsidianfs_add_dir_entry - Add a directory entry
 * @dir: the directory inode
 * @qstr: the directory entry to add
 * @ino: the inode number
 * Returns 0 on success, or a negative error code on failure.
 */
int obsidianfs_add_dir_entry(struct inode *dir, const struct qstr *qstr, unsigned long ino)
{
	struct super_block *sb = dir->i_sb;
	unsigned int needed    = obsidianfs_dir_rec_len(qstr->len);
	unsigned long blksize  = sb->s_blocksize;
	unsigned long nblocks  = (dir->i_size + blksize - 1) >> sb->s_blocksize_bits;
	unsigned long blk;

	for (blk = 0; blk <= nblocks; blk++) {
		struct buffer_head *bh;
		struct obsidianfs_dir_entry *de;
		char *base, *end;
		unsigned int rec_len;
		int is_new = (blk == nblocks);

		bh = obsidianfs_dir_getblk(dir, blk, is_new);
		if (!bh)
			return -EIO;

		if (is_new) {
			de = (struct obsidianfs_dir_entry *)bh->b_data;
			de->ino      = 0;
			de->rec_len  = cpu_to_le16(blksize);
			de->name_len = 0;
			de->pad      = 0;
			dir->i_size += blksize;
			mark_inode_dirty(dir);
		}

		base = bh->b_data;
		end  = base + blksize;
		de   = (struct obsidianfs_dir_entry *)base;

		while ((char *)de < end) {
			rec_len = le16_to_cpu(de->rec_len);
			if (!rec_len || rec_len > (unsigned int)(end - (char *)de)) {
				brelse(bh);
				return -EIO;
			}

			if (!le32_to_cpu(de->ino) && rec_len >= needed) {
				de->ino      = cpu_to_le32(ino);
				de->name_len = qstr->len;
				de->pad      = 0;
				memcpy(de->name, qstr->name, qstr->len);
				mark_buffer_dirty(bh);
				brelse(bh);
				return 0;
			}

			if (le32_to_cpu(de->ino) &&
			    rec_len >= obsidianfs_dir_rec_len(de->name_len) + needed) {
				unsigned int used = obsidianfs_dir_rec_len(de->name_len);
				struct obsidianfs_dir_entry *new_de =
					(struct obsidianfs_dir_entry *)((char *)de + used);
				new_de->ino      = cpu_to_le32(ino);
				new_de->rec_len  = cpu_to_le16(rec_len - used);
				new_de->name_len = qstr->len;
				new_de->pad      = 0;
				memcpy(new_de->name, qstr->name, qstr->len);
				de->rec_len = cpu_to_le16(used);
				mark_buffer_dirty(bh);
				brelse(bh);
				return 0;
			}

			de = (struct obsidianfs_dir_entry *)((char *)de + rec_len);
		}
		brelse(bh);
	}
	return -ENOSPC;
}

/*
 * obsidianfs_remove_dir_entry - Remove a directory entry
 * @dir: the directory inode
 * @qstr: the directory entry to remove
 * Returns 0 on success, or a negative error code on failure.
 */
int obsidianfs_remove_dir_entry(struct inode *dir, const struct qstr *qstr)
{
	struct super_block *sb = dir->i_sb;
	unsigned long blksize  = sb->s_blocksize;
	unsigned long nblocks  = (dir->i_size + blksize - 1) >> sb->s_blocksize_bits;
	unsigned long blk;

	for (blk = 0; blk < nblocks; blk++) {
		struct buffer_head *bh;
		struct obsidianfs_dir_entry *de, *prev;
		char *base, *end;

		bh = obsidianfs_dir_getblk(dir, blk, 0);
		if (!bh)
			return -EIO;

		base = bh->b_data;
		end  = base + blksize;
		prev = NULL;
		de   = (struct obsidianfs_dir_entry *)base;

		while ((char *)de < end) {
			unsigned int rec_len = le16_to_cpu(de->rec_len);

			if (!rec_len || rec_len > (unsigned int)(end - (char *)de)) {
				brelse(bh);
				return -EIO;
			}

			if (le32_to_cpu(de->ino) &&
			    de->name_len == qstr->len &&
			    memcmp(de->name, qstr->name, qstr->len) == 0) {
				if (prev)
					le16_add_cpu(&prev->rec_len, rec_len);
				else
					de->ino = 0;
				mark_buffer_dirty(bh);
				brelse(bh);
				return 0;
			}

			prev = de;
			de   = (struct obsidianfs_dir_entry *)((char *)de + rec_len);
		}
		brelse(bh);
	}
	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Data block freeing                                                  */
/* ------------------------------------------------------------------ */

/*
 * obsidianfs_truncate_blocks - Truncate a file's data blocks
 * @inode: the inode to truncate
 */
void obsidianfs_truncate_blocks(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	struct buffer_head *ind_bh, *dind_bh;
	__le32 *ind_blocks, *dind_blocks;
	int i, j;

	for (i = 0; i < NDIR_BLOCKS; i++) {
		if (oi->i_data[i]) {
			obsidianfs_free_blocks(inode, le32_to_cpu(oi->i_data[i]), 1);
			oi->i_data[i] = 0;
		}
	}

	if (oi->i_data[OBSIDIAN_IND_BLOCK]) {
		ind_bh = sb_bread(sb, le32_to_cpu(oi->i_data[OBSIDIAN_IND_BLOCK]));
		if (ind_bh) {
			ind_blocks = (__le32 *)ind_bh->b_data;
			for (i = 0; i < (int)ADDR_PER_BLOCK(sb); i++) {
				if (ind_blocks[i])
					obsidianfs_free_blocks(inode, le32_to_cpu(ind_blocks[i]), 1);
			}
			brelse(ind_bh);
		}
		obsidianfs_free_blocks(inode, le32_to_cpu(oi->i_data[OBSIDIAN_IND_BLOCK]), 1);
		oi->i_data[OBSIDIAN_IND_BLOCK] = 0;
	}

	if (oi->i_data[OBSIDIAN_DIND_BLOCK]) {
		dind_bh = sb_bread(sb, le32_to_cpu(oi->i_data[OBSIDIAN_DIND_BLOCK]));
		if (dind_bh) {
			dind_blocks = (__le32 *)dind_bh->b_data;
			for (i = 0; i < (int)ADDR_PER_BLOCK(sb); i++) {
				if (!dind_blocks[i])
					continue;
				ind_bh = sb_bread(sb, le32_to_cpu(dind_blocks[i]));
				if (ind_bh) {
					ind_blocks = (__le32 *)ind_bh->b_data;
					for (j = 0; j < (int)ADDR_PER_BLOCK(sb); j++) {
						if (ind_blocks[j])
							obsidianfs_free_blocks(inode, le32_to_cpu(ind_blocks[j]), 1);
					}
					brelse(ind_bh);
				}
				obsidianfs_free_blocks(inode, le32_to_cpu(dind_blocks[i]), 1);
			}
			brelse(dind_bh);
		}
		obsidianfs_free_blocks(inode, le32_to_cpu(oi->i_data[OBSIDIAN_DIND_BLOCK]), 1);
		oi->i_data[OBSIDIAN_DIND_BLOCK] = 0;
	}

	/* Triple-indirect not implemented — files this large not expected */

	inode->i_size   = 0;
	inode->i_blocks = 0;
	mark_inode_dirty(inode);
}

/* ------------------------------------------------------------------ */
/* Directory inode operations                                           */
/* ------------------------------------------------------------------ */

/**
 * obsidianfs_create - Create a new file
 * @idmap: the mount ID map
 * @dir: the parent directory
 * @dentry: the directory entry for the new file
 * @mode: the file mode
 * @excl: whether to fail if the file already exists
 * Returns 0 on success, or a negative error code on failure.
 */
static int obsidianfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return obsidianfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

/**
 * obsidianfs_hardlink - Create a hard link
 * @old_dentry: the existing directory entry
 * @dir: the parent directory
 * @dentry: the new directory entry
 * Returns 0 on success, or a negative error code on failure.
 */
static int obsidianfs_hardlink(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int err;

	err = obsidianfs_add_dir_entry(dir, &dentry->d_name, inode->i_ino);
	if (err) {
		return err;
	}
	inode_set_mtime_to_ts(dir, inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	inc_nlink(inode);
	igrab(inode);
	d_instantiate(dentry, inode);
	mark_inode_dirty(dir);
	return 0;
}

/**
 * obsidianfs_symlink - Create a symbolic link
 * @idmap: the mount ID map
 * @dir: the parent directory
 * @dentry: the directory entry for the new link
 * @symname: the target of the symbolic link
 * Returns 0 on success, or a negative error code on failure.
 */
static int obsidianfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error;

	inode = obsidianfs_create_inode_memory(dir->i_sb, dir, S_IFLNK | 0700, 0);
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

	error = obsidianfs_add_dir_entry(dir, &dentry->d_name, inode->i_ino);
	if (error) {
		iput(inode);
		return error;
	}

	d_instantiate(dentry, inode);
	dget(dentry);
	return 0;
}

/*
 * obsidianfs_unlink - Remove a directory entry
 * @dir: the parent directory
 * @dentry: the directory entry to remove
 * Returns 0 on success, or a negative error code on failure.
 */
static int obsidianfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	int err;

	if (oi->flagsProtected) {
		pr_err("[ERROR OBSIDIANFS] %s: file is protected\n", __func__);
		return -EPERM;
	}

	err = obsidianfs_remove_dir_entry(dir, &dentry->d_name);
	if (err)
		return err;

	inode_set_mtime_to_ts(dir,
		inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	drop_nlink(inode);
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	return 0;
}

/*
 * obsidianfs_setattr - Set file attributes
 * @idmap: the mount ID map
 * @dentry: the directory entry
 * @iattr: the attribute structure
 * Returns 0 on success, or a negative error code on failure.
 */
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

/*
 * obsidianfs_dir_is_empty - Check if a directory is empty
 * @dir: the directory inode
 * Returns 1 if the directory is empty, 0 otherwise.
 */
static int obsidianfs_dir_is_empty(struct inode *dir)
{
	struct super_block *sb = dir->i_sb;
	unsigned long blksize  = sb->s_blocksize;
	unsigned long nblocks  = (dir->i_size + blksize - 1) >> sb->s_blocksize_bits;
	unsigned long blk;

	for (blk = 0; blk < nblocks; blk++) {
		struct buffer_head *bh;
		struct obsidianfs_dir_entry *de;
		char *base, *end;

		bh = obsidianfs_dir_getblk(dir, blk, 0);
		if (!bh)
			return 0;

		base = bh->b_data;
		end  = base + blksize;
		de   = (struct obsidianfs_dir_entry *)base;

		while ((char *)de < end) {
			unsigned int rec_len = le16_to_cpu(de->rec_len);

			if (!rec_len)
				break;

			if (le32_to_cpu(de->ino) &&
			    !(de->name_len == 1 && de->name[0] == '.') &&
			    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
				brelse(bh);
				return 0;
			}
			de = (struct obsidianfs_dir_entry *)((char *)de + rec_len);
		}
		brelse(bh);
	}
	return 1;
}

/*
 * obsidianfs_lookup - Look up a directory entry
 * @dir: the directory inode
 * @dentry: the directory entry to look up
 * @flags: lookup flags
 * Returns the dentry if found, or an error code on failure.
 */
static struct dentry *obsidianfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	unsigned long blksize  = sb->s_blocksize;
	unsigned long nblocks  = (dir->i_size + blksize - 1) >> sb->s_blocksize_bits;
	struct inode *inode    = NULL;
	unsigned long blk;

	for (blk = 0; blk < nblocks && !inode; blk++) {
		struct buffer_head *bh;
		struct obsidianfs_dir_entry *de;
		char *base, *end;

		bh = obsidianfs_dir_getblk(dir, blk, 0);
		if (!bh)
			continue;

		base = bh->b_data;
		end  = base + blksize;
		de   = (struct obsidianfs_dir_entry *)base;

		while ((char *)de < end) {
			unsigned int rec_len = le16_to_cpu(de->rec_len);

			if (!rec_len)
				break;

			if (le32_to_cpu(de->ino) &&
			    de->name_len == dentry->d_name.len &&
			    memcmp(de->name, dentry->d_name.name, dentry->d_name.len) == 0) {
				inode = obsidianfs_iget(sb, le32_to_cpu(de->ino));
				break;
			}
			de = (struct obsidianfs_dir_entry *)((char *)de + rec_len);
		}
		brelse(bh);
	}

	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

/*
 * obsidianfs_readdir - Read directory entries
 * @file: the directory file being read
 * @ctx: the directory context to fill with entries
 * Returns 0 on success, or a negative error code on failure.
 */
static int obsidianfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir      = file_inode(file);
	struct super_block *sb = dir->i_sb;
	unsigned long blksize  = sb->s_blocksize;
	unsigned long nblocks  = (dir->i_size + blksize - 1) >> sb->s_blocksize_bits;
	loff_t byte_off;
	unsigned long first_blk, blk;

	if (!dir_emit_dots(file, ctx))
		return 0;

	/* ctx->pos >= 2; byte offset into dir data = ctx->pos - 2 */
	byte_off  = ctx->pos - 2;
	first_blk = (unsigned long)(byte_off >> sb->s_blocksize_bits);

	for (blk = first_blk; blk < nblocks; blk++) {
		struct buffer_head *bh;
		struct obsidianfs_dir_entry *de;
		char *base, *p, *end;
		unsigned long start_off;

		bh = obsidianfs_dir_getblk(dir, blk, 0);
		if (!bh) {
			ctx->pos = 2 + (loff_t)(blk + 1) * blksize;
			continue;
		}

		base      = bh->b_data;
		end       = base + blksize;
		start_off = (blk == first_blk) ? (unsigned long)(byte_off & (blksize - 1)) : 0;
		p         = base + start_off;

		while (p < end) {
			unsigned int rec_len;

			de      = (struct obsidianfs_dir_entry *)p;
			rec_len = le16_to_cpu(de->rec_len);

			if (!rec_len || rec_len > (unsigned int)(end - p)) {
				brelse(bh);
				return -EIO;
			}

			if (le32_to_cpu(de->ino)) {
				if (!dir_emit(ctx, de->name, de->name_len,
					      le32_to_cpu(de->ino), DT_UNKNOWN)) {
					brelse(bh);
					return 0;
				}
			}
			p += rec_len;
			ctx->pos = 2 + (loff_t)blk * blksize + (p - base);
		}
		brelse(bh);
	}
	return 0;
}

/*
 * obsidianfs_rmdir - Remove a directory
 * @dir: the parent directory
 * @dentry: the directory entry to remove
 * Returns 0 on success, or a negative error code on failure.
 */
static int obsidianfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	if (!obsidianfs_dir_is_empty(inode))
		return -ENOTEMPTY;

	err = obsidianfs_remove_dir_entry(dir, &dentry->d_name);
	if (err)
		return err;

	inode_set_mtime_to_ts(dir,
		inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	drop_nlink(inode);
	drop_nlink(inode);
	drop_nlink(dir);
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Rename                                                               */
/* ------------------------------------------------------------------ */

static int obsidianfs_rename(struct mnt_idmap *idmap,
			      struct inode *old_dir, struct dentry *old_dentry,
			      struct inode *new_dir, struct dentry *new_dentry,
			      unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (new_inode) {
		if (S_ISDIR(old_inode->i_mode)) {
			if (!S_ISDIR(new_inode->i_mode))
				return -ENOTDIR;
			if (!obsidianfs_dir_is_empty(new_inode))
				return -ENOTEMPTY;
		}
		err = obsidianfs_remove_dir_entry(new_dir, &new_dentry->d_name);
		if (err)
			return err;
		if (S_ISDIR(new_inode->i_mode)) {
			drop_nlink(new_inode);
			drop_nlink(new_dir);
		}
		drop_nlink(new_inode);
		mark_inode_dirty(new_inode);
	}

	err = obsidianfs_remove_dir_entry(old_dir, &old_dentry->d_name);
	if (err)
		return err;

	err = obsidianfs_add_dir_entry(new_dir, &new_dentry->d_name, old_inode->i_ino);
	if (err) {
		/* Best-effort rollback: re-add the old entry to avoid losing the file */
		obsidianfs_add_dir_entry(old_dir, &old_dentry->d_name, old_inode->i_ino);
		return err;
	}

	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}

	inode_set_mtime_to_ts(old_dir, inode_set_ctime_current(old_dir));
	if (old_dir != new_dir)
		inode_set_mtime_to_ts(new_dir, inode_set_ctime_current(new_dir));
	mark_inode_dirty(old_dir);
	mark_inode_dirty(new_dir);
	mark_inode_dirty(old_inode);
	return 0;
}
