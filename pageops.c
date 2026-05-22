// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/time.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include "inode.h"
#include "pageops.h"
#include "super.h"

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

int obsidianfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);
static int obsidianfs_get_blocks(struct inode *inode, sector_t iblock, unsigned long maxblocks, u32 *bno, bool *new, bool *boundary, int create);
static int obsidian_block_to_path(struct inode *inode, long i_block, int offsets[4], int *boundary);
static Indirect *obsidianfs_get_branch(struct inode *inode, int depth, int *offsets, Indirect chain[4], int *err);
static obsidianfs_fsblk_t obsidianfs_group_first_block_no(struct super_block *sb, unsigned long group_no);
static unsigned long obsidianfs_find_near(struct inode *inode, Indirect *ind);
static unsigned long obsidianfs_find_goal(struct inode *inode, long block, Indirect *partial);
static int obsidianfs_blks_to_allocate(Indirect *branch, int k, unsigned long blks, int blocks_to_boundary);
static int obsidianfs_alloc_branch(struct inode *inode, int indirect_blks, int *blks, unsigned long goal, int *offsets, Indirect *branch);
static int obsidianfs_alloc_blocks(struct inode *inode, obsidianfs_fsblk_t goal, int indirect_blks, int blks, obsidianfs_fsblk_t new_blocks[4], int *err);
void obsidianfs_free_blocks(struct inode *inode, obsidianfs_fsblk_t block, unsigned long count);
static void obsidianfs_splice_branch(struct inode *inode, long block, Indirect *where, int num, int blks);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v)
{
	p->key = *(p->p = v);
	p->bh  = bh;
}

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

/* ------------------------------------------------------------------ */
/* Page cache read/write                                              */
/* ------------------------------------------------------------------ */

int obsidian_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, obsidianfs_get_block);
}

int obsidian_write_begin(const struct kiocb *iocb, struct address_space *mapping,
			 loff_t pos, unsigned len,
			 struct folio **foliop, void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop, obsidianfs_get_block);
}

