#!/bin/bash
# tests/xv6_test.sh - Linux <-> xv6 tests with the real xv6 booted in QEMU.
#   1. Linux -> xv6: write files with our FUSE program, boot xv6, cat / wc them
#   2. xv6 -> Linux: create files inside xv6, mount, read them on Linux
#   3. fsck after each step
# Usage (from the project folder):  ./tests/xv6_test.sh
# Make sure xv6 is NOT already running.

cd "$(dirname "$0")/.." || exit 1   # always run from the project folder

IMG=xv6-riscv/fs.img
MNT=mnt
OUT=$(mktemp)                       # xv6's screen output is saved here
P=0; F=0

pass() { echo "  PASS  $1"; P=$((P + 1)); }
fail() { echo "  FAIL  $1"; F=$((F + 1)); }
fsck_ok() {
  if ./fsck_xv6 "$IMG" > "$OUT.fsck" 2>&1; then pass "$1"
  else fail "$1"; cat "$OUT.fsck"; fi
}

cleanup() {
  mountpoint -q "$MNT" && fusermount3 -u "$MNT"
  rm -f "$OUT" "$OUT.fsck"
}
trap cleanup EXIT

# Boot xv6, type each argument as a command, then press Ctrl-A X to quit.
# Everything xv6 prints is saved in $OUT.
run_xv6() {
  {
    sleep 10                        # wait for xv6 to boot
    for cmd in "$@"; do
      echo "$cmd"
      sleep 2                       # give each command time to finish
    done
    sleep 3
    printf '\001x'                  # Ctrl-A X = quit QEMU
  } | (cd xv6-riscv && timeout 120 make qemu) > "$OUT" 2>&1
  pkill -f "qemu-system-riscv64.*fs.img" 2>/dev/null   # in case QEMU did not quit
}

echo "== Building"
make -s || { echo "make failed"; exit 1; }
gcc -Wall -Wextra -o fsck_xv6 fsck_xv6.c disk.c alloc.c || { echo "fsck build failed"; exit 1; }
mkdir -p "$MNT"
mountpoint -q "$MNT" && fusermount3 -u "$MNT"

# Safety: if "make qemu" would rebuild fs.img, our files would be wiped.
if (cd xv6-riscv && make -n qemu 2>/dev/null | grep -q "mkfs/mkfs"); then
  echo "STOP: 'make qemu' would rebuild fs.img and erase it. Run 'make qemu' in"
  echo "xv6-riscv once by hand, quit it, then run this test again."
  exit 1
fi

fsck_ok "fsck before tests: CONSISTENT"

# Remove leftovers from an earlier run
./xv6fs "$IMG" "$MNT" && rm -rf "$MNT/l2x.txt" "$MNT/l2xbig" "$MNT/x2l.txt" "$MNT/x2ldir"
fusermount3 -u "$MNT"

echo "== Linux -> xv6"
BIG=xv6-riscv/kernel/fs.c                       # about 17 KB: uses the indirect block
./xv6fs "$IMG" "$MNT"
echo "hello from linux" > "$MNT/l2x.txt"
cp "$BIG" "$MNT/l2xbig"
fusermount3 -u "$MNT"
fsck_ok "fsck after writing from Linux: CONSISTENT"

# Expected wc numbers (lines words bytes), computed on Linux
WC=$(wc < "$BIG" | awk '{print $1" "$2" "$3}')

echo "   booting xv6 (about 30 seconds)..."
run_xv6 "cat l2x.txt" \
        "wc l2xbig" \
        "echo made in xv6 > x2l.txt" \
        "mkdir x2ldir" \
        "echo nested in xv6 > x2ldir/n"

if grep -q "init: starting sh" "$OUT"; then pass "xv6 booted"
else fail "xv6 booted"; echo "---- xv6 output ----"; cat "$OUT"; exit 1; fi

if grep -q "^hello from linux" "$OUT"; then pass "xv6: cat l2x.txt shows the Linux text"
else fail "xv6: cat l2x.txt shows the Linux text"; fi

if grep -q "$WC l2xbig" "$OUT"; then pass "xv6: wc of 17 KB file matches Linux ($WC)"
else fail "xv6: wc of 17 KB file matches Linux ($WC)"; fi

echo "== xv6 -> Linux"
fsck_ok "fsck after xv6 wrote files: CONSISTENT"
./xv6fs "$IMG" "$MNT"
if [ "$(cat "$MNT/x2l.txt" 2>/dev/null)" = "made in xv6" ]; then pass "Linux: cat x2l.txt shows the xv6 text"
else fail "Linux: cat x2l.txt shows the xv6 text"; fi
if [ -d "$MNT/x2ldir" ]; then pass "Linux: folder made in xv6 is there"
else fail "Linux: folder made in xv6 is there"; fi
if [ "$(cat "$MNT/x2ldir/n" 2>/dev/null)" = "nested in xv6" ]; then pass "Linux: file inside xv6 folder reads correctly"
else fail "Linux: file inside xv6 folder reads correctly"; fi

echo "== Cleaning up"
rm -rf "$MNT/l2x.txt" "$MNT/l2xbig" "$MNT/x2l.txt" "$MNT/x2ldir"
fusermount3 -u "$MNT"
fsck_ok "fsck after cleanup: CONSISTENT"

echo
echo "RESULT: $P passed, $F failed"
if [ "$F" -ne 0 ]; then echo "---- xv6 output ----"; cat "$OUT"; fi
[ "$F" -eq 0 ]