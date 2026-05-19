# ObsidianFS

ObsidianFS is a Linux kernel filesystem module written in C, developed as an academic project using the Virtual Filesystem(VFS) layer. It implements a custom filesystem based on ext2.

The project currently operates in RAM mode (no block device required), which makes it easy to mount, test, and study without requiring a formatted partition. The block allocation infrastructure is already scaffolded in preparation for a persistent, on-disk version.

---

## Project structure

```
obsidianfs/
├── inode.h          — core types: obsidianfs_inode_meta, OBSIDIANFS_INODE()
├── pageops.h        — block addressing types: Indirect, obsidianfs_block_alloc_info
├── file.h           — exported symbols for file and directory operations
├── ioctlops.h       — ioctl command definitions (shared with userspace)
├── super.c          — superblock, module init/exit, inode slab
├── inode.c          — inode lifecycle and directory inode_operations
├── file.c           — file_operations, address_space_operations, mknod/mkdir
├── pageops.c        — page cache I/O, ext2-style block mapping
├── ioctlops.c       — ioctl handler
└── obsidiancommand.c — userspace CLI to invoke custom ioctls
```

---

## Core data structure — `obsidianfs_inode_meta`

Every inode managed by ObsidianFS is represented by an `obsidianfs_inode_meta` structure, defined in `inode.h`. This structure embeds the kernel's generic `struct inode` as its first element, which is the standard pattern required by the VFS to allow safe casting via `container_of`. This design means no extra pointer indirection is needed: the kernel allocates and casts inodes through the custom slab allocator, and the helper `OBSIDIANFS_INODE(inode)` recovers the full ObsidianFS-specific wrapper from any `struct inode *`.

Beyond the standard VFS inode, `obsidianfs_inode_meta` carries: a `struct mutex` (`i_lock`) protecting block allocation and `valid_size` updates; an `rwlock_t` (`readwritelock`) guarding the indirect block chain during concurrent reads; a `bool flagsProtected` controlling write and truncation permissions; an `i_data[15]` array holding 12 direct block pointers plus one single-, one double-, and one triple-indirect pointer (following the ext2 layout); and an `i_block_group` field indicating the block group to which the inode belongs.

---

## Module lifecycle — `super.c`

### Inode slab: `obsidianfs_alloc_inode` / `obsidianfs_free_inode`

ObsidianFS manages its inode memory through a dedicated `kmem_cache` created at module load time. `obsidianfs_alloc_inode` is the `super_operations.alloc_inode` callback: each time the VFS needs a new inode, it calls this function, which allocates an `obsidianfs_inode_meta` from the slab and returns a pointer to its embedded `struct inode`. The slab constructor `obsidianfs_i_init_once` initialises the mutex, the rwlock, and the VFS inode fields exactly once per slab object, which avoids redundant initialisation across reuse cycles.

`obsidianfs_free_inode` is the symmetric callback (`super_operations.free_inode`): it returns the `obsidianfs_inode_meta` wrapper back to the slab. Together these two functions ensure that inode memory is managed efficiently and that per-inode locks are always in a valid state.

### Superblock initialisation: `obsidianfs_fill_super`

`obsidianfs_fill_super` is invoked by the kernel during a `mount` call. It sets the mandatory superblock fields (`s_magic`, `s_op`, `s_blocksize`, `s_maxbytes`), then allocates and instantiates a root directory inode by calling `obsidianfs_get_inode` with `S_IFDIR | 0755`. The root dentry is attached via `d_make_root`. In the future, this function will be the entry point for reading the on-disk superblock, validating the magic number, and populating `s_fs_info` with block group metadata.

### Module registration: `obsidianfs_init` / `obsidianfs_exit`

`obsidianfs_init` is the `module_init` entry point. It first creates the inode slab cache, then calls `register_filesystem` to make `obsidianfs` available as a mountable type. If slab creation fails the function returns early without registering, preventing a partially initialised module from being used. `obsidianfs_exit` reverses the operation in the correct order: it unregisters the filesystem type first, then destroys the slab cache.

---

## Inode operations — `inode.c`

### Inode allocation: `obsidianfs_get_inode`

`obsidianfs_get_inode` is the main function for creating inodes. It calls the kernel's `new_inode` to obtain a fresh VFS inode, then initialises the ObsidianFS-specific fields (`flagsProtected = false`, `valid_size = 0`) and wires up the correct `inode_operations` and `file_operations` pointers depending on the file type (`S_IFREG`, `S_IFDIR`, `S_IFLNK`, or special). It also assigns `obsidianfs_page_ops` as the `address_space_operations`, which routes all page-cache I/O through the custom read/write folio callbacks.

### File and directory creation: `obsidianfs_create`, `obsidianfs_mknod`

`obsidianfs_create` is the `inode_operations.create` callback, called by the VFS when a user creates a regular file (e.g., via `open(O_CREAT)`). It simply delegates to `obsidianfs_mknod` with the `S_IFREG` flag ORed in. `obsidianfs_mknod` is the generic node creation routine: it calls `obsidianfs_get_inode`, associates the resulting inode with the provided dentry through `d_instantiate`, and updates the parent directory's modification time. This separation keeps `mknod` reusable for regular files, device nodes, and FIFOs.

