// GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/dax.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>

#include "inode.h"
#include "pageops.h"

int obsidian_read_folio (struct file *file, struct folio *folio);
int obsidian_write_begin (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
int obsidian_write_end (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct folio *folio, void *fsdata);
static int obsidianfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);
static int ext2_get_blocks(struct inode *inode, sector_t iblock, unsigned long maxblocks, u32 *bno, bool *new, bool *boundary, int create);

int obsidian_read_folio (struct file *file, struct folio *folio) {
    //pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    return mpage_read_folio(folio, obsidianfs_get_block);
}

int obsidian_write_begin (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata) {
    //pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    struct folio *folio;
    
    // The FGP_WRITEBEGIN flag is used to indicate that the write operation is starting, and the page should be locked and prepared for writing.
    // The function __filemap_get_folio will return a folio that is ready for writing, which may involve allocating a new folio or locking an existing one. 
    // The mapping_gfp_mask function is used to determine the appropriate GFP flags for memory allocation based on the address space's mapping.
    folio = __filemap_get_folio(mapping, pos / PAGE_SIZE, FGP_WRITEBEGIN, mapping_gfp_mask(mapping));

    if (IS_ERR(folio)) {
        pr_err("[ERROR OBSIDIANFS] error while calling %s\n", __func__);
        return PTR_ERR(folio);
    }

    *foliop = folio;

    /*
    If the folio is not up-to-date and the length of the write don't cover the entire folio
    We need to zero the pqrt of the folio thqt is not going to be written.
    In order to avoid any leaking of data from the kernel to the user space.
    */
    if (!folio_test_uptodate(folio) && (len != folio_size(folio))) {
		size_t from = offset_in_folio(folio, pos);
		folio_zero_segments(folio, 0, from, from + len, folio_size(folio));
	}
    return 0;
}

/*
pos : The position in the file where the write operation is taking place.
len : The length of the data being written. type: loff_t -> is a signed long long integer type used for file sizes.
copied : The amount of data that has been copied to the page cache so far.
folio : The folio that is being written to.
*/
int obsidian_write_end (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct folio *folio, void *fsdata) {
    //pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
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


/*
Function from the ext2 file system
*/
static int obsidianfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create) {
	unsigned max_blocks = bh_result->b_size >> inode->i_blkbits;
	bool new = false;
	bool boundary = false;
	u32 bno;
	int ret;

	ret = obsidianfs_get_blocks(inode, iblock, max_blocks, &bno, &new, &boundary, create);

	if (ret <= 0) {
		return ret;
	}
	
	map_bh(bh_result, inode->i_sb, bno); //Allow to leak the buffer to a physical block number.
	bh_result->b_size = (ret << inode->i_blkbits);

	if (new) {
		set_buffer_new(bh_result); // Set the flag new, so the system have to zeroed the block in order to avoid any data leak
	}
	if (boundary) {
		set_buffer_boundary(bh_result); // Set the flag to indicate the end of a contiguous data area. 
	}

	return 0;
}

/*
Function from the ext2 file system
*/
static int ext2_get_blocks(struct inode *inode, sector_t iblock, unsigned long maxblocks, u32 *bno, bool *new, bool *boundary, int create) {
	int err;
	int offsets[4];
	Indirect chain[4];
	Indirect *partial;
	unsigned long goal;
	int indirect_blks;
	int blocks_to_boundary = 0;
	int depth;
	struct ext2_inode_info *oi = EXT2_I(inode);
	int count = 0;
	unsigned long first_block = 0;

	if (WARN_ON_ONCE(maxblocks == 0))
		return -EINVAL;

	depth = ext2_block_to_path(inode,iblock,offsets,&blocks_to_boundary); // A ANALYSER 

	if (depth == 0)
		return -EIO;

	partial = ext2_get_branch(inode, depth, offsets, chain, &err); // A ANALYSER 
	/* Simplest case - block found, no allocation needed */
	if (!partial) {
		first_block = le32_to_cpu(chain[depth - 1].key);
		count++;
		/*map more blocks*/
		while (count < maxblocks && count <= blocks_to_boundary) {
			ext2_fsblk_t blk;

			if (!verify_chain(chain, chain + depth - 1)) {
				/*
				 * Indirect block might be removed by
				 * truncate while we were reading it.
				 * Handling of that case: forget what we've
				 * got now, go to reread.
				 */
				err = -EAGAIN;
				count = 0;
				partial = chain + depth - 1;
				break;
			}
			blk = le32_to_cpu(*(chain[depth-1].p + count));
			if (blk == first_block + count)
				count++;
			else
				break;
		}
		if (err != -EAGAIN)
			goto got_it;
	}

	/* Next simple case - plain lookup or failed read of indirect block */
	if (!create || err == -EIO)
		goto cleanup;

	mutex_lock(&oi->i_lock);
	/*
	 * If the indirect block is missing while we are reading
	 * the chain(ext2_get_branch() returns -EAGAIN err), or
	 * if the chain has been changed after we grab the semaphore,
	 * (either because another process truncated this branch, or
	 * another get_block allocated this branch) re-grab the chain to see if
	 * the request block has been allocated or not.
	 *
	 * Since we already block the truncate/other get_block
	 * at this point, we will have the current copy of the chain when we
	 * splice the branch into the tree.
	 */
	if (err == -EAGAIN || !verify_chain(chain, partial)) {
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
		partial = ext2_get_branch(inode, depth, offsets, chain, &err);
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

	/*
	 * Okay, we need to do block allocation.  Lazily initialize the block
	 * allocation info here if necessary
	*/
	if (S_ISREG(inode->i_mode) && (!ei->i_block_alloc_info)) // A MODIFIER => VOIR A QUOI CA SERT
		ext2_init_block_alloc_info(inode);

	goal = ext2_find_goal(inode, iblock, partial);

	/* the number of blocks need to allocate for [d,t]indirect blocks */
	indirect_blks = (chain + depth) - partial - 1;
	/*
	 * Next look up the indirect map to count the total number of
	 * direct blocks to allocate for this branch.
	 */
	count = ext2_blks_to_allocate(partial, indirect_blks,
					maxblocks, blocks_to_boundary);
	/*
	 * XXX ???? Block out ext2_truncate while we alter the tree
	 */
	err = ext2_alloc_branch(inode, indirect_blks, &count, goal,
				offsets + (partial - chain), partial);

	if (err) {
		mutex_unlock(&oi->i_lock);
		goto cleanup;
	}

	if (IS_DAX(inode)) {
		/*
		 * We must unmap blocks before zeroing so that writeback cannot
		 * overwrite zeros with stale data from block device page cache.
		 */
		clean_bdev_aliases(inode->i_sb->s_bdev,
				   le32_to_cpu(chain[depth-1].key),
				   count);
		/*
		 * block must be initialised before we put it in the tree
		 * so that it's not found by another thread before it's
		 * initialised
		 */
		err = sb_issue_zeroout(inode->i_sb,
				le32_to_cpu(chain[depth-1].key), count,
				GFP_KERNEL);
		if (err) {
			mutex_unlock(&oi->i_lock);
			goto cleanup;
		}
	}
	*new = true;

	ext2_splice_branch(inode, iblock, partial, indirect_blks, count);
	mutex_unlock(&oi->i_lock);
got_it:
	if (count > blocks_to_boundary)
		*boundary = true;
	err = count;
	/* Clean up and exit */
	partial = chain + depth - 1;	/* the whole chain */
cleanup:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	if (err > 0)
		*bno = le32_to_cpu(chain[depth-1].key);
	return err;
}