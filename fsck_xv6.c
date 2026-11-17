// fsck_xv6.c - checks an xv6 fs.img for mistakes (read-only, changes nothing).
// Build: gcc -Wall -Wextra -o fsck_xv6 fsck_xv6.c disk.c alloc.c
// Run:   ./fsck_xv6 xv6-riscv/fs.img
//
// What it checks:
//  1. superblock: magic number, regions in the right order and sizes
//  2. every used inode: valid type, block numbers inside the data area,
//     enough blocks for its size, no blocks past the end of the file
//  3. no block is used by two inodes (or twice by one)
//  4. bitmap: every used block is marked 1, every unused data block is 0
//  5. folders: first entry ".", second "..", entries point to used inodes
//  6. link counts: file nlink = number of names; folder nlink = 1 + subfolders
//  7. every used inode can be reached from "/" (no lost inodes)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xv6fs.h"

static int errors = 0;

// Print one problem and count it.
#define ERR(...) do { printf("ERROR: "); printf(__VA_ARGS__); printf("\n"); errors++; } while (0)

static uint32_t data_start;    // first data block
static uint32_t *owner;        // owner[b] = inode using block b (0 = nobody)
static int *names;             // names[i] = how many folder entries point to inode i
static int *subdirs;           // subdirs[i] = how many sub-folders folder i has
static char *reached;          // reached[i] = 1 if inode i can be reached from "/"

// Is bit b set in the bitmap?
static int bitmap_used(uint32_t b) {
  unsigned char buf[BSIZE];
  read_block(sb.bmapstart + b / (BSIZE * 8), buf);
  uint32_t bit = b % (BSIZE * 8);
  return (buf[bit / 8] >> (bit % 8)) & 1;
}

// Record that inode inum uses block b. Reports bad or duplicate blocks.
static void claim(uint32_t inum, uint32_t b, const char *what) {
  if (b < data_start || b >= sb.size) {
    ERR("inode %u: %s block %u is outside the data area (%u..%u)",
        inum, what, b, data_start, sb.size - 1);
    return;
  }
  if (owner[b] != 0)
    ERR("block %u is used by inode %u and inode %u", b, owner[b], inum);
  else
    owner[b] = inum;
}

// ---- 1. superblock ----
static int check_superblock(void) {
  if (sb.magic != FSMAGIC) { ERR("bad magic 0x%x", sb.magic); return 0; }

  uint32_t ninodeblocks = sb.ninodes / IPB + 1;
  uint32_t nbitmap      = sb.size / (BSIZE * 8) + 1;
  data_start = sb.size - sb.nblocks;

  if (sb.logstart != 2)
    ERR("superblock: logstart is %u, expected 2", sb.logstart);
  if (sb.inodestart != sb.logstart + sb.nlog)
    ERR("superblock: inodestart is %u, expected %u", sb.inodestart, sb.logstart + sb.nlog);
  if (sb.bmapstart != sb.inodestart + ninodeblocks)
    ERR("superblock: bmapstart is %u, expected %u", sb.bmapstart, sb.inodestart + ninodeblocks);
  if (data_start != sb.bmapstart + nbitmap)
    ERR("superblock: data starts at %u, expected %u", data_start, sb.bmapstart + nbitmap);
  return 1;
}

// ---- 2 & 3. inodes and their blocks ----
static void check_inodes(void) {
  for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
    struct dinode d;
    read_inode(inum, &d);
    if (d.type == 0) continue;                       // free inode

    if (d.type != T_DIR && d.type != T_FILE && d.type != T_DEVICE) {
      ERR("inode %u: unknown type %d", inum, d.type);
      continue;
    }
    if (d.size > MAXFILE * BSIZE)
      ERR("inode %u: size %u is bigger than the xv6 limit %u", inum, d.size, (unsigned)(MAXFILE * BSIZE));
    if (d.type == T_DIR && d.size % sizeof(struct dirent) != 0)
      ERR("folder inode %u: size %u is not a multiple of %zu", inum, d.size, sizeof(struct dirent));

    uint32_t need = (d.size + BSIZE - 1) / BSIZE;    // blocks the size needs

    for (uint32_t n = 0; n < NDIRECT; n++) {         // direct blocks
      if (d.addrs[n] == 0) {
        if (n < need) ERR("inode %u: block %u of the file is missing", inum, n);
        continue;
      }
      if (n >= need) ERR("inode %u: has block %u past the end of the file", inum, n);
      claim(inum, d.addrs[n], "direct");
    }

    if (d.addrs[NDIRECT] != 0) {                     // indirect block
      if (need <= NDIRECT)
        ERR("inode %u: has an indirect block but the file is small", inum);
      claim(inum, d.addrs[NDIRECT], "indirect");

      uint32_t ind[NINDIRECT];
      read_block(d.addrs[NDIRECT], ind);
      for (uint32_t n = 0; n < NINDIRECT; n++) {
        uint32_t fileblk = NDIRECT + n;
        if (ind[n] == 0) {
          if (fileblk < need) ERR("inode %u: block %u of the file is missing", inum, fileblk);
          continue;
        }
        if (fileblk >= need) ERR("inode %u: has block %u past the end of the file", inum, fileblk);
        claim(inum, ind[n], "indirect-listed");
      }
    } else if (need > NDIRECT) {
      ERR("inode %u: size needs %u blocks but there is no indirect block", inum, need);
    }
  }
}

