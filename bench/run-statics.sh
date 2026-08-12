#!/bin/sh
# bench/run-statics.sh — 3-lane A/B/C for STATIC guests (ADR-0016):
#   proot-distro  vs  sprout notify-statics  vs  sprout legacy ptrace lane
#
# Usage: bench/run-statics.sh [rootfs] [iterations]
# Rootfs: debian container carrying the freestanding statics under /tmp.
set -eu
ROOTFS=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs}
N=${2:-5}
[ -d "$ROOTFS" ] || { echo "rootfs missing: $ROOTFS" >&2; exit 1; }
SPROUT_BIN=${SPROUT_BIN:-$PREFIX/bin/sprout}
[ -x "$SPROUT_BIN" ] || { echo "sprout missing: $SPROUT_BIN" >&2; exit 1; }
DISTRO=debian; case "$ROOTFS" in *alpine*) DISTRO=alpine ;; esac

median() {
    # statics under test exit 42 = success; any rc counts as long as the
    # run completes (no timeout guard here — workloads are bounded).
    label=$1; shift
    ms=$(i=0; while [ "$i" -lt "$N" ]; do
        s=$(date +%s%N); "$@" >/dev/null 2>&1 || :; e=$(date +%s%N)
        echo $(( (e - s) / 1000000 )); i=$((i + 1))
    done | sort -n | awk -v n="$N" 'NR==int(n/2)+1 {print $1}')
    printf '%-38s %s ms\n' "$label" "$ms" >&2
    echo "$ms"
}

case3() {
    name=$1; shift
    p=$(median "proot                 $name" proot-distro login "$DISTRO" -- "$@")
    n=$(median "sprout(notif-statics) $name" "$SPROUT_BIN" -r "$ROOTFS" "$@")
    t=$(median "sprout(ptrace-lane)   $name" env SPROUT_NOTIFY_STATICS=0 "$SPROUT_BIN" -r "$ROOTFS" "$@")
    if [ -n "$p" ] && [ -n "$n" ]; then
        echo "=> $name  proot-vs-notify  $(echo "$p $n" | awk '{printf "%.2f", $1/$2}')x"
    fi
    if [ -n "$t" ] && [ -n "$n" ]; then
        echo "=> $name  ptrace-vs-notify $(echo "$t $n" | awk '{printf "%.2f", $1/$2}')x"
    fi
    echo
}

echo "### rootfs=$ROOTFS N=$N"
case3 "spawn x50 (fork+exec static)"       /tmp/sp_spawner /tmp/sp_min
case3 "open+read+close x20k"               /tmp/sp_ioloop
case3 "newfstatat x20k"                    /tmp/sp_statloop
case3 "self-exec chain depth 8"            /tmp/sp_execdepth 8
case3 "static->dynamic basename"           /tmp/sp_exec2 /usr/bin/basename /a/b
case3 "static->dynamic python3 -c pass"    /tmp/sp_exec2 /usr/bin/python3 -c pass
