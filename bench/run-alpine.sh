#!/bin/sh
# bench/run-alpine.sh — musl-guest battery (busybox tools): medians-of-N.
# Alpine paths differ from debian: all through busybox applets + python3.
set -eu
ROOTFS=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs}
N=${2:-5}
SP=${SP:-$(cd "$(dirname "$0")/.." && pwd)}
# H6: fail-loud (same stale-debug trap as run.sh).
B=${SPROUT_BIN:-$PREFIX/bin/sprout}
[ -x "$B" ] || { echo "run-alpine.sh: sprout binary not found: $B (set SPROUT_BIN=...)" >&2; exit 1; }
export SPROUT_PRELOAD_PATH=${SPROUT_PRELOAD_PATH:-$SP/target/libsprout-core-musl.so}

median() {
    label=$1; shift
    "$@" >/dev/null 2>&1 || { printf '%-34s FAILED\n' "$label" >&2; return 1; }
    ms=$(i=0; while [ "$i" -lt "$N" ]; do
        t0=$(date +%s%N); "$@" >/dev/null 2>&1; t1=$(date +%s%N)
        echo $(( (t1 - t0) / 1000000 )); i=$((i + 1))
    done | sort -n | awk -v n="$N" 'NR==int(n/2)+1 {print $1}')
    printf '%-34s %s ms\n' "$label" "$ms" >&2
    echo "$ms"
}

run_case() {
    name=$1; shift
    p=$(median "proot-distro  $name" proot-distro login alpine -- "$@")
    s=$(median "sprout        $name" "$B" -r "$ROOTFS" "$@")
    if [ -n "$p" ] && [ -n "$s" ]; then
        speedup=$(echo "$p $s" | awk '{printf "%.2f", $1/$2}')
        printf '=> %s speedup %sx\n' "$name" "$speedup"
    fi
    echo
}

run_ab() {
    name=$1; shift
    s=$(median "sprout(notify)  $name" "$B" -r "$ROOTFS" "$@")
    t=$(median "sprout(ptrace)  $name" env SPROUT_USER_NOTIFY=0 "$B" -r "$ROOTFS" "$@")
    if [ -n "$t" ] && [ -n "$s" ]; then
        rel=$(echo "$s $t" | awk '{printf "%.2f", $2/$1}')
        printf '=> %s notify-vs-ptr-lane %sx (>1 = notify win)\n' "$name" "$rel"
    fi
    echo
}

run_case "sh -c true (busybox ash)" /bin/sh -c true
run_case "exec-chain 20x true" /bin/sh -c 'for i in $(seq 20); do /bin/true; done'
run_case "cmdsubst-pipe 100x" /bin/sh -c 'i=0; while [ $i -lt 100 ]; do x=$(echo a|cut -c1); i=$((i+1)); done'
run_case "python3 -c pass (musl)" /usr/bin/python3 -c pass
run_case "find /usr/bin -type f" /usr/bin/find /usr/bin -type f
run_case "dd write 64MB + sync" /bin/sh -c 'dd if=/dev/zero of=/tmp/b-dd bs=1M count=64 conv=fdatasync 2>/dev/null; rm -f /tmp/b-dd'

echo "== musl notify-vs-ptr-lane (stat-heavy: expected near-parity) =="
run_ab "find /usr -type f" /usr/bin/find /usr -type f
run_ab "sh -c true" /bin/sh -c true
