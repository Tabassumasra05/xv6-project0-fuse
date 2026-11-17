# xv6 FUSE File System

Mounts xv6's `fs.img` on Linux using **libfuse 3 (C)**, so normal Linux commands
(`ls`, `cat`, `cp`, `mkdir`, `rm`, `truncate`) read and write xv6's disk directly.
Files written on Linux appear inside xv6, and files written inside xv6 appear on Linux.

## Team and contributions

| # | Member | Roll No. | Contribution | Files |
|---|---|---|---|---|
| 1 | Asra Tabassum | IIT2025063 | Disk format and reading: superblock, inodes, direct and indirect blocks | `xv6fs.h`, `disk.c` |
| 2 | Pooja Chaudhari | IIT2025061 | Bitmap and block allocation, writing and growing files | `alloc.c`, `disk.c` |
| 3 | Nisha Kumari | IIT2025048 | Truncate and delete (rm, rmdir), Linux test script | `disk.c`, `xv6fs.c`, `tests/linux_test.sh` |
| 4 | Mihika Singh | IIT2025055 | Directories and paths, creating files and folders (create, mkdir) | `dirops.c`, `xv6fs.c` |
| 5 | Rahi Birajdar | IIT2025027 | FUSE integration (mounting, getattr, readdir, read, write), xv6 test script | `xv6fs.c`, `Makefile`, `tests/xv6_test.sh` |
| 6 | Dhruva Shah | IIT2025010 | fsck-style consistency checker | `fsck_xv6.c` |

All members tested the final system together and wrote this README.

## Requirements
Ubuntu (tested on Ubuntu under WSL2), with:
```bash
sudo apt install -y fuse3 libfuse3-dev pkg-config build-essential
```
and an xv6-riscv build (`xv6-riscv/fs.img` must exist; run `make qemu` once in `xv6-riscv`).

## Build
```bash
make                                                      # builds ./xv6fs
gcc -Wall -Wextra -o fsck_xv6 fsck_xv6.c disk.c alloc.c   # builds ./fsck_xv6
```
(The test scripts build both automatically.)

## Run
```bash
./xv6fs xv6-riscv/fs.img mnt     # mount
ls -l mnt                        # use it like any folder
fusermount3 -u mnt               # unmount (always do this before booting xv6)
```
Add `-f` at the end to keep the program in the foreground (useful for debugging).

**Never mount fs.img while xv6 is running.** Two programs writing the same disk will corrupt it.

## Test
```bash
./tests/linux_test.sh            # 33 Linux-side tests, prints PASS/FAIL
./tests/xv6_test.sh              # boots xv6: Linux -> xv6 and xv6 -> Linux (10 tests)
./fsck_xv6 xv6-riscv/fs.img      # consistency check only
```

**`tests/linux_test.sh`** runs fsck before and after; copies small (5 KB), medium (100 KB, indirect
block) and maximum-size (268 KB) files and compares them byte by byte; checks that a file over the
limit is refused; tests overwrite, append, truncate (shrink and grow), mkdir, nested folders,
14/15-character names, unmount + remount, rm and rmdir; and checks that every freed block is
returned to the bitmap.

**`tests/xv6_test.sh`** writes two files from Linux (one is 17 KB, so it uses the indirect block),
boots xv6 in QEMU and types commands into it automatically (`cat`, `wc`, then creates a file and a
folder inside xv6). It quits QEMU with Ctrl-A X, mounts on Linux and reads the files xv6 made.
fsck runs after every step, and the test files are removed at the end.

## Output
Screenshots of the real runs are in `output/`:

| File | Shows |
|---|---|
| `1_linux_test.png` | `tests/linux_test.sh`: 33 passed, 0 failed |
| `2_xv6_test.png` | `tests/xv6_test.sh`: 10 passed, 0 failed |
| `3_linux_to_xv6.png` | File written on Linux, read with `cat` inside xv6 |
| `4_xv6_to_linux.png` | File and folder created inside xv6, read on Linux |
| `5_fsck.png` | `fsck_xv6` on the real disk: CONSISTENT |

## Files
| File | Purpose |
|---|---|
| `xv6fs.h` | On-disk structs copied from `xv6-riscv/kernel/fs.h` (`superblock`, `dinode`, `dirent`), constants, shared function list |
| `disk.c` | Open the image, read/write blocks and inodes, map file block → disk block (direct + indirect), read/write/truncate file data |
| `alloc.c` | Bitmap: allocate/free data blocks (`balloc`, `bfree`); allocate inodes (`ialloc`) |
| `dirops.c` | Directory lookup, path → inode, add/remove directory entries |
| `xv6fs.c` | FUSE callbacks and `main()` |
| `fsck_xv6.c` | Read-only consistency checker |
| `tests/linux_test.sh` | Automatic Linux-side tests (read, write, big files, truncate, mkdir, rm, fsck) |
| `tests/xv6_test.sh` | Boots xv6 in QEMU and checks both directions automatically |
| `Makefile` | Builds `xv6fs` |

## How it works

### Disk layout (1024-byte blocks)
```
| boot | superblock | log | inodes | bitmap | data blocks ... |
   0        1         2..
```
The superblock (block 1) gives the start of each region. Inodes are 64 bytes, 16 per block.
Each inode has 12 direct block addresses and 1 indirect block holding 256 more,
so the maximum file size is (12 + 256) × 1024 = 274,432 bytes.
A directory is a file of 16-byte entries: 2-byte inode number + 14-byte name. The root is inode 1.

### FUSE operations
| Operation | Linux command | What it does on fs.img |
|---|---|---|
| `getattr` | `ls -l`, `stat` | Path → inode; return type, size, link count |
| `readdir` | `ls` | List the non-empty entries of a directory |
| `read` | `cat` | Map offset → block (direct or indirect), copy bytes |
| `write` | `cp`, `echo >` | Allocate missing blocks (and the indirect block) via the bitmap, write bytes, grow size |
| `create` | `touch`, `cp` | Allocate an inode (`T_FILE`, nlink = 1), add a directory entry |
| `mkdir` | `mkdir` | Allocate a `T_DIR` inode, add `.` and `..`, add the entry, parent `nlink++` (same as xv6) |
| `unlink` | `rm` | Remove the entry, `nlink--`; at 0 free all blocks (including indirect) and the inode |
| `truncate` | `truncate`, `echo >` | Shrink: free blocks past the new size (and the indirect block if unused). Grow: fill with zeros |
| `rmdir` (extra) | `rmdir`, `rm -r` | Remove an empty directory, parent `nlink--` |
| `open` | any open | Handles `O_TRUNC` (FUSE 3 passes truncation here) |

### Consistency checker (`fsck_xv6`)
Checks the superblock layout; every inode's type, size and block numbers; that no block is
used twice; that the bitmap marks exactly the used blocks; that every directory starts with
`.` and `..` and only points to used inodes; link counts (file nlink = number of names,
directory nlink = 1 + number of sub-directories, as xv6 does); and that every used inode is
reachable from `/`. It was also tested on deliberately damaged images (cleared/extra bitmap
bits, shared block, wrong nlink, orphan inode, bad block number, missing `.`, bad magic),
and every case was reported.

## Limitations
- xv6 stores no timestamps, owners or permissions, so these are not saved (files show as the current user).
- `rename`, hard links and symlinks are not implemented (not required).
- File names are at most 14 characters; files at most 274,432 bytes (xv6 limits).
- Requests run one at a time (`-s`), so writes never race.
- The xv6 log is not replayed. If the log has unfinished entries, the program prints a warning;
  boot xv6 once and shut it down normally first.