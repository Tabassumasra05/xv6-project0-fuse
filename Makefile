# Makefile - type "make" to build, "make clean" to delete built files.
# (Recipe lines start with ">" instead of a Tab, so copy-paste can't break it.)
.RECIPEPREFIX = >

CC     = gcc
CFLAGS = -Wall -Wextra -g $(shell pkg-config --cflags fuse3)
LIBS   = $(shell pkg-config --libs fuse3)

all: xv6fs

xv6fs: xv6fs.c disk.c dirops.c alloc.c xv6fs.h
> $(CC) $(CFLAGS) -o xv6fs xv6fs.c disk.c dirops.c alloc.c $(LIBS)

clean:
> rm -f xv6fs