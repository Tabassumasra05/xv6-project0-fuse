// disk.c - low-level access to fs.img: blocks, inodes, file data
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "xv6fs.h"

static int fd = -1;          // fs.img, opened for reading and writing
struct superblock sb;

// Open fs.img and read its superblock. Returns 0 on success, -1 on error.
int disk_open(const char *path) {
  fd = open(path, O_RDWR);
  if (fd < 0) { perror(path); return -1; }

  char buf[BSIZE];
  read_block(1, buf);                      // superblock lives in block 1
  memcpy(&sb, buf, sizeof(sb));

  if (sb.magic != FSMAGIC) {
    fprintf(stderr, "%s: not an xv6 file system (magic = 0x%x)\n", path, sb.magic);
    return -1;
  }

  // The log's first block holds a count of unfinished writes.
  // If it is not 0, xv6 was stopped in the middle of writing.
  int pending;
  read_block(sb.logstart, buf);
  memcpy(&pending, buf, sizeof(pending));
  if (pending != 0)
    fprintf(stderr, "warning: xv6 log has %d unfinished blocks; "
                    "boot xv6 once and shut it down cleanly first\n", pending);
  return 0;
}

// Copy block number bno of fs.img into buf (BSIZE bytes).
void read_block(uint32_t bno, void *buf) {
  if (pread(fd, buf, BSIZE, (off_t)bno * BSIZE) != BSIZE)
    memset(buf, 0, BSIZE);
}

// Write BSIZE bytes from buf into block number bno of fs.img.
void write_block(uint32_t bno, const void *buf) {
  if (pwrite(fd, buf, BSIZE, (off_t)bno * BSIZE) != BSIZE)
    perror("write_block");
}

// Copy inode number inum into *ip.
void read_inode(uint32_t inum, struct dinode *ip) {
  char buf[BSIZE];
  read_block(sb.inodestart + inum / IPB, buf);
  memcpy(ip, buf + (inum % IPB) * sizeof(struct dinode), sizeof(*ip));
}

// Save *ip as inode number inum (read the block, change one inode, write it back).
void write_inode(uint32_t inum, const struct dinode *ip) {
  char buf[BSIZE];
  uint32_t bno = sb.inodestart + inum / IPB;
  read_block(bno, buf);
  memcpy(buf + (inum % IPB) * sizeof(struct dinode), ip, sizeof(*ip));
  write_block(bno, buf);
}

// Which disk block holds the n-th block of this file?  (0 = none)
// n = 0..11   -> addrs[n]                  (direct)
// n = 12..267 -> entry n-12 of the indirect block addrs[12]
uint32_t block_of(const struct dinode *ip, uint32_t n) {
  if (n < NDIRECT)
    return ip->addrs[n];

  n -= NDIRECT;
  if (n >= NINDIRECT || ip->addrs[NDIRECT] == 0)
    return 0;

  uint32_t ind[NINDIRECT];
  read_block(ip->addrs[NDIRECT], ind);
  return ind[n];
}

// Copy up to n bytes of the file, starting at byte off, into dst.
// Returns how many bytes were copied (0 at end of file).
int read_data(const struct dinode *ip, char *dst, uint32_t off, uint32_t n) {
  if (off >= ip->size) return 0;
  if (off + n > ip->size) n = ip->size - off;   // don't read past the end

  uint32_t done = 0;
  char buf[BSIZE];
  while (done < n) {
    uint32_t bno   = block_of(ip, off / BSIZE);
    uint32_t start = off % BSIZE;                 // position inside block
    uint32_t chunk = BSIZE - start;               // bytes left in block
    if (chunk > n - done) chunk = n - done;

    if (bno == 0) memset(buf, 0, BSIZE);          // missing block = zeros
    else          read_block(bno, buf);

    memcpy(dst + done, buf + start, chunk);
    done += chunk;
    off  += chunk;
  }
  return done;
}

