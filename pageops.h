#ifndef PAGEOPSOBSIDIANFS_H
#define PAGEOPSOBSIDIANFS_H

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/log2.h>

#define NDIR_BLOCKS 			12
#define FGP_WRITEBEGIN		    (FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)
#define BLOCK_SIZE(s)		    ((s)->s_blocksize)
#define	ADDR_PER_BLOCK(s)		(BLOCK_SIZE(s) / sizeof (__u32))
#define BLOCK_SIZE_BITS(s)		((s)->s_blocksize_bits)
#define	ADDR_PER_BLOCK_BITS(s)	(ilog2(ADDR_PER_BLOCK(s)))

extern int obsidian_read_folio (struct file *file, struct folio *folio);
extern int obsidian_write_begin (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
extern int obsidian_write_end (const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct folio *folio, void *fsdata);

typedef struct {
	__le32	*p;
	__le32	key;
	struct buffer_head *bh;
} Indirect;

struct rb_node {
	unsigned long  __rb_parent_color;
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct ext2_reserve_window {
	ext2_fsblk_t		_rsv_start;	
	ext2_fsblk_t		_rsv_end;	
};

struct ext2_reserve_window_node {
	struct rb_node	 	rsv_node;
	__u32			rsv_goal_size;
	__u32			rsv_alloc_hit;
	struct ext2_reserve_window	rsv_window;
};

struct ext2_block_alloc_info {

	struct ext2_reserve_window_node	rsv_window_node;
	__u32			last_alloc_logical_block;
	ext2_fsblk_t		last_alloc_physical_block;
};

struct obsidian_chain {
	unsigned int dir;
	unsigned int size;
	unsigned char flags;
};

#endif