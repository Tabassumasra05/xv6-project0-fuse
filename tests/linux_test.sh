#!/bin/bash
# tests/linux_test.sh - Linux-side tests on xv6's fs.img, prints PASS / FAIL.
# Usage (from the project folder):  ./tests/linux_test.sh
# Make sure xv6 is NOT running before you start.

cd "$(dirname "$0")/.." || exit 1   # always run from the project folder

IMG=xv6-riscv/fs.img
MNT=mnt
TMP=$(mktemp -d)                 # scratch folder for test files on Linux
P=0; F=0                         # pass / fail counters

pass() { echo "  PASS  $1"; P=$((P + 1)); }
fail() { echo "  FAIL  $1"; F=$((F + 1)); }
check() {                        # check "description" command...
  local name=$1; shift
  if "$@" > /dev/null 2>&1; then pass "$name"; else fail "$name"; fi
}
free_blocks() { ./fsck_xv6 "$IMG" | sed -n 's/.*(\([0-9]*\) free).*/\1/p'; }
size_of() { stat -c %s "$1" 2>/dev/null; }

cleanup() {
  mountpoint -q "$MNT" && fusermount3 -u "$MNT"
  rm -rf "$TMP"
}
trap cleanup EXIT

echo "== Building"
make -s || { echo "make failed"; exit 1; }
gcc -Wall -Wextra -o fsck_xv6 fsck_xv6.c disk.c alloc.c || { echo "fsck build failed"; exit 1; }
mkdir -p "$MNT"
mountpoint -q "$MNT" && fusermount3 -u "$MNT"

# Remove leftovers from an earlier run that was stopped halfway
./xv6fs "$IMG" "$MNT" && rm -rf "$MNT/t" && fusermount3 -u "$MNT"

echo "== Checking the disk before the tests"
if ./fsck_xv6 "$IMG" > "$TMP/fsck0" 2>&1; then pass "fsck before tests: CONSISTENT"
else fail "fsck before tests"; cat "$TMP/fsck0"; exit 1; fi
FREE_BEFORE=$(free_blocks)

# Test files made on Linux (random bytes, so any mistake shows up in cmp)
head -c 5000   /dev/urandom > "$TMP/small"    # 5 blocks: direct only
head -c 100000 /dev/urandom > "$TMP/medium"   # 98 blocks: needs the indirect block
head -c 274432 /dev/urandom > "$TMP/max"      # 268 blocks: largest xv6 file
head -c 274433 /dev/urandom > "$TMP/toobig"   # 1 byte over the limit

echo "== Mount + read (getattr, readdir, read)"
check "mount fs.img on $MNT"             ./xv6fs "$IMG" "$MNT"
check "ls shows xv6's README"            test -f "$MNT/README"
if [ -f xv6-riscv/README ]; then
  check "cat README matches xv6 source"  cmp "$MNT/README" xv6-riscv/README
fi
check "console is a device file"         test -c "$MNT/console"
check "missing file gives an error"      bash -c "! ls $MNT/no_such_file"

echo "== Write (create, write)"
mkdir "$MNT/t"
echo "hello xv6" > "$MNT/t/hello.txt"
check "echo > new file, read back"       test "$(cat "$MNT/t/hello.txt")" = "hello xv6"
cp "$TMP/small"  "$MNT/t/small";  check "copy 5 KB file (direct blocks)"      cmp "$TMP/small"  "$MNT/t/small"
cp "$TMP/medium" "$MNT/t/medium"; check "copy 100 KB file (indirect block)"   cmp "$TMP/medium" "$MNT/t/medium"
cp "$TMP/max"    "$MNT/t/max";    check "copy 268 KB file (xv6 max size)"     cmp "$TMP/max"    "$MNT/t/max"
check "269th block refused (File too large)" bash -c "! cp $TMP/toobig $MNT/t/toobig"
rm -f "$MNT/t/toobig"

echo "== Overwrite and append"
echo "new" > "$MNT/t/hello.txt"
check "echo > existing file replaces it"  test "$(cat "$MNT/t/hello.txt")" = "new"
echo "more" >> "$MNT/t/hello.txt"
check "echo >> appends"                   test "$(cat "$MNT/t/hello.txt")" = "$(printf 'new\nmore')"

echo "== Truncate"
truncate -s 13000 "$MNT/t/medium"
check "shrink 100 KB -> 13000 bytes"      test "$(size_of "$MNT/t/medium")" = 13000
check "kept part is unchanged"            cmp -n 13000 "$TMP/medium" "$MNT/t/medium"
truncate -s 20000 "$MNT/t/medium"
check "grow back to 20000 bytes"          test "$(size_of "$MNT/t/medium")" = 20000
check "grown part is zeros"               cmp -i 13000:0 -n 7000 "$MNT/t/medium" /dev/zero
truncate -s 0 "$MNT/t/max"
check "truncate to 0"                     test "$(size_of "$MNT/t/max")" = 0

echo "== Folders (mkdir)"
check "mkdir"                             mkdir "$MNT/t/dir"
check "mkdir inside a folder"             mkdir "$MNT/t/dir/sub"
echo "deep" > "$MNT/t/dir/sub/f"
check "file inside nested folder"         test "$(cat "$MNT/t/dir/sub/f")" = "deep"
check "15-char name refused (xv6 max 14)" bash -c "! touch $MNT/t/abcdefghijklmno"
check "14-char name allowed"              touch "$MNT/t/abcdefghijklmn"

echo "== Unmount + mount again (data really saved in fs.img)"
fusermount3 -u "$MNT"
check "mount again"                       ./xv6fs "$IMG" "$MNT"
check "5 KB file still there"             cmp "$TMP/small" "$MNT/t/small"
check "nested file still there"           test "$(cat "$MNT/t/dir/sub/f")" = "deep"

echo "== Delete (unlink, rmdir)"
check "rm a file"                         rm "$MNT/t/small"
check "deleted file is gone"              test ! -e "$MNT/t/small"
check "rmdir on non-empty folder refused" bash -c "! rmdir $MNT/t/dir"
check "rm -r removes everything"          rm -r "$MNT/t"
check "test folder is gone"               test ! -e "$MNT/t"

echo "== Consistency after everything"
fusermount3 -u "$MNT"
if ./fsck_xv6 "$IMG" > "$TMP/fsck1" 2>&1; then pass "fsck after tests: CONSISTENT"
else fail "fsck after tests"; cat "$TMP/fsck1"; fi
check "all blocks given back ($FREE_BEFORE free)" test "$(free_blocks)" = "$FREE_BEFORE"

echo
echo "RESULT: $P passed, $F failed"
echo "Next: ./tests/xv6_test.sh  (boots xv6 and checks both directions)"
[ "$F" -eq 0 ]