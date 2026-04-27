#include <linux/fs.h>
#include <linux/mm.h>
#include "ramobsidianfs.h"

const struct file_operations obsidianfs_file_ops = {
    .read_iter   = generic_file_read_iter,
    .write_iter  = generic_file_write_iter,
    .mmap        = generic_file_mmap,
    .fsync       = noop_fsync,
    .llseek      = generic_file_llseek,
    .splice_read = filemap_splice_read,
};

/*
#include <linux/fs.h>
#include <linux/mm.h>
#include "ramobsidianfs.h"

static int obsidianfs_read_folio(struct file *file, struct folio *folio)
{
    folio_zero_range(folio, 0, folio_size(folio));
    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;
}

const struct address_space_operations obsidianfs_aops = {
    .read_folio  = obsidianfs_read_folio,
    .write_begin = simple_write_begin,
    .write_end   = simple_write_end_nolocking,
};

const struct file_operations obsidianfs_file_ops = {
    .read_iter   = generic_file_read_iter,
    .write_iter  = generic_file_write_iter,
    .mmap        = generic_file_mmap,
    .fsync       = noop_fsync,
    .llseek      = generic_file_llseek,
    .splice_read = filemap_splice_read,
};
*/