### Directory creation: `obsidianfs_mkdir`

`obsidianfs_mkdir` is the `inode_operations.mkdir` callback. It delegates to `obsidianfs_mknod` with `S_IFDIR` set, then increments the parent directory's link count via `inc_nlink` to reflect the new `..` entry pointing back to the parent. In the Linux VFS model, each subdirectory contributes one hard link to its parent, so this increment is mandatory for correct link count semantics as reported by `stat`.

### Hard links: `obsidianfs_hardlink`

`obsidianfs_hardlink` implements `inode_operations.link`. A hard link is a new dentry pointing to an already-existing inode. The function increments the inode's reference count with `igrab` (preventing premature eviction), increments the link count with `inc_nlink`, and associates the new dentry with the inode using `d_instantiate`. Both the inode's change time and the parent directory's modification time are updated to reflect the new directory entry.

### Symbolic links: `obsidianfs_symlink`

`obsidianfs_symlink` allocates a new `S_IFLNK` inode and stores the target path string in its page cache using the kernel helper `page_symlink`. If page allocation fails, the inode reference is dropped with `iput` to avoid a leak. The kernel's `page_symlink_inode_operations` (assigned in `obsidianfs_get_inode`) later handles readlink and follow-link transparently, so no additional code is needed for symlink resolution.

### Deletion: `obsidianfs_unlink`

`obsidianfs_unlink` is the `inode_operations.unlink` callback. It first checks `flagsProtected`: if the file is marked as protected, the call returns `-EPERM` immediately. Otherwise it decrements the inode's link count with `drop_nlink` and marks both the inode and parent directory as dirty. When the link count reaches zero and the last file descriptor is closed, the VFS will call `evict_inode` to release the inode's data blocks.

### Attribute modification: `obsidian_setattr`

`obsidian_setattr` is the `inode_operations.setattr` callback, invoked by the VFS for `chmod`, `chown`, `truncate`, and similar operations. It enforces the protection flag first: a protected inode cannot have its attributes modified, including its size. After passing the kernel's `setattr_prepare` validation, it handles size changes via `truncate_setsize` and applies all other attribute changes through the standard `setattr_copy` helper.

---

## File operations — `file.c`

### Write path: `obsidian_write_iter`

`obsidian_write_iter` is the `file_operations.write_iter` callback, called for every `write(2)` system call. It first checks the protection flag and returns `-EPERM` if the file is protected. It then acquires the inode lock to serialise concurrent writers, runs `generic_write_checks` to validate the write parameters (offset, length, file size limits), and delegates the actual data copy to `__generic_file_write_iter`, which routes data through the page cache. After releasing the lock, `generic_write_sync` is called to handle `O_SYNC` semantics if requested.

### Address space operations: `obsidianfs_page_ops`

The `address_space_operations` structure (`obsidianfs_page_ops`) defines how the kernel interacts with ObsidianFS's page cache. It wires `read_folio` to `obsidian_read_folio`, `write_begin` and `write_end` to the custom implementations in `pageops.c`, `dirty_folio` to `block_dirty_folio`, and `invalidate_folio` to `block_invalidate_folio`. The block-based dirty and invalidate helpers are selected in anticipation of the on-disk persistence layer, where folios will have buffer heads attached to physical disk blocks.

---

## Page and block operations — `pageops.c`

### Write preparation: `obsidian_write_begin`

`obsidian_write_begin` is called by the generic write path before data is copied into the page cache. It retrieves (or allocates) the target folio using `__filemap_get_folio` with the `FGP_WRITEBEGIN` flags, which lock the folio and prepare it for writing. If the folio is not yet up-to-date and the write only covers part of it, the function zeroes the unwritten regions with `folio_zero_segments` to prevent accidental exposure of stale kernel memory to userspace.

### Write completion: `obsidian_write_end`

`obsidian_write_end` is called after the data has been copied into the folio. It updates the inode's `i_size` if the write extended the file, and keeps `valid_size` (the highest byte ever written) in sync under the inode mutex. The folio is then marked up-to-date and dirty, unlocked, and its reference count decremented. This function is the authority on the file's logical size: `valid_size` will be essential for the on-disk write path to know which blocks need to be persisted.

### Block path resolution: `obsidian_block_to_path`

`obsidian_block_to_path` translates a logical block number into a sequence of indices (stored in an `offsets[4]` array) that describe the path through the inode's block tree. It follows the classic ext2 design: the first 12 blocks are direct (indexed directly by `i_data[0..11]`); the next range goes through a single indirect block; the following range through a double indirect block; and finally through a triple indirect block. The function returns the depth of the path (1 to 4), which drives the chain walk in `obsidianfs_get_branch`.

### Indirect chain walk: `obsidianfs_get_branch`

