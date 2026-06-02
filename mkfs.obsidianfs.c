//mkfs obsidian is a tool in order to create the obsidianfs partition.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <endian.h>
#include "addressbitmap.h"
#include <linux/types.h>
#include "inode.h"

#define OBSIDIANFS_INODE_SIZE         128u

struct __attribute__((packed)) obsidianfs_super_block {
	uint32_t s_magic;				// 4
	uint32_t s_blocks_count;		// 4
	uint32_t s_inodes_count;		// 4
	uint32_t s_free_blocks_count;   // 4
	uint32_t s_free_inodes_count;   // 4
	uint32_t s_first_data_block;	// 4
	uint32_t s_log_block_size;		// 4
	uint32_t s_inode_size;			// 4
	uint32_t s_mtime;				// 4
	uint32_t s_wtime;				// 4
	uint8_t  s_uuid[16];			// 16 
	char     s_volume_name[16];		// 16
	uint8_t  s_padding[4024];		// 4024 => In order to use all the block size
									// SUM = 4096 bytes
};


static uint64_t get_device_size(int fd)
{
	struct stat st;
	uint64_t size = 0;

	if (fstat(fd, &st) < 0) { // Check if the file descriptor is correct. store the attribute of the file in the struct stat st.
		perror("fstat");
		return 0;
	}
	if (S_ISBLK(st.st_mode)) { // Check if the file is in block mode 
		if (ioctl(fd, BLKGETSIZE64, &size) < 0) { // Check if the size is strictly positive and set the return value to the size of the device. 
			perror("ioctl(BLKGETSIZE64)");
			return 0;
		}
	} else if (S_ISREG(st.st_mode)) { // Check if the file is a regular file
		size = (uint64_t)st.st_size; // Set the return value to the size of the file. 
	} else {
		fprintf(stderr, "Error: %s is neither a block device nor a regular file\n",
			"device");
		return 0;
	}
	return size;
}

/*
fd: file descriptor
blk_no: block number
blk_size: block size
count: quantity of block to write
data: pointer to the data to write

return 0 if everything went well, -1 else. 
*/
static int write_blocks(int fd, uint32_t blk_no, uint32_t blk_size, uint32_t count, const void *data)
{
	off_t    off  = (off_t)blk_no * blk_size; // Offset find by multiplicating the block number by the block size 
	size_t   len  = (size_t)count * blk_size; // Size of the data to write. 
	ssize_t  n    = pwrite(fd, data, len, off); // writing data in the file pointed by the fd. 

	if (n < 0) { // Case if the write was failed
		fprintf(stderr, "pwrite at block %u: %s\n", blk_no, strerror(errno));
		return -1;
	}
	if ((size_t)n != len) { // Case if the size of the data written is not equal to the size of the data who should be written. 
		fprintf(stderr, "Short write at block %u: wrote %zd of %zu\n", blk_no, n, len);
		return -1;
	}
	return 0;
}

static void read_uuid(uint8_t uuid[16])
{
	int rfd = open("/dev/urandom", O_RDONLY);
	if (rfd < 0 || read(rfd, uuid, 16) != 16) {
		memset(uuid, 0, 16);
	}
	if (rfd >= 0) {
		close(rfd);
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-L volume_label] <device>\n"
		"  -L  Volume label, max 15 chars (default: empty)\n"
		"\nExample:\n"
		"  dd if=/dev/zero of=test.img bs=8192 count=6400000 (produce a 50GB file)\n"
		"  %s -L myfs test.img\n",
		prog, prog);
}

