#ifndef PAGEOPSOBSIDIANFS_H
#define PAGEOPSOBSIDIANFS_H

#include <linux/fs.h>
#include <linux/pagemap.h>

#define FGP_WRITEBEGIN		(FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)

extern int obsidian_read_folio (struct file *file, struct folio *folio);
extern int obsidian_write_begin (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
extern int obsidian_write_end (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct folio *folio, void *fsdata);

struct obsidian_chain {
	unsigned int dir;
	unsigned int size;
	unsigned char flags;
};

#endif