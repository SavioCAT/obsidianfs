# ObsidianFS

ObsidianFS is a Linux kernel filesystem module written in C, developed as an academic project using the Virtual Filesystem (VFS) layer. It implements a custom filesystem inspired by ext2, aiming for a persistent ransomware resistant file system.

---

## Project structure

```
obsidianfs/
├── inode.h          — on-disk inode struct, in-memory inode_meta, OBSIDIANFS_INODE()
├── super.h          — on-disk superblock struct, in-memory sb_info, disk layout constants
├── pageops.h        — block addressing types: Indirect, obsidianfs_block_alloc_info
├── file.h           — exported symbols for file and directory operations
├── ioctlops.h       — ioctl command definitions (shared with userspace)
├── super.c          — superblock, module init/exit, inode slab, write_inode
├── inode.c          — inode lifecycle: iget, create, directory inode_operations
├── file.c           — file_operations, address_space_operations, mknod/mkdir
├── pageops.c        — page cache I/O, ext2-style block mapping
├── ioctlops.c       — ioctl handler
└── obsidiancommand.c — userspace CLI to invoke custom ioctls
```

---

## On-disk data structures

### `obsidianfs_super_block` (`super.h`)

The on-disk superblock is stored at block 0 and padded to 512 bytes. It holds the filesystem geometry: total block and inode counts, free counts, first data block, block size exponent, inode size, mount/write timestamps, a 128-bit UUID, and a volume name.

### `obsidianfs_inode` (`inode.h`)

The on-disk inode is 128 bytes, naturally aligned without implicit padding. Key design choices over the original ext2 layout:

- `i_size` is `__le64` — supports files up to 2^63 bytes (Huge)
- `i_uid` / `i_gid` are `__le32` — supports UIDs and GIDs above 65535 (Also huge)
- Use the POSIX standarts for the ACL 
- All block pointers and field counts use `OBSIDIAN_N_BLOCKS` (15)

---

## Core data structures — `inode.h`

### `obsidianfs_inode_meta`

Every inode managed by ObsidianFS is represented by an `obsidianfs_inode_meta` structure. This structure embeds the kernel's generic `struct inode` as its first element, which is the standard pattern required by the VFS to allow safe casting via `container_of`. The helper `OBSIDIANFS_INODE(inode)` recovers the full ObsidianFS-specific wrapper from any `struct inode *`.

Beyond the standard VFS inode, `obsidianfs_inode_meta` carries: a `struct mutex` (`i_lock`) protecting block allocation and `valid_size` updates; an `rwlock_t` (`readwritelock`) guarding the indirect block chain during concurrent reads; a `bool flagsProtected` controlling write and truncation permissions; an `i_data[OBSIDIAN_N_BLOCKS]` array mirroring the on-disk block pointers; and an `i_block_group` field indicating the block group to which the inode belongs.

---

## Module lifecycle — `super.c`

### Inode slab: `obsidianfs_alloc_inode` / `obsidianfs_free_inode`

ObsidianFS manages its inode memory through a dedicated `kmem_cache` created at module load time. `obsidianfs_alloc_inode` is the `super_operations.alloc_inode` callback: each time the VFS needs a new inode, it calls this function, which allocates an `obsidianfs_inode_meta` from the slab and returns a pointer to its embedded `struct inode`. The slab constructor `obsidianfs_i_init_once` initialises the mutex, the rwlock, and the VFS inode fields exactly once per slab object, which avoids redundant initialisation across reuse cycles.

`obsidianfs_free_inode` is the symmetric callback (`super_operations.free_inode`): it returns the `obsidianfs_inode_meta` wrapper back to the slab.

### Superblock initialisation: `obsidianfs_fill_super_persistent`

`obsidianfs_fill_super_persistent` is invoked by the kernel during a `mount` call on a block device. It allocates `obsidianfs_sb_info`, reads the on-disk superblock via `sb_bread`, switches to the block size stored on disk (re-reading the superblock if the size changed), wires up the VFS superblock fields, and then calls `obsidianfs_iget` to load the root inode from disk. The root dentry is attached via `d_make_root`.

### Inode writeback: `obsidianfs_write_inode`

`obsidianfs_write_inode` is the `super_operations.write_inode` callback, invoked by the writeback machinery whenever `mark_inode_dirty()` fires. It computes the block and byte offset of the inode in the inode table, reads that block via `sb_bread`, serialises all in-memory fields (mode, uid, gid, size, timestamps, link count, block pointers) into the on-disk `obsidianfs_inode` slot using `cpu_to_le*` and `from_kuid/kgid`, marks the buffer dirty, and forces a synchronous flush only when `wbc->sync_mode == WB_SYNC_ALL` (i.e. on an explicit `fsync`).

