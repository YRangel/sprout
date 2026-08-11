#!/bin/sh
# bench/run.sh — reproducible sprout-vs-proot benchmark harness.
#
# Usage:  sh bench/run.sh <rootfs> [runs]
# Output: bench/results-<ts>.json
#
# Requires: sprout at $SPROUT_BIN (default target/release/sprout), proot
# in PATH, GNU time(1). Runs are sequential; both binaries get the same
# warm page cache. Pin CPUs externally via taskset for stable numbers.

set -eu

ROOTFS=${1:?usage: run.sh <rootfs> [runs]}
RUNS=${2:-5}
OUT=bench/results-$(date +%Y%m%d-%H%M%S).json
SPROUT_BIN=${SPROUT_BIN:-target/release/sprout}

command -v proot >/dev/null     || { echo "bench: proot not in PATH" >&2; exit 2; }
test -x "$SPROUT_BIN"           || { echo "bench: $SPROUT_BIN missing; cargo build --release" >&2; exit 2; }

median_of() {
    i=1; times=""
    while [ "$i" -le "$RUNS" ]; do
        t=$( { /usr/bin/time -f '%e' "$@" >/dev/null 2>&1; } 2>&1 )
        times="$times $t"
        i=$((i + 1))
    done
    # shellcheck disable=SC2086
    echo $times | tr ' ' '\n' | sort -n | awk '{v[NR]=$1} END{print v[int((NR+1)/2)]}'
}

emit_case() {
    name=$1; last=$2; shift 2
    p=$(median_of proot -r "$ROOTFS" -- "$@")
    s=$(median_of "$SPROUT_BIN" -r "$ROOTFS" -- "$@")
    if [ "$last" = 1 ]; then comma=''; else comma=','; fi
    printf '    {"name": "%s", "proot_s": %s, "sprout_s": %s}%s\n' "$name" "$p" "$s" "$comma"
}

{
  printf '{\n'
  printf '  "rootfs": "%s",\n' "$ROOTFS"
  printf '  "runs": %s,\n' "$RUNS"
  printf '  "cases": [\n'
  emit_case "node-version" 0 /usr/bin/node --version
  emit_case "python-pass"  0 /usr/bin/python3 -c pass
  emit_case "find-storm"   0 /usr/bin/find /usr -maxdepth 2
  emit_case "true"         1 /bin/true
  printf '  ]\n}\n'
} > "$OUT"

echo "wrote $OUT"
