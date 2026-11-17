// alloc.c - the bitmap (which blocks are used) and free inodes.
// Same rules as xv6's balloc / bfree / ialloc in kernel/fs.c.
#include <string.h>
#include "xv6fs.h"

#define BPB (BSIZE * 8)                    // bitmap bits per block = 8192

// Find a free block, mark it used in the bitmap, fill it with zeros.
// Returns the block number, or 0 if the disk is full.
uint32_t balloc(void) {
  char buf[BSIZE];
  uint32_t first_data = sb.size - sb.nblocks;   // blocks before this are metadata

  for (uint32_t base = 0; base < sb.size; base += BPB) {
    uint32_t bmap_bno = sb.bmapstart + base / BPB;
    read_block(bmap_bno, buf);

    for (uint32_t bit = 0; bit < BPB && base + bit < sb.size; bit++) {
      uint32_t b = base + bit;
      if (b < first_data) continue;              // never hand out metadata blocks
      unsigned char mask = 1 << (bit % 8);
      if ((buf[bit / 8] & mask) == 0) {          // 0 = free
        buf[bit / 8] |= mask;                    // mark used
        write_block(bmap_bno, buf);

        char zero[BSIZE] = {0};                  // new block starts empty
        write_block(b, zero);
        return b;
      }
    }
  }
  return 0;                                      // no free block
}

// Mark block b as free in the bitmap.
void bfree(uint32_t b) {
  char buf[BSIZE];
  uint32_t bmap_bno = sb.bmapstart + b / BPB;
  uint32_t bit = b % BPB;

  read_block(bmap_bno, buf);
  buf[bit / 8] &= ~(1 << (bit % 8));             // 0 = free
  write_block(bmap_bno, buf);
}

// Find a free inode (type == 0), give it `type`, save it.
// Returns the inode number, or -1 if all inodes are used.
int ialloc(int16_t type) {
  struct dinode d;
  for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
    read_inode(inum, &d);
    if (d.type == 0) {                           // 0 = free
      memset(&d, 0, sizeof(d));
      d.type = type;
      write_inode(inum, &d);
      return inum;
    }
  }
  return -1;
}