`obsidianfs_get_branch` walks the indirect block chain described by `offsets[]`, starting from the inode's `i_data` array. At each level it reads the next block from disk using `sb_bread` and follows the pointer stored at the relevant offset. The walk is protected by the inode's `readwritelock` at each step to detect concurrent modifications. If the chain is complete, the function returns `NULL` (block found). If a pointer is missing (`key == 0`), it returns a pointer to the first unpopulated `Indirect` entry, signalling that allocation is needed.

### Block mapping: `obsidianfs_get_block` / `obsidianfs_get_blocks`

`obsidianfs_get_block` is the callback passed to `mpage_read_folio` and used internally by the write path. It translates a single logical block number into a physical block number by calling `obsidianfs_get_blocks`. `obsidianfs_get_blocks` orchestrates the full block lookup and optional allocation: it calls `obsidian_block_to_path`, walks the chain with `obsidianfs_get_branch`, and if the block is missing and `create` is set, it acquires the inode mutex, computes an allocation goal via `obsidianfs_find_goal`, and calls `obsidianfs_alloc_branch` followed by `obsidianfs_splice_branch`. This function is the central piece of the block layer and is the primary target for the on-disk persistence work.

### Block allocation goal: `obsidianfs_find_goal` / `obsidianfs_find_near`

`obsidianfs_find_goal` selects a preferred physical block number for the next allocation. It first checks `i_block_alloc_info`: if the previous allocation was sequential, it returns the next physical block to encourage contiguous layout on disk. Otherwise it falls back to `obsidianfs_find_near`, which scans the inode's existing block pointers backwards to find a recently used block as a locality hint. If no hint is available, it falls back to the start of the inode's block group (once `s_fs_info` is populated).

### Branch allocation: `obsidianfs_alloc_branch` (stub)

`obsidianfs_alloc_branch` is responsible for allocating a chain of new blocks (both indirect metadata blocks and the final data block) from the filesystem's free block bitmap. It is currently a stub returning `-ENOSPC`, as it requires the on-disk block group descriptor table and bitmap blocks to be read via `s_fs_info`. Implementing this function is the primary remaining step for full disk persistence.

### Branch splice: `obsidianfs_splice_branch`

`obsidianfs_splice_branch` links a newly allocated branch into the inode's block tree by writing the first block number of the new chain into the appropriate slot of the parent indirect block or `i_data` array (`*where->p = where->key`). It also updates the `i_block_alloc_info` structure with the last allocated logical and physical block numbers, which `obsidianfs_find_goal` uses on the next write to maintain sequential allocation.

---

## ioctl interface — `ioctlops.c`

### File protection: `OBSIDIAN_IOC_PROTECT`

ObsidianFS introduces a custom `ioctl` command `OBSIDIAN_IOC_PROTECT` that sets the `flagsProtected` flag on an inode. Once set, any attempt to write to, truncate, or modify the attributes of that file will be rejected with `-EPERM`. This flag is enforced in `obsidian_write_iter`, `obsidian_setattr`, and `obsidianfs_unlink`. The protection is volatile (stored only in the in-memory inode) (for the moment).

### Flag query: `OBSIDIAN_IOC_GET_FLAGS`

`OBSIDIAN_IOC_GET_FLAGS` allows userspace to query the current protection state of a file. The ioctl handler reads `flagsProtected` from the inode and copies a single byte (1 if protected, 0 otherwise) back to userspace via `copy_to_user`. This provides a clean introspection interface without requiring access to kernel internals. (Not implement yet user side).

---

## Userspace tool — `obsidiancommand.c`

`obsidiancommand` is a minimal C program that opens a file by path and issues the `OBSIDIAN_IOC_PROTECT` ioctl to mark it as protected. It serves as both a practical utility and a reference implementation showing how to use the custom ioctl interface from userspace. The `ioctlops.h` header uses an `#ifdef __KERNEL__` guard so that the ioctl command definitions (`_IO` macros) can be shared between the kernel module and the userspace program without conflict.

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

### Mount (RAM mode)

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

ObsidianFS compiles cleanly with linux kernel **6.19** and is fully functional as an in-memory filesystem. The following components are implemented and operational:

- VFS registration and mount lifecycle
- Regular files, directories, symbolic links, hard links, and special files
- Read and write through the page cache
- Custom ioctl-based file protection
- ext2-style indirect block addressing (lookup path only)

The following work remains before the first persistent on-disk version:

1. **On-disk format** — define `obsidianfs_super_block`, `obsidianfs_inode_disk`, and `obsidianfs_group_desc` structures.
2. **`obsidianfs_fill_super`** — read and validate the on-disk superblock; populate `s_fs_info`.
3. **`obsidianfs_alloc_branch`** — implement block allocation from the free block bitmap.
4. **`write_inode` / `evict_inode`** — persist inode metadata to disk on eviction.
5. **`mkfs.obsidianfs`** — userspace formatting tool to initialise the on-disk structures.
6. **`kill_block_super` + `get_tree_bdev`** — switch from RAM mount to block device mount.

---

## License

GPL-2.0-only