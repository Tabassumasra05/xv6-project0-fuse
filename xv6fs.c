// xv6fs.c - the FUSE program. Linux calls these functions when someone
// uses ls, cat, cp ... on the mounted folder.
// Run:  ./xv6fs <fs.img> <mount-folder>
// Stop: fusermount3 -u <mount-folder>
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "xv6fs.h"

// "Does this path exist? Is it a file or folder? How big?"  (ls -l, stat, almost everything)
static int x_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
  (void)fi;
  int inum = path_lookup(path);
  if (inum < 0) return -ENOENT;                 // "No such file or directory"

  struct dinode d;
  read_inode(inum, &d);

  memset(st, 0, sizeof(*st));
  st->st_ino   = inum;
  st->st_nlink = d.nlink;
  st->st_size  = d.size;
  st->st_uid   = getuid();                      // show files as yours
  st->st_gid   = getgid();
  st->st_blocks = (d.size + 511) / 512;

  if (d.type == T_DIR)       st->st_mode = S_IFDIR | 0755;
  else if (d.type == T_FILE) st->st_mode = S_IFREG | 0644;
  else                       st->st_mode = S_IFCHR | 0666;   // T_DEVICE (console)
  return 0;
}

// "What's inside this folder?"  (ls)
static int x_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi,
                     enum fuse_readdir_flags flags) {
  (void)offset; (void)fi; (void)flags;
  int inum = path_lookup(path);
  if (inum < 0) return -ENOENT;

  struct dinode dir;
  read_inode(inum, &dir);
  if (dir.type != T_DIR) return -ENOTDIR;

  struct dirent de;
  for (uint32_t off = 0; off < dir.size; off += sizeof(de)) {
    read_data(&dir, (char *)&de, off, sizeof(de));
    if (de.inum == 0) continue;                 // empty slot

    char name[DIRSIZ + 1];
    memcpy(name, de.name, DIRSIZ);
    name[DIRSIZ] = '\0';                        // make sure it ends
    filler(buf, name, NULL, 0, 0);              // hand one name to Linux
  }
  return 0;
}

// "I want to open this file."  Check it exists. If opened with O_TRUNC
// (for example  echo hi > file), empty the file first.
static int x_open(const char *path, struct fuse_file_info *fi) {
  int inum = path_lookup(path);
  if (inum < 0) return -ENOENT;

  if (fi->flags & O_TRUNC) {
    struct dinode d;
    read_inode(inum, &d);
    if (d.type == T_FILE) {
      trunc_data(&d, 0);
      write_inode(inum, &d);
    }
  }
  return 0;
}

// "Give me `size` bytes of this file, starting at `offset`."  (cat)
static int x_read(const char *path, char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi) {
  (void)fi;
  int inum = path_lookup(path);
  if (inum < 0) return -ENOENT;

  struct dinode d;
  read_inode(inum, &d);
  if (d.type == T_DIR) return -EISDIR;

  return read_data(&d, buf, offset, size);     // number of bytes copied
}

// "Write `size` bytes into this file at `offset`."  (cp, echo > file)
static int x_write(const char *path, const char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
  (void)fi;
  int inum = path_lookup(path);
  if (inum < 0) return -ENOENT;

  struct dinode d;
  read_inode(inum, &d);
  if (d.type == T_DIR) return -EISDIR;

  int r = write_data(&d, buf, offset, size);
  write_inode(inum, &d);                        // save new size / blocks
  return r;                                     // bytes written, or -error
}

// "Make a new empty file."  (cp, touch, echo > newfile)
static int x_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
  (void)mode; (void)fi;
  char name[DIRSIZ + 1];
  int pinum = parent_lookup(path, name);
  if (pinum < 0) return pinum;                  // -ENOENT / -ENAMETOOLONG
  if (dir_lookup(pinum, name) >= 0) return -EEXIST;

  int inum = ialloc(T_FILE);                    // new inode
  if (inum < 0) return -ENOSPC;

  struct dinode d;
  read_inode(inum, &d);
  d.nlink = 1;                                  // one folder entry points here
  write_inode(inum, &d);

  int r = dir_add(pinum, name, inum);           // put the name in the folder
  if (r < 0) {                                  // undo if the folder is full
    d.type = 0; d.nlink = 0;
    write_inode(inum, &d);
    return r;
  }
  return 0;
}

