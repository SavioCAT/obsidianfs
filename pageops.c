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

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static int obsidianfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);
static int obsidianfs_get_blocks(struct inode *inode, sector_t iblock, unsigned long maxblocks, u32 *bno, bool *new, bool *boundary, int create);
static int obsidian_block_to_path(struct inode *inode, long i_block, int offsets[4], int *boundary);
static Indirect *obsidianfs_get_branch(struct inode *inode, int depth, int *offsets, Indirect chain[4], int *err);
static obsidianfs_fsblk_t obsidianfs_group_first_block_no(struct super_block *sb, unsigned long group_no);
static unsigned long obsidianfs_find_near(struct inode *inode, Indirect *ind);
static unsigned long obsidianfs_find_goal(struct inode *inode, long block, Indirect *partial);
static int obsidianfs_blks_to_allocate(Indirect *branch, int k, unsigned long blks, int blocks_to_boundary);
static int obsidianfs_alloc_branch(struct inode *inode, int indirect_blks, int *blks, unsigned long goal, int *offsets, Indirect *branch);
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
	struct folio *folio;

	folio = __filemap_get_folio(mapping, pos / PAGE_SIZE,
				    FGP_WRITEBEGIN, mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		pr_err("[ERROR OBSIDIANFS] %s: get_folio failed\n", __func__);
		return PTR_ERR(folio);
	}

	*foliop = folio;

	/*
	 * Zero the region outside the write to prevent kernel data leaks
	 * when the folio is not yet up-to-date and the write is partial.
	 */
	if (!folio_test_uptodate(folio) && len != folio_size(folio)) {
		size_t from = offset_in_folio(folio, pos);

		folio_zero_segments(folio, 0, from, from + len, folio_size(folio));
	}

	return 0;
}

int obsidian_write_end(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct folio *folio, void *fsdata)
{
	struct inode *inode = folio->mapping->host;
	struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
	loff_t last_pos = pos + copied;

	if (!folio_test_uptodate(folio)) {
		if (copied < len) {
			size_t from = offset_in_folio(folio, pos);

			folio_zero_range(folio, from + copied, len - copied);
		}
		folio_mark_uptodate(folio);
	}

	mutex_lock(&oi->i_lock);
	if (last_pos > i_size_read(inode))
		i_size_write(inode, last_pos);
	if (last_pos > oi->valid_size)
		oi->valid_size = last_pos;
	mutex_unlock(&oi->i_lock);

	folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_put(folio);

	return copied;
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
		pr_err("[ERROR OBSIDIANFS] %s: block %ld out of range\n",
		       __func__, i_block);
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

/*
 * TODO: allocate a chain of blocks from the on-disk bitmap.
 * Requires the block-group allocator (s_fs_info + bitmap blocks).
 */
static int obsidianfs_alloc_branch(struct inode *inode, int indirect_blks, int *blks, unsigned long goal, int *offsets, Indirect *branch)
{
	return -ENOSPC;
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

static int obsidianfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create)
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
