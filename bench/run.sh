#!/bin/sh
# bench/run.sh — sprout vs proot-distro on the same guest, median-of-N.
#
# Usage: bench/run.sh [rootfs] [iterations]
# Rootfs defaults to the proot-distro v5 debian container; sprout must be
# built (target/debug/sprout) and target/libsprout-core.so must exist.
set -eu
ROOTFS=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs}
N=${2:-5}
SP=${SP:-$(cd "$(dirname "$0")/.." && pwd)}
B=${SPROUT_BIN:-$SP/target/debug/sprout}
export SPROUT_PRELOAD_PATH=${SPROUT_PRELOAD_PATH:-$SP/target/libsprout-core.so}

[ -x "$B" ] || { echo "sprout not built at $B" >&2; exit 1; }
[ -d "$ROOTFS" ] || { echo "rootfs missing at $ROOTFS" >&2; exit 1; }

median() {
    # contract: LABELING on stderr (visible to the operator), the MEDIAN
    # NUMBER ONLY on stdout (consumed via $(median ...)); this keeps
    # run_case's awk speedup division well-defined.
    label=$1; shift
    "$@" >/dev/null 2>&1 || { printf '%-30s FAILED\n' "$label" >&2; return 1; }
    ms=$(i=0; while [ "$i" -lt "$N" ]; do
        t0=$(date +%s%N); "$@" >/dev/null 2>&1; t1=$(date +%s%N)
        echo $(( (t1 - t0) / 1000000 )); i=$((i + 1))
    done | sort -n | awk -v n="$N" 'NR==int(n/2)+1 {print $1}')
    printf '%-30s %s ms\n' "$label" "$ms" >&2
    echo "$ms"
}

run_case() {
    name=$1; shift
    p=$(median "proot-distro  $name" proot-distro login debian -- "$@")
    s=$(median "sprout        $name" "$B" -r "$ROOTFS" "$@")
    if [ -n "$p" ] && [ -n "$s" ]; then
        speedup=$(echo "$p $s" | awk '{printf "%.2f", $1/$2}')
        printf '=> %s speedup %sx\n' "$name" "$speedup"
    fi
    echo
}

run_case "python3 -c pass" /usr/bin/python3 -c pass
run_case "bash -c true" /bin/bash -c true
run_case "exec-chain (20x /bin/true)" /bin/bash -c 'for i in $(seq 20); do /bin/true; done'
run_case "find /etc -maxdepth 2 -type f" /usr/bin/find /etc -maxdepth 2 -type f
