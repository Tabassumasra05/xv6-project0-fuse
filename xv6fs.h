// xv6fs.h - xv6's on-disk format (copied from xv6-riscv/kernel/fs.h)
// plus the names of our shared helper functions.
#ifndef XV6FS_H
#define XV6FS_H

#include <stdint.h>

#define BSIZE     1024                         // block size in bytes
#define ROOTINO   1                            // inode number of "/"
#define FSMAGIC   0x10203040                   // superblock signature
#define NDIRECT   12                           // direct block slots
#define NINDIRECT (BSIZE / sizeof(uint32_t))   // 256 slots in indirect blk
#define MAXFILE   (NDIRECT + NINDIRECT)        // max blocks in one file
#define DIRSIZ    14                           // max file-name length

// inode types (from xv6-riscv/kernel/stat.h)
#define T_DIR    1
#define T_FILE   2
#define T_DEVICE 3

struct superblock {
  uint32_t magic;       // must be FSMAGIC
  uint32_t size;        // total blocks in fs.img
  uint32_t nblocks;     // number of data blocks
  uint32_t ninodes;     // number of inodes
  uint32_t nlog;        // number of log blocks
  uint32_t logstart;    // first log block
  uint32_t inodestart;  // first inode block
  uint32_t bmapstart;   // first bitmap block
};

struct dinode {
  int16_t  type;                  // 0 = free, T_DIR, T_FILE, T_DEVICE
  int16_t  major;
  int16_t  minor;
  int16_t  nlink;                 // how many folder entries point here
  uint32_t size;                  // file size in bytes
  uint32_t addrs[NDIRECT + 1];    // 12 direct blocks + 1 indirect block
};

struct dirent {
  uint16_t inum;                  // 0 = empty slot
  char     name[DIRSIZ];          // NOT always '\0'-terminated
};

#define IPB (BSIZE / sizeof(struct dinode))   // inodes per block = 16

extern struct superblock sb;      // filled by disk_open()

// ---- disk.c : raw access to fs.img ----
int  disk_open(const char *path);
void read_block(uint32_t bno, void *buf);
void write_block(uint32_t bno, const void *buf);
void read_inode(uint32_t inum, struct dinode *ip);
void write_inode(uint32_t inum, const struct dinode *ip);
uint32_t block_of(const struct dinode *ip, uint32_t n);
int  read_data(const struct dinode *ip, char *dst, uint32_t off, uint32_t n);
uint32_t block_of_alloc(struct dinode *ip, uint32_t n);
int  write_data(struct dinode *ip, const char *src, uint32_t off, uint32_t n);
int  trunc_data(struct dinode *ip, uint32_t new_size);

// ---- alloc.c : bitmap and free inodes ----
uint32_t balloc(void);
void bfree(uint32_t b);
int  ialloc(int16_t type);

// ---- dirops.c : folders and paths ----
int  dir_lookup(uint32_t dir_inum, const char *name);
int  path_lookup(const char *path);
int  parent_lookup(const char *path, char *name);
int  dir_add(uint32_t dir_inum, const char *name, uint32_t inum);
int  dir_remove(uint32_t dir_inum, const char *name);
int  dir_is_empty(uint32_t dir_inum);

#endif