// Like block_of, but if the n-th block is missing, allocate it (and the
// indirect block too, if needed). Changes ip->addrs, so the caller must
// save the inode with write_inode afterwards.
// Returns the block number, or 0 if the disk is full.
uint32_t block_of_alloc(struct dinode *ip, uint32_t n) {
  if (n < NDIRECT) {
    if (ip->addrs[n] == 0)
      ip->addrs[n] = balloc();
    return ip->addrs[n];
  }

  n -= NDIRECT;
  if (n >= NINDIRECT) return 0;                   // past xv6's max file size

  if (ip->addrs[NDIRECT] == 0) {                  // no indirect block yet
    ip->addrs[NDIRECT] = balloc();
    if (ip->addrs[NDIRECT] == 0) return 0;
  }

  uint32_t ind[NINDIRECT];
  read_block(ip->addrs[NDIRECT], ind);
  if (ind[n] == 0) {
    ind[n] = balloc();
    if (ind[n] == 0) return 0;
    write_block(ip->addrs[NDIRECT], ind);         // save the new entry
  }
  return ind[n];
}

// Copy n bytes from src into the file at byte off, growing it if needed.
// Changes *ip (size, addrs); the caller must save it with write_inode.
// Returns bytes written, or a negative error (-EFBIG, -ENOSPC).
int write_data(struct dinode *ip, const char *src, uint32_t off, uint32_t n) {
  uint32_t max = MAXFILE * BSIZE;                 // xv6 files max 268 KB
  if (off >= max) return -EFBIG;
  if (off + n > max) n = max - off;               // write only what fits

  if (off > ip->size) {                           // writing past the end:
    char zero[BSIZE] = {0};                       // fill the gap with zeros
    while (ip->size < off) {
      uint32_t gap = off - ip->size;
      if (gap > BSIZE) gap = BSIZE;
      int r = write_data(ip, zero, ip->size, gap);
      if (r < 0) return r;
    }
  }

  uint32_t done = 0;
  char buf[BSIZE];
  while (done < n) {
    uint32_t bno = block_of_alloc(ip, off / BSIZE);
    if (bno == 0) return done > 0 ? (int)done : -ENOSPC;

    uint32_t start = off % BSIZE;
    uint32_t chunk = BSIZE - start;
    if (chunk > n - done) chunk = n - done;

    read_block(bno, buf);                         // keep the rest of the block
    memcpy(buf + start, src + done, chunk);
    write_block(bno, buf);

    done += chunk;
    off  += chunk;
    if (off > ip->size) ip->size = off;           // file grew
  }
  return done;
}

// Make the file exactly new_size bytes long.
// Shrinking frees the blocks no longer needed (and the indirect block
// if nothing is left in it). Growing fills with zeros.
// Changes *ip; the caller must save it with write_inode.
int trunc_data(struct dinode *ip, uint32_t new_size) {
  if (new_size > (uint64_t)MAXFILE * BSIZE) return -EFBIG;

  if (new_size > ip->size) {                      // grow
    char zero[BSIZE] = {0};
    while (ip->size < new_size) {
      uint32_t chunk = new_size - ip->size;
      if (chunk > BSIZE) chunk = BSIZE;
      int r = write_data(ip, zero, ip->size, chunk);
      if (r < 0) return r;
    }
    return 0;
  }

  uint32_t keep = (new_size + BSIZE - 1) / BSIZE; // blocks still needed

  for (uint32_t i = keep; i < NDIRECT; i++) {     // free direct blocks
    if (ip->addrs[i]) { bfree(ip->addrs[i]); ip->addrs[i] = 0; }
  }

  if (ip->addrs[NDIRECT]) {                       // free indirect blocks
    uint32_t ind[NINDIRECT];
    read_block(ip->addrs[NDIRECT], ind);
    uint32_t from = keep > NDIRECT ? keep - NDIRECT : 0;
    for (uint32_t i = from; i < NINDIRECT; i++) {
      if (ind[i]) { bfree(ind[i]); ind[i] = 0; }
    }
    if (from == 0) {                              // indirect block now empty
      bfree(ip->addrs[NDIRECT]);
      ip->addrs[NDIRECT] = 0;
    } else {
      write_block(ip->addrs[NDIRECT], ind);
    }
  }

  // Zero the unused tail of the last kept block, so growing the file
  // later shows zeros and not old data.
  if (new_size % BSIZE != 0) {
    uint32_t bno = block_of(ip, new_size / BSIZE);
    if (bno) {
      char buf[BSIZE];
      read_block(bno, buf);
      memset(buf + new_size % BSIZE, 0, BSIZE - new_size % BSIZE);
      write_block(bno, buf);
    }
  }

  ip->size = new_size;
  return 0;
}