#ifndef ADDRESSOBSIDIANFS_H
#define ADDRESSOBSIDIANFS_H

#define OBSIDIANFS_SB_BLOCK           0u
#define OBSIDIANFS_INODE_BITMAP_BLOCK 1u // 
#define OBSIDIANFS_INODE_BITMAP_SIZE  128u // 128 blocks for imap CHANGE THIS VALUE FOR MORE OR LESS INODE          //
#define OBSIDIANFS_BLOCK_BITMAP_BLOCK (OBSIDIANFS_INODE_BITMAP_BLOCK + OBSIDIANFS_INODE_BITMAP_SIZE)                //
#define OBSIDIANFS_BLOCK_BITMAP_SIZE  9314u // 9314 blocks for bmap CHANGE THIS VALUE FOR MORE OR LESS BLOCKS       //
#define OBSIDIANFS_INODE_TABLE_BLOCK  (OBSIDIANFS_BLOCK_BITMAP_BLOCK + OBSIDIANFS_BLOCK_BITMAP_SIZE)                // All these values are used for prototyping, the mechanism of blocks allocation can and will improved in the future
#define OBSIDIANFS_ROOT_INO           1u

#endif