#!/data/data/com.termux/files/usr/bin/bash
# bench/run-all.sh — COMPREHENSIVE sprout-vs-proot suite: identity, CPU/workload
# medians, statics lane, and the vkmark GPU matrix, folded into one
# results/comprehensive-<ts>/ dir with a collated summary.md + machine-info
# provenance block.
#
# usage:  bench/run-all.sh [rootfs] [iterations]
# env:    SECTIONS="cpu statics vkmark"  (space-filter; default: all)
#         VK_SIZE=1280x720  VK_PM=mailbox|immediate
#         ITER=<n>  (iterations for the median suites; default 3)
#         OUT=<dir> (default bench/results/comprehensive-$(ts))
set -u
cd "$(dirname "$0")/.." || exit 1

B=${1:-$PREFIX/var/lib/proot-distro/containers/debian/rootfs}
ITER=${ITER:-${2:-3}}
SECTIONS=${SECTIONS:-"cpu statics vkmark"}
VK_SIZE=${VK_SIZE:-1280x720}
VK_PM=${VK_PM:-}
OUT=${OUT:-bench/results/comprehensive-$(date +%Y%m%d-%H%M%S)}
# substrate parity by default: the proot column uses the raw control lane
# against $B — same rootfs, same Mesa, same guest tooling as sprout.
# Set PROOT_LANE=pd for the proot-distro-container posture (older runs).
export PROOT_LANE=${PROOT_LANE:-control}
mkdir -p "$OUT"/{cpu,statics,vkmark,proben}

has_section() { case " $SECTIONS " in *" $1 "*) return 0;; *) return 1;; esac; }

step() { echo; echo "== $(date +%H:%M:%S)  $* ==" | tee -a "$OUT/summary.md"; }

# ---------------- machine-info / provenance ----------------
{
  echo "# sprout comprehensive bench"
  echo
  echo "date:    $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "host:    $(getprop ro.product.model 2>/dev/null) / Android $(getprop ro.build.version.release 2>/dev/null) (SDK $(getprop ro.build.version.sdk 2>/dev/null))"
  echo "kernel:  $(uname -r)"
  echo "arch:    $(uname -m)"
  echo "rootfs:  $B"
  echo "git:     $(git rev-parse --short HEAD 2>/dev/null) ($(git status --short | wc -l) uncommitted)"
  echo "sprout:  $(md5sum $PREFIX/bin/sprout 2>/dev/null | awk '{print $1}')"
  echo "super:   $(md5sum $PREFIX/bin/sprout-super 2>/dev/null | awk '{print $1}')"
  echo "preload: $(md5sum $PREFIX/bin/libsprout-core.so 2>/dev/null | awk '{print $1}')"
  echo "proot:   $(proot --version 2>/dev/null | head -1)"
  echo "vkmark:  $([ -x "$B/usr/bin/vkmark" ] && "$B/usr/bin/vkmark" --version 2>/dev/null || echo absent) (guest)"
  echo
} | tee "$OUT/machine-info.md"

{
  echo "# summary"
  echo
} > "$OUT/summary.md"

# ---------------- smoke gates (fail-fast) ----------------
step "smoke gates"
ok=1
for probe in \
  "sprout -r $B --user=0:0 -- true" \
  "sprout -r $B --user=0:0 -- /usr/bin/busybox true" \
  "sprout -r $B --user=0:0 -- python3 -c pass" ; do
  if timeout 60 $probe >/dev/null 2>&1; then
    echo "- PASS: $probe" | tee -a "$OUT/summary.md"
  else
    echo "- **FAIL** (aborting): $probe" | tee -a "$OUT/summary.md"; ok=0
  fi
done
[ $ok -eq 1 ] || { echo "smoke-gate: red — suite aborted" | tee -a "$OUT/summary.md"; exit 1; }

# ---------------- section: CPU/workload ----------------
if has_section cpu; then
  step "cpu/workload (MODE=full, median-of-$ITER, sprout vs proot-distro)"
  MODE=full ROOTFS=$B bash bench/run.sh "$B" "$ITER" >"$OUT/cpu/run.log" 2>&1
  rc=$?
  tail -n +1 "$OUT/cpu/run.log" | grep -aE "speedup|FAIL" | sed 's/^/    /' | tee -a "$OUT/summary.md"
  echo "(full log: $OUT/cpu/run.log; rc=$rc)" | tee -a "$OUT/summary.md"
fi

# ---------------- section: statics ----------------
if has_section statics; then
  step "statics lane (median-of-$ITER, sprout-notify / sprout-ptrace vs proot)"
  ROOTFS=$B bash bench/run-statics.sh "$B" "$ITER" >"$OUT/statics/run.log" 2>&1
  rc=$?
  grep -aE "proot-vs-notify|ptrace-vs-notify|FAIL" "$OUT/statics/run.log" | sed 's/^/    /' | tee -a "$OUT/summary.md"
  echo "(full log: $OUT/statics/run.log; rc=$rc)" | tee -a "$OUT/summary.md"
fi

# ---------------- section: vkmark GPU ----------------
if has_section vkmark; then
  step "vkmark matrix (Turnip GPU + llvmpipe CPU, sprout vs proot, $VK_SIZE ${VK_PM:-default-PM})"
  if [ -n "${DISPLAY:-}" ] && [ -e "$PREFIX/tmp/.X11-unix/X${DISPLAY#:}" ]; then
    env B="$B" SIZE="$VK_SIZE" PM="$VK_PM" OUT="$OUT/vkmark" bash bench/run-vkmark.sh >"$OUT/vkmark/run.log" 2>&1
    rc=$?
    grep -aE '^==|vkmark Score|FAIL' "$OUT/vkmark/run.log" | sed 's/^/    /' | tee -a "$OUT/summary.md"
    echo "(full log: $OUT/vkmark/run.log; rc=$rc)" | tee -a "$OUT/summary.md"
  else
    echo "- SKIP: no live X display (set DISPLAY or start termux-x11) — vkmark cannot measure" | tee -a "$OUT/summary.md"
  fi
fi

# ---------------- final ----------------
step "done"
echo "results: $OUT" | tee -a "$OUT/summary.md"
echo "summary: $OUT/summary.md"
