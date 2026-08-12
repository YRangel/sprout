#!/bin/sh
# bench/run.sh — sprout vs proot-distro on the same guest, median-of-N.
#
# Usage: MODE=quick|full bench/run.sh [rootfs] [iterations]
# MODE=quick (default): the 4 original guard cases.
# MODE=full:  extended battery — spawn churn, pipes, traversal, I/O,
#             compute, local git, and a ptrace-only A/B lane.
# Rootfs defaults to the proot-distro v5 debian container.
set -eu
ROOTFS=${1:-/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs}
N=${2:-5}
MODE=${MODE:-quick}
SP=${SP:-$(cd "$(dirname "$0")/.." && pwd)}
B=${SPROUT_BIN:-$SP/target/debug/sprout}
export SPROUT_PRELOAD_PATH=${SPROUT_PRELOAD_PATH:-$SP/target/libsprout-core.so}
DISTRO=debian
case "$ROOTFS" in *alpine*) DISTRO=alpine ;; esac

[ -x "$B" ] || { echo "sprout not built at $B" >&2; exit 1; }
[ -d "$ROOTFS" ] || { echo "rootfs missing at $ROOTFS" >&2; exit 1; }

median() {
    # contract: LABELING on stderr (visible to the operator), the MEDIAN
    # NUMBER ONLY on stdout (consumed via $(median ...)); this keeps
    # run_case's awk speedup division well-defined.
    label=$1; shift
    "$@" >/dev/null 2>&1 || { printf '%-34s FAILED\n' "$label" >&2; return 1; }
    ms=$(i=0; while [ "$i" -lt "$N" ]; do
        t0=$(date +%s%N); "$@" >/dev/null 2>&1; t1=$(date +%s%N)
        echo $(( (t1 - t0) / 1000000 )); i=$((i + 1))
    done | sort -n | awk -v n="$N" 'NR==int(n/2)+1 {print $1}')
    printf '%-34s %s ms\n' "$label" "$ms" >&2
    echo "$ms"
}

proot_run() { proot-distro login "$DISTRO" -- "$@"; }
sprout_run() { "$B" -r "$ROOTFS" "$@"; }
sprout_pt() { SPROUT_USER_NOTIFY=0 "$B" -r "$ROOTFS" "$@"; }

run_case() {
    name=$1; shift
    p=$(median "proot-distro  $name" proot_run "$@")
    s=$(median "sprout        $name" sprout_run "$@")
    if [ -n "$p" ] && [ -n "$s" ]; then
        speedup=$(echo "$p $s" | awk '{printf "%.2f", $1/$2}')
        printf '=> %s speedup %sx\n' "$name" "$speedup"
    fi
    echo
}

run_case_ab() {
    # 3-way: proot vs sprout(default) vs sprout (notify off / ptrace lane)
    name=$1; shift
    p=$(median "proot-distro      $name" proot_run "$@")
    s=$(median "sprout(notify)    $name" sprout_run "$@")
    t=$(median "sprout(ptrace)    $name" sprout_pt "$@")
    if [ -n "$p" ] && [ -n "$s" ]; then
        speedup=$(echo "$p $s" | awk '{printf "%.2f", $1/$2}')
        printf '=> %s proot/sprout-normal %sx\n' "$name" "$speedup"
    fi
    if [ -n "$t" ] && [ -n "$s" ]; then
        rel=$(echo "$s $t" | awk '{printf "%.2f", $2/$1}')
        printf '=> %s notify-vs-ptr-lane %sx\n' "$name" "$rel"
    fi
    echo
}

# --- original guard cases (always) ---
run_case "python3 -c pass" /usr/bin/python3 -c pass
run_case "bash -c true" /bin/bash -c true
run_case "exec-chain (20x /bin/true)" /bin/bash -c 'for i in $(seq 20); do /bin/true; done'
run_case "find /etc -maxdepth 2 -type f" /usr/bin/find /etc -maxdepth 2 -type f

if [ "$MODE" = full ]; then
    echo "== spawn/pipeline churn =="
    run_case "cmdsubst-pipe 100x" /bin/bash -c 'i=0; while [ $i -lt 100 ]; do x=$(echo a|cut -c1); i=$((i+1)); done'
    run_case "grep-churn 200x" /bin/bash -c 'i=0; while [ $i -lt 200 ]; do grep -q z </dev/null; i=$((i+1)); done'
    run_case "sh-loop true 500x" /bin/sh -c 'i=0; while [ $i -lt 500 ]; do true; i=$((i+1)); done'

    echo "== traversal =="
    run_case "find /usr -type f (debian)" /usr/bin/find /usr -type f
    run_case "ls -R /usr/lib" /bin/ls -R /usr/lib
    run_case "python os.walk /usr/lib" /usr/bin/python3 -c 'import os; [1 for _ in os.walk("/usr/lib")]'

    echo "== I/O =="
    run_case "dd write 100MB then sync" /bin/bash -c 'dd if=/dev/zero of=/tmp/bench-dd bs=1M count=100 conv=fdatasync 2>/dev/null; rm -f /tmp/bench-dd'
    run_case "dd read 100MB (warm cache)" /bin/bash -c 'dd if=/dev/zero of=/tmp/bench-dd bs=1M count=100 2>/dev/null; dd if=/tmp/bench-dd of=/dev/null bs=1M 2>/dev/null; rm -f /tmp/bench-dd'
    run_case "tar czf /etc (64K nodes-ballpark)" /bin/bash -c 'tar czf /tmp/bench-t.tgz -C / etc 2>/dev/null; rm -f /tmp/bench-t.tgz'

    echo "== compute (should be ~1x parity) =="
    run_case "awk sum 1..200000" /bin/bash -c 'seq 200000 | awk "{s+=\$1} END {print s}"'

    echo "== git local (repo pre-seeded at /tmp/bench-repo) =="
    run_case "git -C bench-repo status" /usr/bin/git -C /tmp/bench-repo status --porcelain
    run_case "git -C bench-repo log --oneline" /usr/bin/git -C /tmp/bench-repo log --oneline --all

    echo "== notify-vs-ptr lane A/B (value of the notify fast path) =="
    run_case_ab "bash -c true" /bin/bash -c true
    run_case_ab "find /etc -maxdepth 2" /usr/bin/find /etc -maxdepth 2 -type f
fi