### Module registration: `obsidianfs_init` / `obsidianfs_exit`

`obsidianfs_init` creates the inode slab cache then registers the persistent filesystem type (`obsidianfs_persistent`). If registration fails, the slab is destroyed before returning. `obsidianfs_exit` reverses the operation in order: unregister first, then destroy the slab.

---

## Inode operations — `inode.c`

### Loading from disk: `obsidianfs_iget`

`obsidianfs_iget` reads an existing inode from the on-disk inode table. It first validates the inode number against `s_inodes_count`, then calls `iget_locked`: if the inode is already in the VFS cache it is returned immediately; otherwise a fresh locked inode is allocated. The function computes the inode's position in the table (`OBSIDIANFS_INODE_TABLE_BLOCK + (ino-1) / inodes_per_block`), reads the block, deserialises all fields into the VFS inode and `obsidianfs_inode_meta` (including a `memcpy` of the 15 block pointers into `i_data`), wires up the correct `inode_operations` and `file_operations` by file type, and calls `unlock_new_inode` before returning.

### In-memory creation: `obsidianfs_create_inode_memory`

`obsidianfs_create_inode_memory` creates a brand-new inode in RAM for file/directory/symlink creation operations. It calls `new_inode` to obtain a fresh VFS inode, initialises ObsidianFS-specific fields, and wires up operations by file type. The inode number is assigned via `get_next_ino()` — a temporary mechanism that will be replaced by a proper bitmap allocator that reserves a slot in the on-disk inode bitmap.

### File and directory creation: `obsidianfs_create`, `obsidianfs_mknod`

`obsidianfs_create` is the `inode_operations.create` callback, called by the VFS when a user creates a regular file. It delegates to `obsidianfs_mknod` with `S_IFREG` set. `obsidianfs_mknod` calls `obsidianfs_create_inode_memory`, associates the inode with the dentry via `d_instantiate`, and updates the parent directory's modification time.

### Directory creation: `obsidianfs_mkdir`

`obsidianfs_mkdir` is the `inode_operations.mkdir` callback. It delegates to `obsidianfs_mknod` with `S_IFDIR` set, then increments the parent directory's link count via `inc_nlink` to reflect the new `..` entry.

### Hard links: `obsidianfs_hardlink`

`obsidianfs_hardlink` implements `inode_operations.link`. It increments the inode's reference count with `igrab`, increments the link count with `inc_nlink`, and associates the new dentry with the inode using `d_instantiate`. Both the inode's change time and the parent directory's modification time are updated.

### Symbolic links: `obsidianfs_symlink`

`obsidianfs_symlink` allocates a new `S_IFLNK` inode and stores the target path in its page cache using `page_symlink`. If allocation fails, the inode reference is dropped with `iput`. The kernel's `page_symlink_inode_operations` handles readlink and follow-link transparently.

### Deletion: `obsidianfs_unlink`

`obsidianfs_unlink` checks `flagsProtected` first (returns `-EPERM` if set), then decrements the link count with `drop_nlink` and marks both the inode and parent directory dirty. When the link count reaches zero and the last file descriptor is closed, the VFS calls `evict_inode`.

### Attribute modification: `obsidian_setattr`

`obsidian_setattr` enforces the protection flag, runs `setattr_prepare`, handles size changes via `truncate_setsize`, and applies all other attributes through `setattr_copy`.

---

## File operations — `file.c`

### Write path: `obsidian_write_iter`

`obsidian_write_iter` checks the protection flag, acquires the inode lock to serialise concurrent writers, runs `generic_write_checks`, and delegates the actual data copy to `__generic_file_write_iter` which routes data through the page cache. `generic_write_sync` handles `O_SYNC` semantics after the write.

### Address space operations: `obsidianfs_page_ops`

`obsidianfs_page_ops` wires `read_folio` to `obsidian_read_folio`, `write_begin`/`write_end` to the custom implementations in `pageops.c`, `dirty_folio` to `block_dirty_folio`, and `invalidate_folio` to `block_invalidate_folio`.

---

## Page and block operations — `pageops.c`

### Write preparation: `obsidian_write_begin`

Retrieves or allocates the target folio with `FGP_WRITEBEGIN` flags. If the folio is not yet up-to-date and the write is partial, it zeroes the unwritten regions with `folio_zero_segments` to prevent stale kernel memory exposure.