// ---- 4. bitmap ----
static void check_bitmap(void) {
  for (uint32_t b = 0; b < data_start; b++)
    if (!bitmap_used(b))
      ERR("bitmap: metadata block %u is marked free", b);

  for (uint32_t b = data_start; b < sb.size; b++) {
    int used = bitmap_used(b);
    if (used && owner[b] == 0)
      ERR("bitmap: block %u is marked used but no inode uses it", b);
    if (!used && owner[b] != 0)
      ERR("bitmap: block %u is used by inode %u but marked free", b, owner[b]);
  }
}

// ---- 5. folder entries ----
static void check_dirs(void) {
  for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
    struct dinode d;
    read_inode(inum, &d);
    if (d.type != T_DIR) continue;

    struct dirent de;
    for (uint32_t off = 0; off < d.size; off += sizeof(de)) {
      read_data(&d, (char *)&de, off, sizeof(de));
      uint32_t idx = off / sizeof(de);

      char name[DIRSIZ + 1];
      memcpy(name, de.name, DIRSIZ);
      name[DIRSIZ] = '\0';

      if (idx == 0 && (de.inum != inum || strcmp(name, ".") != 0))
        ERR("folder inode %u: first entry is not \".\" pointing to itself", inum);
      if (idx == 1 && strcmp(name, "..") != 0)
        ERR("folder inode %u: second entry is not \"..\"", inum);
      if (de.inum == 0) continue;                    // empty slot

      if (de.inum >= sb.ninodes) {
        ERR("folder inode %u: entry \"%s\" has bad inode number %u", inum, name, de.inum);
        continue;
      }
      struct dinode t;
      read_inode(de.inum, &t);
      if (t.type == 0) {
        ERR("folder inode %u: entry \"%s\" points to free inode %u", inum, name, de.inum);
        continue;
      }
      if (name[0] == '\0')
        ERR("folder inode %u: entry with an empty name", inum);

      if (idx == 1 && t.type != T_DIR)
        ERR("folder inode %u: \"..\" points to a non-folder", inum);

      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
      names[de.inum]++;
      if (t.type == T_DIR) subdirs[inum]++;
    }
    if (d.size < 2 * sizeof(struct dirent))
      ERR("folder inode %u: missing \".\" or \"..\"", inum);
  }
}

// ---- 6. link counts ----
static void check_links(void) {
  for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
    struct dinode d;
    read_inode(inum, &d);
    if (d.type == 0) continue;

    if (d.type == T_DIR) {
      int expect = 1 + subdirs[inum];
      if (d.nlink != expect)
        ERR("folder inode %u: nlink is %d, expected %d (1 + %d sub-folders)",
            inum, d.nlink, expect, subdirs[inum]);
      if (inum != ROOTINO && names[inum] != 1)
        ERR("folder inode %u: listed in %d folders, expected exactly 1", inum, names[inum]);
    } else {
      if (d.nlink != names[inum])
        ERR("inode %u: nlink is %d but %d folder entries point to it",
            inum, d.nlink, names[inum]);
    }
  }
}

// ---- 7. reachable from "/" ----
static void walk(uint32_t inum) {
  if (reached[inum]) return;                       // already visited
  reached[inum] = 1;

  struct dinode d;
  read_inode(inum, &d);
  if (d.type != T_DIR) return;

  struct dirent de;
  for (uint32_t off = 2 * sizeof(de); off < d.size; off += sizeof(de)) {
    read_data(&d, (char *)&de, off, sizeof(de));
    if (de.inum != 0 && de.inum < sb.ninodes) walk(de.inum);
  }
}

static void check_reachable(void) {
  struct dinode root;
  read_inode(ROOTINO, &root);
  if (root.type != T_DIR) { ERR("inode 1 (root) is not a folder"); return; }

  walk(ROOTINO);
  for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
    struct dinode d;
    read_inode(inum, &d);
    if (d.type != 0 && !reached[inum])
      ERR("inode %u is in use but cannot be reached from \"/\"", inum);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <fs.img>\n", argv[0]);
    return 2;
  }
  if (disk_open(argv[1]) < 0) return 2;

  printf("fs.img: %u blocks, %u inodes, %u data blocks\n", sb.size, sb.ninodes, sb.nblocks);
  if (!check_superblock()) return 1;

  owner   = calloc(sb.size, sizeof(uint32_t));
  names   = calloc(sb.ninodes, sizeof(int));
  subdirs = calloc(sb.ninodes, sizeof(int));
  reached = calloc(sb.ninodes, 1);
  if (!owner || !names || !subdirs || !reached) { fprintf(stderr, "out of memory\n"); return 2; }

  check_inodes();
  check_bitmap();
  check_dirs();
  check_links();
  check_reachable();

  uint32_t used_blocks = 0, used_inodes = 0;
  for (uint32_t b = data_start; b < sb.size; b++) used_blocks += (owner[b] != 0);
  for (uint32_t i = 1; i < sb.ninodes; i++) {
    struct dinode d;
    read_inode(i, &d);
    used_inodes += (d.type != 0);
  }
  printf("in use: %u inodes, %u data blocks (%u free)\n",
         used_inodes, used_blocks, sb.nblocks - used_blocks);

  if (errors == 0) printf("fs.img is CONSISTENT\n");
  else             printf("%d error(s) found\n", errors);
  return errors == 0 ? 0 : 1;
}