int obsidian_write_end(const struct kiocb *iocb, struct address_space *mapping,
		       loff_t pos, unsigned int len, unsigned int copied,
		       struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	int ret = generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);

	if (ret > 0) {
		loff_t last = pos + ret;

		mutex_lock(&oi->i_lock);
		if (last > oi->valid_size)
			oi->valid_size = last;
		mutex_unlock(&oi->i_lock);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* Block path resolution (ext2-style indirect blocks)                 */
/* ------------------------------------------------------------------ */

/*
 * Map a logical block number to a path of indices into i_data[] and
 * indirect blocks. Returns the depth (1–4), or 0 on error.
 */
static int obsidian_block_to_path(struct inode *inode, long i_block, int offsets[4], int *boundary)
{
	int ptrs      = ADDR_PER_BLOCK(inode->i_sb);
	int ptrs_bits = ADDR_PER_BLOCK_BITS(inode->i_sb);
	const long direct_blocks   = NDIR_BLOCKS;
	const long indirect_blocks = ptrs;
	const long double_blocks   = (1 << (ptrs_bits * 2));
	int n     = 0;
	int final = 0;

	if (i_block < 0) {
		pr_err("[ERROR OBSIDIANFS] %s: negative block %ld\n",
		       __func__, i_block);
	} else if (i_block < direct_blocks) {
		offsets[n++] = i_block;
		final = direct_blocks;
	} else if ((i_block -= direct_blocks) < indirect_blocks) {
		offsets[n++] = OBSIDIAN_IND_BLOCK;
		offsets[n++] = i_block;
		final = ptrs;
	} else if ((i_block -= indirect_blocks) < double_blocks) {
		offsets[n++] = OBSIDIAN_DIND_BLOCK;
		offsets[n++] = i_block >> ptrs_bits;
		offsets[n++] = i_block & (ptrs - 1);
		final = ptrs;
	} else if (((i_block -= double_blocks) >> (ptrs_bits * 2)) < ptrs) {
		offsets[n++] = OBSIDIAN_TIND_BLOCK;
		offsets[n++] = i_block >> (ptrs_bits * 2);
		offsets[n++] = (i_block >> ptrs_bits) & (ptrs - 1);
		offsets[n++] = i_block & (ptrs - 1);
		final = ptrs;
	} else {
		pr_err("[ERROR OBSIDIANFS] %s: block %ld out of range\n", __func__, i_block);
	}

	if (boundary)
		*boundary = final - 1 - (i_block & (ptrs - 1));

	return n;
}

/*
 * Walk the indirect-block chain. Returns NULL if the chain is complete
 * (block found), or a pointer to the first missing Indirect entry.
 */
static Indirect *obsidianfs_get_branch(struct inode *inode, int depth, int *offsets, Indirect chain[4], int *err)
{
	struct super_block *sb = inode->i_sb;
	Indirect *p = chain;
	struct buffer_head *bh;

	*err = 0;
	add_chain(chain, NULL, OBSIDIANFS_INODE(inode)->i_data + *offsets);
	if (!p->key)
		goto no_block;

	while (--depth) {
		bh = sb_bread(sb, le32_to_cpu(p->key));
		if (!bh)
			goto failure;
		read_lock(&OBSIDIANFS_INODE(inode)->readwritelock);
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, (__le32 *)bh->b_data + *++offsets);
		read_unlock(&OBSIDIANFS_INODE(inode)->readwritelock);
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	read_unlock(&OBSIDIANFS_INODE(inode)->readwritelock);
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;
failure:
	*err = -EIO;
no_block:
	return p;
}

/* ------------------------------------------------------------------ */
/* Block allocation helpers                                           */
/* ------------------------------------------------------------------ */

/* TODO: requires on-disk block group layout in s_fs_info */
static obsidianfs_fsblk_t obsidianfs_group_first_block_no(struct super_block *sb, unsigned long group_no)
{
	return 0;
}

static unsigned long obsidianfs_find_near(struct inode *inode, Indirect *ind)
{
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	__le32 *start = ind->bh ? (__le32 *)ind->bh->b_data : oi->i_data;
	__le32 *p;
	unsigned long bg_start;
	unsigned long colour;

	for (p = ind->p - 1; p >= start; p--)
		if (*p)
			return le32_to_cpu(*p);

	if (ind->bh)
		return ind->bh->b_blocknr;

	/* TODO: use BLOCKS_PER_GROUP once s_fs_info is populated */
	bg_start = obsidianfs_group_first_block_no(inode->i_sb, oi->i_block_group);
	colour   = current->pid % 16;
	return bg_start + colour;
}

static unsigned long obsidianfs_find_goal(struct inode *inode, long block, Indirect *partial)
{
	struct obsidianfs_block_alloc_info *block_i =
		OBSIDIANFS_INODE(inode)->i_block_alloc_info;

	if (block_i &&
	    block == block_i->last_alloc_logical_block + 1 &&
	    block_i->last_alloc_physical_block != 0)
		return block_i->last_alloc_physical_block + 1;

	return obsidianfs_find_near(inode, partial);
}

void obsidianfs_init_block_alloc_info(struct inode *inode)
{
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	struct obsidianfs_block_alloc_info *block_i;
	struct obsidianfs_reserve_window_node *rsv;

	block_i = kmalloc(sizeof(*block_i), GFP_KERNEL);
	if (!block_i)
		return;

	rsv = &block_i->rsv_window_node;
	rsv->rsv_window._rsv_start = 0;
	rsv->rsv_window._rsv_end   = 0;
	/* reservations disabled until mount options are wired up */
	rsv->rsv_goal_size  = 0;
	rsv->rsv_alloc_hit  = 0;
	block_i->last_alloc_logical_block  = 0;
	block_i->last_alloc_physical_block = 0;

	oi->i_block_alloc_info = block_i;
}

/*
 * Count how many direct blocks to allocate for a single branch request.
 */
static int obsidianfs_blks_to_allocate(Indirect *branch, int k, unsigned long blks, int blocks_to_boundary)
{
	unsigned long count = 0;

	if (k > 0) {
		count = min(blks, (unsigned long)(blocks_to_boundary + 1));
		return count;
	}
	count++;
	while (k < 0 && count < blks &&
	       count <= (unsigned long)blocks_to_boundary) {
		++branch->p;
		count++;
		k++;
	}
	return count;
}

static int obsidianfs_alloc_branch(struct inode *inode, int indirect_blks,
				    int *blks, unsigned long goal,
				    int *offsets, Indirect *branch)
{
	int blocksize = inode->i_sb->s_blocksize;
	int i, n = 0;
	int err = 0;
	struct buffer_head *bh;
	int num;
	obsidianfs_fsblk_t new_blocks[4];
	obsidianfs_fsblk_t current_block;

	num = obsidianfs_alloc_blocks(inode, goal, indirect_blks, *blks,
				      new_blocks, &err);
	if (err)
		return err;

	branch[0].key = cpu_to_le32(new_blocks[0]);
	for (n = 1; n <= indirect_blks; n++) {
		bh = sb_getblk(inode->i_sb, new_blocks[n - 1]);
		if (unlikely(!bh)) {
			err = -ENOMEM;
			goto failed;
		}
		branch[n].bh = bh;
		lock_buffer(bh);
		memset(bh->b_data, 0, blocksize);
		branch[n].p = (__le32 *)bh->b_data + offsets[n];
		branch[n].key = cpu_to_le32(new_blocks[n]);
		*branch[n].p = branch[n].key;
		if (n == indirect_blks) {
			current_block = new_blocks[n];
			for (i = 1; i < num; i++)
				*(branch[n].p + i) = cpu_to_le32(++current_block);
		}
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
		if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
			sync_dirty_buffer(bh);
	}
	*blks = num;
	return err;

failed:
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < indirect_blks; i++)
		obsidianfs_free_blocks(inode, new_blocks[i], 1);
	obsidianfs_free_blocks(inode, new_blocks[i], num);
	return err;
}

