#!/bin/sh
# bench/run-hyperfine.sh — hyperfine matrix: sprout vs proot-distro,
# same rootfs images, host-side wall-clock (launcher overhead included).
# Crosscheck of the median-of-N tables from bench/run.sh et al.; raw
# exports land in bench/results/hyperfine-TS/ (gitignored). Consistent
# host described in docs/src/benchmarks.md.
#
# Usage: bench/run-hyperfine.sh [debian-rootfs] [alpine-rootfs]
set -eu
B=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs}
A=${2:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs}
TS=$(date +%Y%m%d-%H%M%S)
OUT=$(cd "$(dirname "$0")/.." && pwd)/bench/results/hyperfine-$TS
mkdir -p "$OUT"
SPROUT_BIN=${SPROUT_BIN:-$PREFIX/bin/sprout}
[ -x "$SPROUT_BIN" ] || { echo "sprout missing: $SPROUT_BIN" >&2; exit 1; }
command -v hyperfine >/dev/null || { echo "pkg install hyperfine" >&2; exit 1; }

cell() { # name runs warm cmdA cmdB — A=sprout B=proot unless internal A/B
    n=$1; r=$2; w=$3; ca=$4; cb=$5
    echo "== $n ==" >&2
    hyperfine --warmup "$w" -r "$r" --ignore-failure \
        -n sprout "$ca" \
        -n proot  "$cb" \
        --export-markdown "$OUT/$n.md" --export-json "$OUT/$n.json" 2>&1 \
        | grep -E "  sprout|  proot|Relative" >&2
}

cell shell-true    20 3 \
    "$SPROUT_BIN -r $B /bin/bash -c true" \
    "proot-distro login debian -- /bin/bash -c true"

cell notify-off    20 3 \
    "env SPROUT_USER_NOTIFY=0 $SPROUT_BIN -r $B /bin/bash -c true" \
    "$SPROUT_BIN -r $B /bin/bash -c true"

cell python-start  12 2 \
    "$SPROUT_BIN -r $B /usr/bin/python3 -c pass" \
    "proot-distro login debian -- /usr/bin/python3 -c pass"

cell exec-chain    10 2 \
    "$SPROUT_BIN -r $B /bin/bash -c 'for i in \$(seq 20); do /bin/true; done'" \
    "proot-distro login debian -- /bin/bash -c 'for i in \$(seq 20); do /bin/true; done'"

cell find-walk     5 1 \
    "$SPROUT_BIN -r $B /usr/bin/find /usr -type f" \
    "proot-distro login debian -- /usr/bin/find /usr -type f"

cell dd-io         5 1 \
    "$SPROUT_BIN -r $B /bin/dd if=/dev/zero of=/dev/null bs=1M count=256" \
    "proot-distro login debian -- /bin/dd if=/dev/zero of=/dev/null bs=1M count=256"

cell static-ioloop 8 1 \
    "$SPROUT_BIN -r $B /tmp/sp_ioloop" \
    "proot-distro login debian -- /tmp/sp_ioloop"

cell static-spawn  10 1 \
    "$SPROUT_BIN -r $B /tmp/sp_spawner /tmp/sp_min" \
    "proot-distro login debian -- /tmp/sp_spawner /tmp/sp_min"

cell musl-shell    15 3 \
    "$SPROUT_BIN -r $A /bin/sh -c true" \
    "proot-distro login alpine -- /bin/sh -c true"

echo "results: $OUT"
