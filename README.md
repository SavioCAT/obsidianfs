# ObsidianFS

ObsidianFS is a ransomware-resistant filesystem implemented as a Linux kernel module in C, developed as my MSc dissertation project at Heriot-Watt University. It combines a versioned copy-on-write mechanism, which keeps a protected snapshot of every file before each modification, with an irreversible protection flag that makes a file immutable even for a process running with root privileges.

The starting point for the on-disk layout and block allocation is the ext2 model, adapted and extended for the immutability and versioning requirements described below. The filesystem was validated with the file system exerciser `fsx`, deployed as the root filesystem of a backup server running Kopia (an open-source backup tool whose repository then benefits from ObsidianFS's guarantees), and tested against a real ransomware sample, the Linux version of Hive. In every tested scenario, at least one clean copy of the data survived the infection and was recoverable with a single rollback command, even when the malware was executed with root privileges on the backup server itself.

---

## Why this filesystem exists

Current recovery solutions such as centralised backups or snapshots share an important weakness: the backup data itself can be encrypted or deleted by an attacker who obtains sufficient privileges (this is exactly what happened with WannaCry, which deleted Windows Volume Shadow Copies before encrypting). The problem is no longer only to prevent a ransomware infection, it's to recover from it as quickly as possible, and current solutions are too often disabled or destroyed by the very attacker they are supposed to protect against.

ObsidianFS is designed around five principles:

1. **Immutability by Default** — once a file is placed under ObsidianFS protection, no process, including one running with root privileges, can alter it. Every historical version produced by the copy-on-write mechanism is automatically protected at creation, so the version history itself cannot be rewritten.
2. **Sub-24-Hour Recovery** — restoration should take a single rollback command, independent of the file size.
3. **Network-Agnostic Operation** — the filesystem must remain fully operational even on a partially or fully compromised network.
4. **Zero Trust Architecture** — no host is implicitly trusted to alter data already stored. A compromised client can add data, never delete or overwrite an existing version.
5. **Automated backups and restoration** — minimal human intervention, both to trigger backups and to restore them.

---

## Project structure

```
obsidianfs/
├── inode.h            — on-disk inode struct, in-memory inode_meta, dir entry format
├── super.h            — on-disk superblock struct, in-memory sb_info, disk layout constants
├── pageops.h          — block addressing types: Indirect, obsidianfs_block_alloc_info
├── addressbitmap.h    — inode/block bitmap helpers
├── cow.h / cow.c       — versioned copy-on-write mechanism, version chain, retention policy
├── file.h             — exported symbols for file and directory operations
├── ioctlops.h         — ioctl command definitions (shared with userspace)
├── super.c            — superblock, module init/exit, inode slab, write_inode
├── inode.c            — inode lifecycle: iget, create, directory ops, bitmap allocator, dir entries
├── file.c             — file_operations, address_space_operations, mmap, copy_file_range
├── pageops.c          — page cache I/O, ext2-style indirect block mapping and block allocator
├── ioctlops.c         — ioctl handler (protect, revert, forward, dentry listing)
├── mkfs.obsidianfs.c   — userspace tool to initialise a blank device
├── obsidiancommand.c   — userspace CLI (protect / rollback / forward / listdentry)
└── obsidian-install    — installer script, deploys a Debian system on ObsidianFS with Kopia
```

---

## On-disk layout

The device is laid out the same way as ext2, plus the fields needed for versioning:

| Block(s) | Description |
|---|---|
| 0 | Superblock |
| 1 ... 128 | Inode bitmap |
| 129 ... 9442 | Block bitmap |
| 9443 ... first data block - 1 | Inode table |
| first data block ... end of device | Raw data |

### `obsidianfs_super_block` (`super.h`)

Stored at block 0, padded to 512 bytes. Holds the device geometry: total block and inode counts, free counters, first data block, block size exponent, inode size, mount/write timestamps, a 128-bit UUID, and a volume name.

### `obsidianfs_inode` (`inode.h`)

The on-disk inode is exactly 128 bytes. It follows the ext2 model for the 15 block pointers (12 direct, one simple indirect, one double indirect, one triple indirect), with a few additions specific to ObsidianFS:

- `i_size` is `__le64` — files up to 2^63 bytes.
- `i_uid` / `i_gid` are `__le32` — UIDs/GIDs above 65535 are supported.
- `i_flagsProtected` — the immutability flag, persisted on disk. Once set, it is never cleared again.
- `i_next_inode` / `i_previous_inode` — the two links of the version chain used by the copy-on-write mechanism (see below).

### `obsidianfs_dir_entry` (`inode.h`)

Classic ext2-style variable-length directory entry: inode number, record length, name length, and the name itself.

---

## Core data structures — `inode.h`

Every inode managed by ObsidianFS is represented by an `obsidianfs_inode_meta` structure, which embeds the kernel's generic `struct inode` as its first member, the usual pattern to allow safe casting via `container_of` (the helper `OBSIDIANFS_INODE(inode)` does this). Beyond the standard VFS inode, it carries a `struct mutex` (`i_lock`) protecting block allocation, an `rwlock_t` guarding the indirect block chain, the `flagsProtected` boolean, the mirrored block pointers, and the `i_next_inode` / `i_previous_inode` fields of the version chain.

---

## Persistence

Persistence relies on three things: device initialisation, serialisation/deserialisation of inodes, and block allocation through the bitmaps.

- **`mkfs.obsidianfs`** writes the superblock, zeroes the inode and block bitmaps (marking the root inode and the metadata blocks as used), zeroes the inode table with the root inode placed at position 0, and zeroes the data area.
- **`obsidianfs_iget`** / the writeback path in `super.c` handle the conversion between the on-disk `obsidianfs_inode` and the in-memory `obsidianfs_inode_meta`, with all multi-byte fields converted through `htole32`/`htole16` on write and back on read.
- **Block allocation** (`pageops.c`) follows the classic ext2 indirect-block model with two goals: locality (`obsidianfs_find_goal` / `obsidianfs_find_near` try to keep a file's blocks close to each other) and contiguity (`obsidianfs_alloc_blocks` looks for one single free stretch big enough for the whole request). Unlike ext2, the current allocator does not fall back to a fragmented allocation if no single stretch is found — a device that is fragmented but not full can report `ENOSPC` before it's really full. This is a known limitation, left for future work.

Correctness of the persistence layer was validated by stress-testing ObsidianFS with `fsx` (File System eXerciser): buffered and memory-mapped reads/writes, truncations, `fallocate`, hole punching, and `copy_file_range`. A clean run of 1,000,000 operations completed with no divergence from the reference model.

---

## Immutability

The protection is enforced in two complementary ways.

**At the inode level**, every VFS operation able to alter or destroy a file checks `flagsProtected` before doing anything: writes (`obsidian_write_iter`), truncation and attribute changes (`obsidian_setattr`), deletion (`obsidianfs_unlink`), and writable shared memory mappings (`obsidianfs_file_mmap` refuses the mapping outright if the file is protected). This rejection happens regardless of the calling process's privileges, root included. Crucially, there is deliberately no "unprotect" ioctl — once `OBSIDIAN_IOC_PROTECT` has been issued, the flag can never be cleared again, which prevents a compromised administrator account from simply unprotecting a file before altering it.

**At the kernel level**, the filesystem is meant to be run on a kernel built with `CONFIG_BLK_DEV_WRITE_MOUNTED=n`, which forbids opening a mounted block device for writing. This closes the bypass where a process ignores the filesystem entirely and writes raw data straight to the device (e.g. `dd if=/dev/zero of=/dev/sdb1`).

One weak point remains: ObsidianFS does not intercept the rename operation, so a protected file can still be renamed (its data cannot be touched, but a ransomware can still pollute the file names). This is a minor issue compared to data encryption, but it's on the list of things to fix.

Also, the immutability guarantee assumes the kernel itself isn't compromised. An attacker who already holds root could in principle load a malicious kernel module or boot a modified kernel, at which point the module-level checks no longer apply. Defending against that is out of scope for a software-only solution — it would need hardware support such as measured boot with kernel module signing, or write-once storage.

---

## Versioned copy-on-write

Protecting a file only solves half of the problem, a user still needs to keep working on their files while preserving the ability to go back to a known-good state. This is what the copy-on-write mechanism in `cow.c` is for.

When a write is about to modify a non-empty file, `obsidianfs_cow_inode` duplicates the inode metadata and every one of its data blocks into a new inode, which becomes a protected snapshot of the content exactly as it was before the modification. The dentry (the name) keeps pointing at the original inode, which is reused for the live, still-writable version, while the snapshot is only reachable through the version chain, it never gets a directory entry of its own. Blocks are copied, never shared between versions, so a later write to the live file can never reach the bytes of any previous version, at the cost of some storage efficiency (this is the main trade-off compared to a block-sharing design like ZFS).

The chain itself is a simple doubly linked list carried by two inode fields, `i_previous_inode` and `i_next_inode`. The head (the live version) has `i_next_inode = 0`, and the oldest snapshot closes the list with `i_previous_inode = 0`. Two ioctls navigate this chain without ever copying data:

- `OBSIDIAN_IOC_REVERT` redirects every dentry aliased to the current inode to the previous version in the chain.
- `OBSIDIAN_IOC_FORWARD` does the mirror operation using `i_next_inode`.

Because the operation only redirects directory entries, restoring a previous version is cheap and independent of the file size.

Keeping every version forever isn't sustainable, since each write duplicates the whole set of data blocks. `obsidianfs_max_backup` therefore caps the chain at `OBSIDIANFS_MAX_BACKUPS` (currently 20, defined in `cow.h`) every time a new snapshot is linked in: the chain is cut at the limit, and everything past the cut is detached and prepared for reclaim by `obsidianfs_evict_inode`. This retention policy is enforced eagerly at every write, so no separate garbage-collector thread is needed. Note that this is also a known weak point: a ransomware aware of the limit could rewrite the same file more than `OBSIDIANFS_MAX_BACKUPS` times to push the clean version out of the chain. A time-based retention policy would be a better answer for critical data, and is left for future work.

---

## Backup layer (Kopia)

ObsidianFS does not reimplement a backup client/server stack. Instead, the `obsidian-install` script deploys a full Debian system with ObsidianFS as its root filesystem and sets up [Kopia](https://kopia.io) (Apache 2.0) as the backup service: a TLS-protected Kopia server exposes a repository that lives on ObsidianFS, so every blob Kopia writes benefits from the immutability and versioning described above. Even if an attacker gains root on the backup server and tries to encrypt or delete the repository, the copy-on-write chain keeps a clean, protected copy of every blob, recoverable with the same rollback mechanism used for a single file.

---

## Userspace tools

### `mkfs.obsidianfs`

Initialises a blank block device (or image file) with a fresh superblock, bitmaps, inode table, and root inode.

### `obsidiancommand` (built as `commandBin`)

```
obsidiancommand <protect|listdentry|rollback|forward> <file|dir>
```

Applies the chosen ioctl to a file, or recursively to every regular file in a directory. `protect` sets the immutability flag, `rollback`/`forward` walk the version chain, `listdentry` prints every dentry alias of an inode (debug helper).

---

## Build and usage

### Requirements

- Linux kernel headers for the running kernel (built and tested against 6.19)
- GCC, make

### Compilation

```bash
make          # builds the kernel module (.ko) and the userspace tools
make clean    # removes all build artefacts
```

### Quick test on a loopback image

```bash
sudo make load    # insmod, creates a test image, mkfs
sudo make unload   # umount + rmmod, removes the test image
```

### Protect a file, then try to overwrite it

```bash
echo "important data" > /tmp/obsidianfs/myfile
./commandBin protect /tmp/obsidianfs/myfile
echo "overwrite attempt" > /tmp/obsidianfs/myfile   # Permission denied
```

### Recover a file overwritten without protection

```bash
echo "clean content" > /tmp/obsidianfs/myfile
echo "encrypted content" > /tmp/obsidianfs/myfile   # this write triggers CoW automatically
./commandBin rollback /tmp/obsidianfs/myfile
cat /tmp/obsidianfs/myfile   # back to "clean content"
```

### Full deployment with Kopia

`sudo ./obsidian-install` boots a live system, partitions and formats a disk with `mkfs.obsidianfs`, installs a full Debian system on it, and configures a TLS-protected Kopia server whose repository lives on the freshly created ObsidianFS partition.

---

## Validation

- **Functional correctness**: `fsx`, 1,000,000 operations, no divergence from the reference model.
- **Real malware**: tested against the Linux build of the Hive ransomware (from theZoo, in an isolated, air-gapped VM) run with root privileges, both against a regular workstation file and against a live Kopia repository stored on ObsidianFS. In both cases, the encryption itself was not blocked, but a single rollback recovered the clean content. A file explicitly marked protected was never altered at all (confirmed by comparing SHA-1 hashes before and after the attack).
- **Performance**: read speed is on par with ext2 (no CoW involved on the read path). Write speed pays the cost of the design, around 4 times slower than ext2 in sequential write, mainly because of the block duplication on every write and the synchronous inode writeback. This trade-off is acceptable for a backup server, where writes only happen during the backup itself and the read speed, which conditions restoration time, is not degraded at all.

---

## Known limitations / future work

- Rename is not intercepted, so a ransomware can still rename a protected file even though it cannot touch its content.
- The retention policy (`OBSIDIANFS_MAX_BACKUPS`) could be exhausted by a ransomware aware of the limit; a time-based policy would be more robust.
- The block allocator needs a single contiguous stretch per request and does not fall back to a fragmented allocation, so a fragmented-but-not-full device can report `ENOSPC` early.
- Each CoW duplicates the whole file, so a massive encryption run fills the device quickly, which is a denial-of-service risk once the device is full.
- The whole security model assumes an uncompromised kernel; defending against an attacker who can load a malicious module or boot a different kernel is out of scope here and would need hardware-backed measures (secure/measured boot, kernel module signing, write-once media).

---

## License

ObsidianFS is a Linux kernel module, which places it under the license of the kernel itself. The Linux kernel is released under the GNU General Public License version 2 (GPL-2.0), and any code compiled and linked against internal kernel symbols, as ObsidianFS is, is considered a derivative work of the kernel for licensing purposes. Consequently, **ObsidianFS is released under the GPL-2.0 licence**, in full compliance with the kernel's own licensing terms.

Kopia, used as the backup layer on top of ObsidianFS, is released under the Apache 2.0 licence and is not modified by this project, ObsidianFS only exposes a standard POSIX interface that Kopia uses like any other filesystem, so no additional obligation applies on that side.