/*
 * Allocate indirect_blks metadata blocks and blks contiguous data blocks
 * from the block bitmap, starting near 'goal'.
 *
 * new_blocks[0..indirect_blks-1] : one metadata block each
 * new_blocks[indirect_blks]      : first data block (next blks-1 are consecutive)
 *
 * Returns the number of data blocks allocated, 0 on error (*err set).
 */
static int obsidianfs_alloc_blocks(struct inode *inode, obsidianfs_fsblk_t goal,
				    int indirect_blks, int blks,
				    obsidianfs_fsblk_t new_blocks[4], int *err)
{
	struct super_block *sb = inode->i_sb;
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);
	struct obsidianfs_super_block *es = sbi->s_es;
	unsigned long first_data    = le32_to_cpu(es->s_first_data_block);
	unsigned long blocks_count  = le32_to_cpu(es->s_blocks_count);
	unsigned long nr_bits       = blocks_count - first_data;
	unsigned long bits_per_bmap = sb->s_blocksize * 8UL;
	int total     = indirect_blks + blks;
	int allocated = 0;
	int ret       = 0;
	unsigned long search_start;
	int j;

	*err = 0;

	if (unlikely(total <= 0 || total > 4 || blks <= 0 || nr_bits == 0)) {
		*err = -EINVAL;
		return 0;
	}

	if (goal < first_data || goal >= blocks_count)
		goal = first_data;

	mutex_lock(&sbi->s_lock);

	if (le32_to_cpu(es->s_free_blocks_count) < (u32)total) {
		*err = -ENOSPC;
		mutex_unlock(&sbi->s_lock);
		return 0;
	}

	search_start = goal - first_data;

	/* --- Allocate indirect blocks one at a time --- */
	for (allocated = 0; allocated < indirect_blks; allocated++) {
		struct buffer_head *bmap_bh;
		bool found = false;
		int pass;

		for (pass = 0; pass < 2 && !found; pass++) {
			unsigned long cur = (pass == 0) ? search_start : 0;
			unsigned long end = (pass == 0) ? nr_bits : search_start;

			while (cur < end) {
				unsigned long bmap_idx = cur / bits_per_bmap;
				unsigned long bit_from = cur % bits_per_bmap;
				unsigned long bit_end  = min(bits_per_bmap,
						end - bmap_idx * bits_per_bmap);
				unsigned long bit;

				bmap_bh = sb_bread(sb, OBSIDIANFS_BLOCK_BITMAP_BLOCK + bmap_idx);
				if (!bmap_bh) {
					*err = -EIO;
					goto rollback;
				}

				bit = find_next_zero_bit_le(bmap_bh->b_data, bit_end, bit_from);
				if (bit < bit_end) {
					__set_bit_le(bit, bmap_bh->b_data);
					le32_add_cpu(&es->s_free_blocks_count, -1);
					new_blocks[allocated] = first_data + bmap_idx * bits_per_bmap + bit;
					mark_buffer_dirty(bmap_bh);
					brelse(bmap_bh);
					search_start = bmap_idx * bits_per_bmap + bit + 1;
					found = true;
					break;
				}
				brelse(bmap_bh);
				cur = (bmap_idx + 1) * bits_per_bmap;
			}
		}

		if (!found) {
			*err = -ENOSPC;
			goto rollback;
		}
	}

	/* --- Allocate 'blks' contiguous data blocks — two-pass search around goal --- */
	{
		bool found = false;
		int pass;

		for (pass = 0; pass < 2 && !found; pass++) {
			unsigned long cur = (pass == 0) ? search_start : 0;
			unsigned long end = (pass == 0) ? nr_bits : search_start;

			while (cur < end && !found) {
				unsigned long bmap_idx = cur / bits_per_bmap;
				unsigned long bit_from = cur % bits_per_bmap;
				unsigned long bit_end  = min(bits_per_bmap,
						end - bmap_idx * bits_per_bmap);
				unsigned long b, run, k;
				struct buffer_head *bmap_bh;

				bmap_bh = sb_bread(sb, OBSIDIANFS_BLOCK_BITMAP_BLOCK + bmap_idx);
				if (!bmap_bh) {
					*err = -EIO;
					goto rollback;
				}

				b = bit_from;
				while (b < bit_end) {
					b = find_next_zero_bit_le(bmap_bh->b_data, bit_end, b);
					if (b >= bit_end)
						break;

					for (run = 0;
					     b + run < bit_end &&
					     !test_bit_le(b + run, bmap_bh->b_data) &&
					     run < (unsigned long)blks;
					     run++)
						;

					if (run >= (unsigned long)blks) {
						for (k = 0; k < (unsigned long)blks; k++)
							__set_bit_le(b + k, bmap_bh->b_data);
						le32_add_cpu(&es->s_free_blocks_count, -(u32)blks);
						new_blocks[allocated] = first_data +
							bmap_idx * bits_per_bmap + b;
						mark_buffer_dirty(bmap_bh);
						brelse(bmap_bh);
						ret = blks;
						found = true;
						break;
					}
					b += run + 1;
				}

				if (!found)
					brelse(bmap_bh);
				cur = (bmap_idx + 1) * bits_per_bmap;
			}
		}

		if (!found) {
			*err = -ENOSPC;
			goto rollback;
		}
	}
	goto out_unlock;

