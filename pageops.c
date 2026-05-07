// GPL-2.0-only

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include "pageops.h"

int obsidian_read_folio (struct file *file, struct folio *folio) {
    //pr_info("[INFO OBSIDIANFS] call %s\n", __func__);
    folio_zero_range(folio, 0, folio_size(folio)); // Zeroed the folio, have the folio pointer, the offset and the length of the range to zero for arguments.
	flush_dcache_folio(folio); // Flush the CPU cache for the folio, no-op operation on x86 architecture but better to write this line for safety.
	folio_mark_uptodate(folio); // Mark this folio as valid to read for the kernel.
	folio_unlock(folio); // Unlock the folio.
    return 0;
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
    struct inode *inode = folio->mapping->host; // get the inode associated with the folio
    loff_t last_pos = pos + copied;

    if (!folio_test_uptodate(folio)) {
        if (copied < len) {
            size_t from = offset_in_folio(folio, pos); 
            folio_zero_range(folio, from + copied, len - copied); // Zero the remaining part of the folio that is not written, to avoid leaking data from the kernel to the user space.
        }
        folio_mark_uptodate(folio);
    }

    if (last_pos > i_size_read(inode)) {
        i_size_write(inode, last_pos); // Update the file size if the write operation extends beyond the current file size.
    }

    folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_put(folio);

	return copied;
}

/*
return true if the folio was not already dirty and is now marked dirty
return false if the folio was already dirty
return false if the folio was successfully marked dirty but was dirtyed by another thread in the meantime
*/
bool obsidian_dirty_folio (struct address_space *mapping, struct folio *folio) {
    if (!folio_test_dirty(folio)) { // Check if the folio is already marked as dirty, if not dirty, execute the following code
        return !folio_test_set_dirty(folio); // set the folio as dirty and return true if the folio wasnt already dirty, but if it was dirty before setting it dirty, return false.
    }
	return false;
}