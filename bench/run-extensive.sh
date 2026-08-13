#!/bin/sh
# bench/run-extensive.sh — full-surface benchmark: every guest class,
# every lane, vs proot-distro. Medians of N. Reproducible: this script
# IS the reproduction recipe (ADR:harness provides numbers, never hand-
# built commands).
#
#   Usage: bench/run-extensive.sh [N]                (default N=7)
#   Env:   SPROUT_BIN (default $PREFIX/bin/sprout)
#          DEB ROOT, ALP ROOT come from proot-distro container dirs
#
# Output: human table on stderr, raw TSV + markdown block under
#   bench/results/extensive-<date>/
set -u

N=${1:-7}
SPROUT_BIN=${SPROUT_BIN:-$PREFIX/bin/sprout}
PDIR=/data/data/com.termux/files/usr/var/lib/proot-distro/containers
DEB=${DEB:-$PDIR/debian/rootfs}
ALP=${ALP:-$PDIR/alpine/rootfs}
CACHE=${SPROUT_CACHE:-$HOME/.cache/sprout}
OUTDIR="$(cd "$(dirname "$0")/../" && pwd)/bench/results/extensive-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUTDIR"
TSV="$OUTDIR/raw.tsv"
MD="$OUTDIR/table.md"
: > "$TSV"

[ -x "$SPROUT_BIN" ] || { echo "sprout missing: $SPROUT_BIN" >&2; exit 1; }
command -v proot-distro >/dev/null || { echo "proot-distro missing" >&2; exit 1; }

# --- timing core ----------------------------------------------------------
# median LABEL -- CMDW...   (eval'd with sh -c so PATH/env variants work)
median() {
    label=$1; shift
    i=0
    while [ "$i" -lt "$N" ]; do
        s=$(date +%s%N); "$@" >/dev/null 2>&1; e=$(date +%s%N)
        echo $(( (e - s) / 1000000 ))
        i=$((i + 1))
    done | sort -n | awk -v n="$N" 'NR==int(n/2)+1 {print $1}' > "$OUTDIR/.m"
    read ms < "$OUTDIR/.m"
    printf '%-42s %8s ms\n' "$label" "$ms" >&2
    echo "$ms"
}

row() { # row SECTION CASE VAL... — one timing value per MD column
    sec=$1; case_=$2; shift 2
    printf '| %s | %s |' "$sec" "$case_" >> "$MD"
    for v in "$@"; do
        printf ' %s |' "$v" >> "$MD"
        printf '%s\t%s\t%s\n' "$sec" "$case_" "$v" >> "$TSV"
    done
    printf '\n' >> "$MD"
}

case3() { # section case -- proot-distro-args... ; measures all three lanes relevant
    sec=$1; case_=$2; distro=$3; rootfs=$4; shift 4
    p=$(median "proot-distro $distro  $case_" proot-distro login "$distro" -- "$@")
    pr=$(median "raw proot           $case_" proot -R "$rootfs" "$@")
    n=$(median "sprout              $case_" "$SPROUT_BIN" -r "$rootfs" "$@")
    if [ -n "$n" ] && [ -n "$p" ]; then
        r=$(echo "$p $n" | awk '{printf "%.2f", $1/$2}')
        echo "=> $case_ sprout-speedup(vs distro) ${r}x"
    fi
    row "$sec" "$case_" "$p" "$pr" "$n"
}

ab() { # labelA labelB case: A/B of a lane pair under sprout only
    sec=$1; case_=$2; rootfs=$3; envA=$4; envB=$5; shift 5
    a=$(median "sprout($envA)  $case_" env "$envA" "$SPROUT_BIN" -r "$rootfs" "$@")
    b=$(median "sprout($envB)   $case_" env "$envB" "$SPROUT_BIN" -r "$rootfs" "$@")
    if [ -n "$a" ] && [ -n "$b" ]; then
        r=$(echo "$a $b" | awk '{printf "%.2f", $2/$1}')
        echo "=> $case_  $envA-vs-$envB >=1 ${r}x"
    fi
    row "$sec" "$case_($envA vs $envB)" "$a" "$b"
}

hdr() { echo; echo "=== $* ===" >&2; }

{
    echo "# extensive benchmark $(date '+%Y-%m-%d %H:%M')"
    echo "- device: $(uname -m), $(getprop ro.product.model 2>/dev/null || echo unknown), kernel $(uname -r)"
    echo "- sprout: $($SPROUT_BIN --version 2>/dev/null | head -1); commit $(cd "$(dirname "$0")/.." && git rev-parse --short HEAD)"
    echo "- medians of N=$N; baselines: proot-distro login AND raw proot -R (fairness)"
    echo
    echo "| section | case | proot-distro | raw proot | sprout |"
    echo "|---------|------|--------------|-----------|--------|"
} > "$MD"