int main(int argc, char *argv[])
{
	uint32_t    block_size   = 4096;
	uint32_t    inodes_count = 8u * block_size * OBSIDIANFS_INODE_BITMAP_SIZE; // 8u because there is 8 bits for each byte, 
	const char *volume_label = "";
	const char *device       = NULL;
	int opt, rc = 0;

	while ((opt = getopt(argc, argv, "L:")) != -1) { // Function to recover the command parameter. 
		switch (opt) {
		case 'L':
			volume_label = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Error: no device specified\n\n");
		usage(argv[0]);
		return 1;
	}
	device = argv[optind];

	if (strlen(volume_label) > 15) {
		fprintf(stderr, "Error: volume label must be at most 15 characters\n");
		return 1;
	}

	int fd = open(device, O_RDWR); // Opening the device. 
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return 1;
	}

	uint64_t device_size = get_device_size(fd); 
	if (device_size == 0) {
		close(fd);
		return 1;
	}

	uint32_t blocks_count     = (uint32_t)(device_size / block_size); 
	uint32_t inodes_per_block = block_size / OBSIDIANFS_INODE_SIZE; // 64
	uint32_t inode_table_blks = (inodes_count + inodes_per_block - 1) / inodes_per_block;
	uint32_t first_data_block = OBSIDIANFS_INODE_TABLE_BLOCK + inode_table_blks;

	uint32_t log_block_size = 0;
	{
		uint32_t bs = block_size;
		while (bs > 1024) { 
			log_block_size++; 
			bs >>= 1; 
		}
	}

	/* ---- Final validation ---- */
	if (blocks_count < first_data_block + 1) {
		fprintf(stderr,
			"Error: device too small. Need at least %u blocks of %u bytes "
			"(%llu bytes total), but device has %u blocks.\n",
			first_data_block + 1, block_size,
			(unsigned long long)(first_data_block + 1) * block_size,
			blocks_count);
		close(fd);
		return 1;
	}

	uint32_t free_blocks = blocks_count - first_data_block;
	uint32_t free_inodes = inodes_count - 1; /* root inode is used */
	uint32_t now         = (uint32_t)time(NULL);
	uint8_t  uuid[16];
	read_uuid(uuid);

	/* ---- Block 0: Superblock ---- */
	{
		struct obsidianfs_super_block sb;
		uint8_t *wbuf;

		memset(&sb, 0, sizeof(sb));
		sb.s_magic             = htole32(OBSIDIAN_MAGIC); // htole: This functions convert the byte encoding of integer values from the byte order that the current CPU (the "host") uses
		sb.s_blocks_count      = htole32(blocks_count);
		sb.s_inodes_count      = htole32(inodes_count);
		sb.s_free_blocks_count = htole32(free_blocks);
		sb.s_free_inodes_count = htole32(free_inodes);
		sb.s_first_data_block  = htole32(first_data_block);
		sb.s_log_block_size    = htole32(log_block_size);
		sb.s_inode_size        = htole32(OBSIDIANFS_INODE_SIZE);
		sb.s_mtime             = 0;
		sb.s_wtime             = htole32(now);
		memcpy(sb.s_uuid, uuid, 16);
		strncpy(sb.s_volume_name, volume_label, sizeof(sb.s_volume_name) - 1);

		wbuf = calloc(1, block_size); // initialise an array of one, of size block_size
		if (!wbuf) { 
			perror("calloc"); 
			rc = 1; 
			goto out_close; 
		}
		memcpy(wbuf, &sb, sizeof(sb)); // Copy the structure super block in the array we just allocate. 

		rc = write_blocks(fd, OBSIDIANFS_SB_BLOCK, block_size, 1, wbuf);
		free(wbuf); // Free the array
		if (rc) {
			goto out_close;
		}
		printf("[OBSIDIANFS] Superblock  OK (block 0)\n");
	}

	/* ---- Block 1: Inode bitmap ---- */
	{
		uint8_t *ibmap = calloc(OBSIDIANFS_INODE_BITMAP_SIZE, block_size);
		if (!ibmap) { 
			perror("calloc"); 
			rc = 1; 
			goto out_close; 
		}
		ibmap[0] |= 0x01;  /* bit (ino-1): inode 1 = root is used */

		rc = write_blocks(fd, OBSIDIANFS_INODE_BITMAP_BLOCK, block_size, OBSIDIANFS_INODE_BITMAP_SIZE, ibmap);
		free(ibmap);
		if (rc) goto out_close;
		printf("[OBSIDIANFS] Inode bitmap (block %u..%u)\n", OBSIDIANFS_INODE_BITMAP_BLOCK, OBSIDIANFS_INODE_BITMAP_BLOCK + OBSIDIANFS_INODE_BITMAP_SIZE - 1);
	}

	/* ---- Block 2: Block bitmap ---- */
	{
		uint8_t *bbmap = calloc(OBSIDIANFS_BLOCK_BITMAP_SIZE, block_size);
		if (!bbmap) { 
			perror("calloc"); 
			rc = 1; 
			goto out_close; 
		}

		for (uint32_t i = 0; i < first_data_block; i++) { // Set the first block until first data block as used. 
			bbmap[i / 8] |= (1u << (i % 8));
		}


		rc = write_blocks(fd, OBSIDIANFS_BLOCK_BITMAP_BLOCK, block_size, OBSIDIANFS_BLOCK_BITMAP_SIZE, bbmap);
		free(bbmap);
		if (rc) {
			goto out_close;
		}
		printf("[OBSIDIANFS] Block bitmap (block %u..%u)\n", OBSIDIANFS_BLOCK_BITMAP_BLOCK, OBSIDIANFS_BLOCK_BITMAP_BLOCK + OBSIDIANFS_BLOCK_BITMAP_SIZE - 1);
	}

	/* ---- Blocks 3..first_data_block-1: Inode table ---- */
	{
		size_t  tblsz     = (size_t)inode_table_blks * block_size; //size in bytes of the inode table
		uint8_t *itable   = calloc(1, tblsz); //allocation of the table
		
		if (!itable) { 
			perror("calloc"); 
			rc = 1; 
			goto out_close; 
		}

		// Root inode
		// It's the inode corresponding to / in the file system
		// Set as a directory in order to carry every other element of the system.

		struct obsidianfs_inode root;
		memset(&root, 0, sizeof(root));
		root.i_mode        = htole16(0x41EDu); /* S_IFDIR | 0755 */
		root.i_links_count = htole16(2u);      /* '.' and '..' */
		root.i_uid         = 0;
		root.i_gid         = 0;
		root.i_size        = 0;
		root.i_blocks      = 0;
		root.i_atime       = htole32(now); //Access time
		root.i_ctime       = htole32(now); //Creation time
		root.i_mtime       = htole32(now); //modification time
		root.i_dtime       = 0;
		memcpy(itable, &root, sizeof(root));

		rc = write_blocks(fd, OBSIDIANFS_INODE_TABLE_BLOCK, block_size, inode_table_blks, itable);
		free(itable);
		if (rc) goto out_close;
		printf("[OBSIDIANFS] Inode table OK (blocks %u..%u)\n", OBSIDIANFS_BLOCK_BITMAP_BLOCK + OBSIDIANFS_BLOCK_BITMAP_SIZE, first_data_block - 1);
	}

	/* ---- Data area: zero-fill ---- */
	{
		uint32_t chunk_blks = (1024u * 1024u) / block_size;
		if (chunk_blks == 0)
			chunk_blks = 1;

		size_t   chunk_sz = (size_t)chunk_blks * block_size;
		uint8_t *chunk    = calloc(1, chunk_sz);
		if (!chunk) { 
			perror("calloc"); 
			rc = 1; 
			goto out_close; 
		}

		uint32_t blk       = first_data_block;
		uint32_t remaining = free_blocks;

		while (remaining > 0) {
			uint32_t n = (remaining < chunk_blks) ? remaining : chunk_blks;
			if (write_blocks(fd, blk, block_size, n, chunk) < 0) {
				free(chunk);
				rc = 1;
				goto out_close;
			}
			blk       += n;
			remaining -= n;
		}
		free(chunk);
		printf("[OBSIDIANFS] Data area zeroed (blocks %u..%u)\n",
		       first_data_block, blocks_count - 1);
	}

	if (fsync(fd) < 0) {
		perror("fsync");
		rc = 1;
		goto out_close;
	}

	printf("\n[OBSIDIANFS] Ready to use on %s\n", device);

out_close:
	close(fd);
	return rc;
}