### Write completion: `obsidian_write_end`

Updates `i_size` if the write extended the file, keeps `valid_size` in sync under the inode mutex, marks the folio up-to-date and dirty, then unlocks and releases it.

### Block path resolution: `obsidian_block_to_path`

Translates a logical block number into a path of indices through the inode's block tree: 12 direct blocks, then single, double, and triple indirect. Returns the depth (1–4).

### Indirect chain walk: `obsidianfs_get_branch`

Walks the indirect chain described by `offsets[]`, reading each level from disk via `sb_bread`, protected by `readwritelock`. Returns `NULL` if the block exists, or a pointer to the first missing entry if allocation is needed.

### Block mapping: `obsidianfs_get_block` / `obsidianfs_get_blocks`

`obsidianfs_get_block` is the callback passed to `mpage_read_folio`. `obsidianfs_get_blocks` orchestrates full block lookup and optional allocation: it calls `obsidian_block_to_path`, walks the chain, and if a block is missing and `create` is set, calls `obsidianfs_alloc_branch` then `obsidianfs_splice_branch`.

### Block allocation goal: `obsidianfs_find_goal` / `obsidianfs_find_near`

`obsidianfs_find_goal` prefers the next sequential physical block if the previous allocation was sequential. Otherwise `obsidianfs_find_near` scans existing block pointers backwards for a locality hint.

### Branch allocation: `obsidianfs_alloc_branch`

Currently returns `-ENOSPC` unconditionally. WIP...

### Branch splice: `obsidianfs_splice_branch`

Links a newly allocated branch into the inode's block tree by writing the first block number into the appropriate slot, then updates `i_block_alloc_info` for sequential allocation hints.

---

## ioctl interface — `ioctlops.c`

### File protection: `OBSIDIAN_IOC_PROTECT`

Sets `flagsProtected` on an inode. Once set, writes, truncations, and attribute changes return `-EPERM`. The flag is currently volatile (in-memory only) — it is not yet persisted to the `i_flags` field on disk.

### Flag query: `OBSIDIAN_IOC_GET_FLAGS`

Copies a single byte (1 = protected, 0 = not protected) to userspace via `copy_to_user`.

---

## Userspace tool — `obsidiancommand.c`

`obsidiancommand` opens a file by path and issues `OBSIDIAN_IOC_PROTECT`. The `ioctlops.h` header uses `#ifdef __KERNEL__` so that ioctl command definitions are shared between kernel and userspace without conflict.

---

## Build and usage

### Requirements

- Linux kernel headers for the running kernel (`kernel-devel` / `linux-headers`)
- GCC
- make

### Compilation

```bash
make          # builds the kernel module (.ko) and the userspace tool
make clean    # removes all build artefacts
```

### Mount

```bash
sudo make load    # insmod + mount at /tmp/obsidian
sudo make unload  # umount + rmmod
```

### Protect a file

```bash
echo "important data" > /tmp/obsidian/myfile
./commandBin /tmp/obsidian/myfile
echo "overwrite attempt" > /tmp/obsidian/myfile  # → Permission denied
```

---

## Current state and roadmap

ObsidianFS compiles cleanly on Linux **6.19**. The following components are implemented and operational:

- VFS registration and block-device mount lifecycle
- On-disk superblock and inode structures
- `obsidianfs_iget` — deserialises any inode from the on-disk inode table
- `obsidianfs_write_inode` — serialises inode metadata back to disk
- Regular files, directories, symbolic links, hard links, and special files
- Read and write through the page cache
- ext2-style indirect block addressing
- Custom ioctl-based file protection

The following work remains before the filesystem is fully persistent:

1. **Block allocator** — implement `obsidianfs_alloc_branch` to read the free block bitmap, find free blocks, mark them used, and return their numbers. `obsidianfs_group_first_block_no` also needs to return the correct group start.

2. **`mkfs.obsidianfs`** — userspace tool to initialise a blank device: write the superblock, inode bitmap, block bitmap, inode table, and root inode (ino 1). 

3. **On-disk directory entry format** — define `obsidianfs_dir_entry` (name, inode number, record length).

4. **Persistent directory lookup** — replace `simple_lookup` with a custom `obsidianfs_lookup` that reads directory entries from disk and calls `obsidianfs_iget` on found entries.

5. **Persistent directory read** — replace `simple_dir_operations` with an `obsidianfs_readdir` that iterates on-disk entries for `getdents`.


---

## License

GPL-2.0-only