# =========================================================================
hdr "A. glibc guest (debian, dynamic)"
# =========================================================================
EXTC=/tmp/ext-hello.c
cat > "$DEB/$EXTC" <<'EOF'
#include <stdio.h>
int main(void){ for(int i=0;i<3;i++) printf("ext-bench %d\n", i); return 0; }
EOF

case3 "A-glibc" "bash-true" debian "$DEB" /bin/bash -c true
case3 "A-glibc" "find /usr/bin -type f" debian "$DEB" /bin/bash -c 'find /usr/bin -type f | wc -l >/dev/null'
case3 "A-glibc" "exec-chain 20x true" debian "$DEB" /bin/bash -c 'i=0; while [ $i -lt 20 ]; do /bin/true; i=$((i+1)); done'
case3 "A-glibc" "python3 -c pass" debian "$DEB" /usr/bin/python3 -c pass
case3 "A-glibc" "python3 mp-Pool(2)" debian "$DEB" /usr/bin/python3 -c 'from multiprocessing import Pool
Pool(2).map(str.rstrip, ["a ","b ","c "])'
case3 "A-glibc" "cc -O2 hello" debian "$DEB" /usr/bin/cc -O2 -o /tmp/ext-hello /tmp/ext-hello.c
case3 "A-glibc" "dd 64MB fdatasync" debian "$DEB" /bin/bash -c 'dd if=/dev/zero of=/tmp/ext-dd bs=1M count=64 conv=fdatasync 2>/dev/null; rm -f /tmp/ext-dd'
case3 "A-glibc" "dpkg -l wc" debian "$DEB" /bin/bash -c 'dpkg -l | wc -l >/dev/null'
ab "A-glibc" "bash-true" "$DEB" SPROUT_USER_NOTIFY=0 SPROUT_USER_NOTIFY=1 /bin/bash -c true
ab "A-glibc" "find-storm" "$DEB" SPROUT_USER_NOTIFY=0 SPROUT_USER_NOTIFY=1 /bin/bash -c 'find /usr/bin -type f | wc -l >/dev/null'

# cache cold/warm (glibc lane sanitization + loader cache)
rm -rf "$CACHE"
cOLD=$(median "sprout bash-true COLD" "$SPROUT_BIN" -r "$DEB" /bin/bash -c true)
wARM=$(median "sprout bash-true WARM" "$SPROUT_BIN" -r "$DEB" /bin/bash -c true)
row "A-glibc" "bash-true (cold vs warm)" "$cOLD" "$wARM"
rm -f "$DEB/tmp/ext-hello" "$DEB/$EXTC"

# =========================================================================
hdr "B. statics (debian rootfs)"
# =========================================================================
case3 "B-static" "sp_min (nop static)" debian "$DEB" /tmp/sp_min
case3 "B-static" "sp_statloop" debian "$DEB" /tmp/sp_statloop
case3 "B-static" "sp_ioloop" debian "$DEB" /tmp/sp_ioloop
case3 "B-static" "glibc-static step" debian "$DEB" /tmp/step
[ -x "$DEB/tmp/cloudflared" ] && case3 "B-static" "cloudflared --version" debian "$DEB" /tmp/cloudflared --version
case3 "B-static" "static->script exec_script" debian "$DEB" /tmp/exec_script
ab "B-static" "sp_ioloop" "$DEB" SPROUT_NOTIFY_STATICS=1 SPROUT_NOTIFY_STATICS=0 /tmp/sp_ioloop
ab "B-static" "sp_statloop" "$DEB" SPROUT_NOTIFY_STATICS=1 SPROUT_NOTIFY_STATICS=0 /tmp/sp_statloop

# =========================================================================
hdr "C. musl guest (alpine)"
# =========================================================================
case3 "C-musl" "sh-true (busybox ash)" alpine "$ALP" /bin/sh -c true
case3 "C-musl" "find /usr/bin -type f" alpine "$ALP" /bin/sh -c 'find /usr/bin -type f | wc -l >/dev/null'
case3 "C-musl" "busybox chain 20x" alpine "$ALP" /bin/sh -c 'i=0; while [ $i -lt 20 ]; do /bin/true; i=$((i+1)); done'
case3 "C-musl" "dd 64MB fdatasync" alpine "$ALP" /bin/sh -c 'dd if=/dev/zero of=/tmp/ext-dd bs=1M count=64 conv=fdatasync 2>/dev/null; rm -f /tmp/ext-dd'
ab "C-musl" "sh-true" "$ALP" SPROUT_USER_NOTIFY=0 SPROUT_USER_NOTIFY=1 /bin/sh -c true
ab "C-musl" "find-storm" "$ALP" SPROUT_USER_NOTIFY=0 SPROUT_USER_NOTIFY=1 /bin/sh -c 'find /usr/bin -type f | wc -l >/dev/null'

echo
echo "artifacts: $OUTDIR" >&2
cat "$MD"
