#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Reads qemu-storage-daemon exports with Nbd::DiskReader, through abr-disk-nbd-read, and checks the result against
# what was written and against nbdinfo --map: a full read, a dirty bitmap, a missing bitmap, an NBD URI, a rate limit
# and a discard.
#
#   nbd-selftest.sh HELPER SCRATCH_DIR
#
set -euo pipefail

helper=$1
dir=$2

rm -rf "$dir"
mkdir -p "$dir"
cd "$dir"

failures=0
fail() { echo "FAILED: $*"; failures=$((failures + 1)); }

cleanup() {
    for pidfile in disk.pid copy.pid; do
        [ -s "$pidfile" ] && kill "$(cat "$pidfile")" 2>/dev/null || true
    done
}
trap cleanup EXIT

# Starts a daemon exporting IMAGE as "disk" on SOCKET, with the bitmap b0. QEMU ignores discards unless asked to unmap.
start() {
    qemu-storage-daemon --daemonize --pidfile "$3" \
        --blockdev driver=file,node-name=file,filename="$1",discard=unmap \
        --blockdev driver=qcow2,node-name=disk,file=file,discard=unmap \
        --nbd-server addr.type=unix,addr.path="$2" \
        --export type=nbd,id=export,node-name=disk,name=disk,writable=on,bitmaps.0=b0
}

# The ranges nbdinfo reports for a context, merged where they touch, in the helper's "range OFFSET LENGTH" form.
# KEEP is an awk condition on the type column.
expected() {
    nbdinfo --map="$2" "nbd+unix:///disk?socket=$PWD/$1" | awk -v keep="$3" '
        { offset = $1; length_ = $2; type = $3 }
        (keep == "dirty" && type % 2 == 1) || (keep == "data" && int(type / 2) % 2 == 0) {
            if (have && end == offset) { end += length_; next }
            if (have) print "range", start, end - start
            start = offset; end = offset + length_; have = 1
        }
        END { if (have) print "range", start, end - start }'
}

ranges() { grep '^range ' "$1" || true; }

# A 64 MiB qcow2: data written before the bitmap, the bitmap, then the changes it records. A copy for the discard test.
qemu-img create -q -f qcow2 disk.qcow2 64M
qemu-io -f qcow2 -c 'write -q -P 0x11 0 1M' -c 'write -q -P 0x22 8M 64k' disk.qcow2
qemu-img bitmap --add --granularity 64k disk.qcow2 b0
qemu-io -f qcow2 -c 'write -q -P 0x33 16M 128k' -c 'write -q -P 0x44 0 4k' disk.qcow2
cp disk.qcow2 copy.qcow2

start disk.qcow2 disk.sock disk.pid
uri="nbd+unix:///disk?socket=$PWD/disk.sock"

# A full read: the allocated data, which reads back the same as the export.
if "$helper" "$PWD/disk.sock" disk - full.raw > full.out; then
    grep -qx 'context base:allocation' full.out || fail "full read context: $(head -1 full.out)"
    grep -qx 'geometry 131072 512 [0-9]*' full.out || fail "full read geometry: $(grep geometry full.out)"
    [ "$(ranges full.out)" = "$(expected disk.sock base:allocation data)" ] || fail "full read ranges: $(ranges full.out)"
    qemu-img compare -q -f raw -F raw full.raw "$uri" || fail "full read data differs from the export"
else
    fail "full read"
fi

# The dirty bitmap: one 64 KiB block at 0 and 128 KiB at 16 MiB, with what the export holds there.
if "$helper" "$PWD/disk.sock" disk b0 inc.raw > inc.out; then
    grep -qx 'context qemu:dirty-bitmap:b0' inc.out || fail "bitmap context: $(head -1 inc.out)"
    [ "$(ranges inc.out)" = "$(printf 'range 0 65536\nrange 16777216 131072')" ] || fail "bitmap ranges: $(ranges inc.out)"
    [ "$(ranges inc.out)" = "$(expected disk.sock qemu:dirty-bitmap:b0 dirty)" ] || fail "bitmap ranges differ from nbdinfo"
    check=$(qemu-io -f raw -r -c 'read -q -P 0x44 0 4k' -c 'read -q -P 0x11 4k 60k' -c 'read -q -P 0x33 16M 128k' \
        -c 'read -q -P 0 64k 1M' inc.raw 2>&1)
    [ -z "$check" ] || fail "bitmap data: $check"
else
    fail "bitmap read"
fi

# A bitmap the export does not have is an error, not a full read.
if "$helper" "$PWD/disk.sock" disk nosuch none.raw > none.out 2> none.err; then
    fail "a missing bitmap was read"
elif ! grep -q "offers no dirty bitmap 'nosuch'" none.err; then
    fail "missing bitmap message: $(cat none.err)"
fi

# The same export named by an NBD URI.
if "$helper" "$uri" - - uri.raw > uri.out; then
    [ "$(ranges uri.out)" = "$(ranges full.out)" ] || fail "URI read ranges: $(ranges uri.out)"
else
    fail "URI read"
fi

# A rate limit of 1 MiB/s over the 1.19 MiB of data takes at least a second.
started=$(date +%s.%N)
"$helper" "$PWD/disk.sock" disk - rate.raw --rate 1048576 > rate.out || fail "rate limited read"
elapsed=$(awk -v a="$started" -v b="$(date +%s.%N)" 'BEGIN { printf "%.2f", b - a }')
awk -v e="$elapsed" 'BEGIN { exit !(e >= 1.0) }' || fail "rate limited read took only $elapsed s"

# A discard on the copy: the data read is gone from the export afterwards, so a second full read finds none.
start copy.qcow2 copy.sock copy.pid
if "$helper" "$PWD/copy.sock" disk - discard.raw --discard > discard.out; then
    total=$(ranges discard.out | awk '{ sum += $3 } END { print sum + 0 }')
    grep -qx "discarded $total" discard.out || fail "discarded $(grep discarded discard.out), expected $total"
    cmp -s discard.raw full.raw || fail "a discarding read returned other data"
    "$helper" "$PWD/copy.sock" disk - after.raw > after.out || fail "read after discard"
    [ -z "$(ranges after.out)" ] || fail "data left after discard: $(ranges after.out)"
else
    fail "discarding read"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
cd /
rm -rf "$dir"
echo "All NBD checks passed"