rollback:
	for (j = 0; j < allocated; j++) {
		unsigned long rel      = new_blocks[j] - first_data;
		unsigned long bmap_idx = rel / bits_per_bmap;
		unsigned long bit_pos  = rel % bits_per_bmap;
		struct buffer_head *bmap_bh = sb_bread(sb, OBSIDIANFS_BLOCK_BITMAP_BLOCK + bmap_idx);

		if (bmap_bh) {
			__clear_bit_le(bit_pos, bmap_bh->b_data);
			le32_add_cpu(&es->s_free_blocks_count, 1);
			mark_buffer_dirty(bmap_bh);
			brelse(bmap_bh);
		}
	}
out_unlock:
	mutex_unlock(&sbi->s_lock);
	if (ret > 0)
		mark_buffer_dirty(sbi->s_sbh);
	return ret;
}

/*
 * Free 'count' blocks starting at 'block'.
 * Updates the block bitmap (block OBSIDIANFS_BLOCK_BITMAP_BLOCK) and
 * s_free_blocks_count in the on-disk superblock.
 */
void obsidianfs_free_blocks(struct inode *inode, obsidianfs_fsblk_t block,
			     unsigned long count)
{
	struct super_block *sb = inode->i_sb;
	struct obsidianfs_sb_info *sbi = OBSIDIANFS_SB(sb);
	struct obsidianfs_super_block *es = sbi->s_es;
	unsigned long first_data    = le32_to_cpu(es->s_first_data_block);
	unsigned long blocks_count  = le32_to_cpu(es->s_blocks_count);
	unsigned long bits_per_bmap = sb->s_blocksize * 8UL;
	unsigned long freed = 0;
	unsigned long i     = 0;

	if (count == 0)
		return;

	if (block < first_data || block + count > blocks_count ||
	    block + count < block) {
		pr_err("[OBSIDIANFS ERROR] %s: invalid block range [%lu, %lu) "
		       "(first=%lu, total=%lu)\n",
		       __func__, block, block + count, first_data, blocks_count);
		return;
	}

	mutex_lock(&sbi->s_lock);

	while (i < count) {
		unsigned long rel      = block + i - first_data;
		unsigned long bmap_idx = rel / bits_per_bmap;
		unsigned long bit_from = rel % bits_per_bmap;
		unsigned long n        = min(bits_per_bmap - bit_from, count - i);
		struct buffer_head *bmap_bh;
		unsigned long k;

		bmap_bh = sb_bread(sb, OBSIDIANFS_BLOCK_BITMAP_BLOCK + bmap_idx);
		if (!bmap_bh) {
			pr_err("[OBSIDIANFS ERROR] %s: cannot read block bitmap block %lu\n",
			       __func__, bmap_idx);
			break;
		}

		for (k = 0; k < n; k++) {
			unsigned long bit = bit_from + k;

			if (test_bit_le(bit, bmap_bh->b_data)) {
				__clear_bit_le(bit, bmap_bh->b_data);
				freed++;
			} else {
				pr_err("[OBSIDIANFS ERROR] %s: block %lu already free\n",
				       __func__, block + i + k);
			}
		}

		mark_buffer_dirty(bmap_bh);
		if (sb->s_flags & SB_SYNCHRONOUS)
			sync_dirty_buffer(bmap_bh);
		brelse(bmap_bh);
		i += n;
	}

	if (freed)
		le32_add_cpu(&es->s_free_blocks_count, (u32)freed);

	mutex_unlock(&sbi->s_lock);

	if (freed) {
		mark_buffer_dirty(sbi->s_sbh);
		mark_inode_dirty(inode);
	}
}

