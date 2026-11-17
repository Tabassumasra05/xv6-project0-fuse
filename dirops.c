// dirops.c - folders: find a name inside a folder, turn a path into an inode
#include <string.h>
#include <errno.h>
#include "xv6fs.h"

// Look for `name` inside folder dir_inum.
// Returns its inode number, or -1 if not found.
int dir_lookup(uint32_t dir_inum, const char *name) {
  struct dinode dir;
  read_inode(dir_inum, &dir);
  if (dir.type != T_DIR) return -1;

  struct dirent de;
  for (uint32_t off = 0; off < dir.size; off += sizeof(de)) {
    read_data(&dir, (char *)&de, off, sizeof(de));
    if (de.inum == 0) continue;                      // empty slot
    if (strncmp(de.name, name, DIRSIZ) == 0)         // names are max 14 chars
      return de.inum;
  }
  return -1;
}

// Turn a path like "/docs/note.txt" into an inode number.
// Walks one name at a time starting from the root folder.
// Returns -1 if any part is missing.
int path_lookup(const char *path) {
  int inum = ROOTINO;
  char name[DIRSIZ + 1];

  while (*path) {
    while (*path == '/') path++;                     // skip slashes
    if (*path == '\0') break;

    int len = 0;                                     // copy one name
    while (*path && *path != '/') {
      if (len < DIRSIZ) name[len] = *path;
      len++; path++;
    }
    if (len > DIRSIZ) return -1;                     // too long for xv6
    name[len] = '\0';

    inum = dir_lookup(inum, name);
    if (inum < 0) return -1;
  }
  return inum;
}

// Split "/docs/a.txt" into its parent folder (inode of "/docs") and
// the last name ("a.txt"). Returns the parent's inode number,
// -ENOENT if the parent is missing, -ENAMETOOLONG if the name is > 14 chars.
int parent_lookup(const char *path, char *name) {
  const char *slash = strrchr(path, '/');        // last '/' in the path
  const char *last  = slash ? slash + 1 : path;

  size_t len = strlen(last);
  if (len == 0) return -ENOENT;
  if (len > DIRSIZ) return -ENAMETOOLONG;
  memcpy(name, last, len);
  name[len] = '\0';

  // parent path = everything before the last '/'
  char parent[4096];
  size_t plen = slash ? (size_t)(slash - path) : 0;
  if (plen >= sizeof(parent)) return -ENAMETOOLONG;
  memcpy(parent, path, plen);
  parent[plen] = '\0';

  int pinum = path_lookup(plen == 0 ? "/" : parent);
  if (pinum < 0) return -ENOENT;

  struct dinode pd;
  read_inode(pinum, &pd);
  if (pd.type != T_DIR) return -ENOTDIR;
  return pinum;
}

// Add the entry (inum, name) to folder dir_inum.
// Reuses an empty slot if there is one, otherwise adds at the end.
int dir_add(uint32_t dir_inum, const char *name, uint32_t inum) {
  struct dinode dir;
  read_inode(dir_inum, &dir);

  struct dirent de;
  uint32_t off;
  for (off = 0; off < dir.size; off += sizeof(de)) {
    read_data(&dir, (char *)&de, off, sizeof(de));
    if (de.inum == 0) break;                     // found an empty slot
  }

  memset(&de, 0, sizeof(de));
  de.inum = inum;
  strncpy(de.name, name, DIRSIZ);                // 14 chars, no '\0' needed

  int r = write_data(&dir, (const char *)&de, off, sizeof(de));
  if (r < 0) return r;
  write_inode(dir_inum, &dir);                   // folder may have grown
  return 0;
}

// Remove the entry called `name` from folder dir_inum (sets its inum to 0).
int dir_remove(uint32_t dir_inum, const char *name) {
  struct dinode dir;
  read_inode(dir_inum, &dir);

  struct dirent de;
  for (uint32_t off = 0; off < dir.size; off += sizeof(de)) {
    read_data(&dir, (char *)&de, off, sizeof(de));
    if (de.inum != 0 && strncmp(de.name, name, DIRSIZ) == 0) {
      memset(&de, 0, sizeof(de));                // empty slot
      write_data(&dir, (const char *)&de, off, sizeof(de));
      return 0;
    }
  }
  return -ENOENT;
}

// Is this folder empty (only "." and "..")?  1 = yes, 0 = no.
int dir_is_empty(uint32_t dir_inum) {
  struct dinode dir;
  read_inode(dir_inum, &dir);

  struct dirent de;
  for (uint32_t off = 2 * sizeof(de); off < dir.size; off += sizeof(de)) {
    read_data(&dir, (char *)&de, off, sizeof(de));
    if (de.inum != 0) return 0;
  }
  return 1;
}