// "Make a new folder."  (mkdir)  Same steps as xv6's create() for T_DIR.
static int x_mkdir(const char *path, mode_t mode) {
  (void)mode;
  char name[DIRSIZ + 1];
  int pinum = parent_lookup(path, name);
  if (pinum < 0) return pinum;
  if (dir_lookup(pinum, name) >= 0) return -EEXIST;

  int inum = ialloc(T_DIR);
  if (inum < 0) return -ENOSPC;

  struct dinode d;
  read_inode(inum, &d);
  d.nlink = 1;
  write_inode(inum, &d);

  if (dir_add(inum, ".", inum) < 0 ||           // "."  = itself
      dir_add(inum, "..", pinum) < 0 ||         // ".." = parent
      dir_add(pinum, name, inum) < 0)           // name in the parent
    return -ENOSPC;

  struct dinode p;                              // the new ".." points to the
  read_inode(pinum, &p);                        // parent, so the parent gets
  p.nlink++;                                    // one more link (like xv6)
  write_inode(pinum, &p);
  return 0;
}

// "Delete this file."  (rm)
static int x_unlink(const char *path) {
  char name[DIRSIZ + 1];
  int pinum = parent_lookup(path, name);
  if (pinum < 0) return pinum;

  int inum = dir_lookup(pinum, name);
  if (inum < 0) return -ENOENT;

  struct dinode d;
  read_inode(inum, &d);
  if (d.type == T_DIR) return -EISDIR;          // folders use rmdir

  dir_remove(pinum, name);                      // 1. remove the name
  d.nlink--;                                    // 2. one less link
  if (d.nlink <= 0) {                           // 3. no names left:
    trunc_data(&d, 0);                          //    free all its blocks
    memset(&d, 0, sizeof(d));                   //    and free the inode
  }
  write_inode(inum, &d);
  return 0;
}

// "Delete this empty folder."  (rmdir) - not required, but rm -r needs it.
static int x_rmdir(const char *path) {
  char name[DIRSIZ + 1];
  int pinum = parent_lookup(path, name);
  if (pinum < 0) return pinum;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EINVAL;

  int inum = dir_lookup(pinum, name);
  if (inum < 0) return -ENOENT;

  struct dinode d;
  read_inode(inum, &d);
  if (d.type != T_DIR) return -ENOTDIR;
  if (!dir_is_empty(inum)) return -ENOTEMPTY;

  dir_remove(pinum, name);
  trunc_data(&d, 0);                            // free the folder's blocks
  memset(&d, 0, sizeof(d));                     // free its inode
  write_inode(inum, &d);

  struct dinode p;                              // its ".." is gone, so the
  read_inode(pinum, &p);                        // parent loses one link
  p.nlink--;
  write_inode(pinum, &p);
  return 0;
}

// "Make this file exactly `size` bytes."  (truncate, and echo > existing file)
static int x_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
  (void)fi;
  int inum = path_lookup(path);
  if (inum < 0) return -ENOENT;

  struct dinode d;
  read_inode(inum, &d);
  if (d.type == T_DIR) return -EISDIR;

  int r = trunc_data(&d, size);
  write_inode(inum, &d);
  return r;
}

// xv6 does not store times, so just say "OK". Without this, touch fails.
static int x_utimens(const char *path, const struct timespec tv[2],
                     struct fuse_file_info *fi) {
  (void)tv; (void)fi;
  return path_lookup(path) < 0 ? -ENOENT : 0;
}

// The list we give FUSE: "for this question, call this function".
static const struct fuse_operations ops = {
  .getattr  = x_getattr,
  .readdir  = x_readdir,
  .open     = x_open,
  .read     = x_read,
  .write    = x_write,
  .create   = x_create,
  .mkdir    = x_mkdir,
  .unlink   = x_unlink,
  .rmdir    = x_rmdir,
  .truncate = x_truncate,
  .utimens  = x_utimens,
};

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <fs.img> <mount-folder> [-f]\n", argv[0]);
    return 1;
  }
  if (disk_open(argv[1]) < 0) return 1;

  // Build FUSE's argument list: program name, then everything after fs.img,
  // plus "-s" (one request at a time, so two writes never clash).
  struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
  fuse_opt_add_arg(&args, argv[0]);
  for (int i = 2; i < argc; i++)
    fuse_opt_add_arg(&args, argv[i]);
  fuse_opt_add_arg(&args, "-s");

  return fuse_main(args.argc, args.argv, &ops, NULL);
}