/*
 * Splice an already-allocated branch into the inode block tree.
 */
static void obsidianfs_splice_branch(struct inode *inode, long block, Indirect *where, int num, int blks)
{
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);

	*where->p = where->key;

	if (oi->i_block_alloc_info) {
		oi->i_block_alloc_info->last_alloc_logical_block  =
			block + blks - 1;
		oi->i_block_alloc_info->last_alloc_physical_block =
			le32_to_cpu(where[num].key) + blks - 1;
	}
	mark_inode_dirty(inode);
}

/* ------------------------------------------------------------------ */
/* Core block mapping                                                   */
/* ------------------------------------------------------------------ */

static int obsidianfs_get_blocks(struct inode *inode, sector_t iblock, unsigned long maxblocks, u32 *bno, bool *new, bool *boundary, int create)
{
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	int offsets[4];
	Indirect chain[4];
	Indirect *partial;
	unsigned long goal;
	int indirect_blks;
	int blocks_to_boundary = 0;
	int depth;
	int count = 0;
	unsigned long first_block = 0;
	int err = 0;

	if (WARN_ON_ONCE(maxblocks == 0))
		return -EINVAL;

	depth = obsidian_block_to_path(inode, iblock, offsets, &blocks_to_boundary);
	if (depth == 0)
		return -EIO;

	partial = obsidianfs_get_branch(inode, depth, offsets, chain, &err);

	if (!partial) {
		first_block = le32_to_cpu(chain[depth - 1].key);
		count++;
		while (count < maxblocks && count <= (unsigned long)blocks_to_boundary) {
			obsidianfs_fsblk_t blk;

			if (!verify_chain(chain, chain + depth - 1)) {
				err     = -EAGAIN;
				count   = 0;
				partial = chain + depth - 1;
				break;
			}
			blk = le32_to_cpu(*(chain[depth - 1].p + count));
			if (blk == first_block + count)
				count++;
			else
				break;
		}
		if (err != -EAGAIN)
			goto got_it;
	}

	if (!create || err == -EIO)
		goto cleanup;

	mutex_lock(&oi->i_lock);

	if (err == -EAGAIN || !verify_chain(chain, partial)) {
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
		partial = obsidianfs_get_branch(inode, depth, offsets, chain, &err);
		if (!partial) {
			count++;
			mutex_unlock(&oi->i_lock);
			goto got_it;
		}
		if (err) {
			mutex_unlock(&oi->i_lock);
			goto cleanup;
		}
	}

	if (S_ISREG(inode->i_mode) && !oi->i_block_alloc_info)
		obsidianfs_init_block_alloc_info(inode);

	goal = obsidianfs_find_goal(inode, iblock, partial);

	indirect_blks = (chain + depth) - partial - 1;
	count = obsidianfs_blks_to_allocate(partial, indirect_blks,
					    maxblocks, blocks_to_boundary);

	err = obsidianfs_alloc_branch(inode, indirect_blks, &count, goal,
				      offsets + (partial - chain), partial);
	if (err) {
		mutex_unlock(&oi->i_lock);
		goto cleanup;
	}

	*new = true;
	obsidianfs_splice_branch(inode, iblock, partial, indirect_blks, count);
	mutex_unlock(&oi->i_lock);

got_it:
	if (count > blocks_to_boundary)
		*boundary = true;
	err = count;
	partial = chain + depth - 1;
cleanup:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	if (err > 0)
		*bno = le32_to_cpu(chain[depth - 1].key);
	return err;
}

int obsidianfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create)
{
	unsigned max_blocks = bh_result->b_size >> inode->i_blkbits;
	bool new      = false;
	bool boundary = false;
	u32 bno;
	int ret;

	ret = obsidianfs_get_blocks(inode, iblock, max_blocks, &bno,
				    &new, &boundary, create);
	if (ret <= 0)
		return ret;

	map_bh(bh_result, inode->i_sb, bno);
	bh_result->b_size = (ret << inode->i_blkbits);

	if (new)
		set_buffer_new(bh_result);
	if (boundary)
		set_buffer_boundary(bh_result);

	return 0;
}
