#!/data/data/com.termux/files/usr/bin/bash
# bench/soak-long-tail.sh — H8: honest long-tail correctness soaks per
# workload class. NOT a benchmark: the report is rc+errors per phase.
# Approx budget: 20-40 min wall clock on-device. Run, watch for
# supervisor-crashes/hangs/errors that 1-second battery cells can't see.
set -u
B=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
A=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs
S=$PREFIX/bin/sprout
LOG=$TMPDIR/soak-$$
mkdir -p "$LOG"
err=0

phase() { echo; echo "== [$(date +%H:%M:%S)] PHASE $1" | tee -a "$LOG/main.log"; }
note()  { echo "$1" | tee -a "$LOG/main.log"; }
rc() { if [ $2 -eq 0 ]; then note "OK   $1 (rc=0)"; else note "FAIL $1 (rc=$2)"; err=$((err+1)); fi }

phase "1/4 apt lifecycle (debian glibc; agent-level: update+install+remove+purge rounds)"
$S -r $B apt-get -qq update >>"$LOG/apt.log" 2>&1; rc "apt update" $?
for pkg in sl cowsay; do
  $S -r $B apt-get -qq -y install $pkg >>"$LOG/apt.log" 2>&1; rc "apt install $pkg" $?
  $S -r $B dpkg -L $pkg >/dev/null 2>&1; rc "dpkg -L $pkg" $?
done
$S -r $B apt-get -qq -y install python3-numpy >>"$LOG/apt.log" 2>&1; rc "apt install python3-numpy" $?
$S -r $B apt-get -qq -y purge sl >>"$LOG/apt.log" 2>&1; rc "apt purge sl" $?
$S -r $B apt-get -qq autoremove >>"$LOG/apt.log" 2>&1; rc "apt autoremove" $?

phase "2/4 toolchain build soak (hello-2.12 autotools end-to-end)"
TARBALL=$PREFIX/tmp/hello-2.12.tar.gz
tar -xf "$TARBALL" -C "$B/tmp/" 2>>"$LOG/build.log"
(
  $S -r $B /bin/bash -c '
    set -e
    cd /tmp/hello-2.12
    [ -d /tmp/hello-build ] && rm -rf /tmp/hello-build || true
    [ -d hello-build-tmp ] && rm -rf hello-build-tmp || true
    CC=cc ./configure --prefix=/tmp/hello-install > /tmp/cfg.log 2>&1
    make -j2 >> /tmp/make.log 2>&1
  ' >>"$LOG/build.log" 2>&1
); rc "configure+make hello" $?
(
  $S -r $B /bin/bash -c 'cd /tmp/hello-2.12 && make check >> /tmp/check.log 2>&1'
); rc "make check hello" $?
$S -r $B /tmp/hello-2.12/hello --traditional 2>&1 | head -1 | tee -a "$LOG/main.log"

phase "3/4 python scientific import tree (numpy real ops)"
$S -r $B /usr/bin/python3 -c '
import numpy as np, sys
a = np.random.RandomState(0).rand(300, 300)
for i in range(60):
    a = a @ a / (np.linalg.norm(a) + 1e-9)
print("NUMPY-OK", float(a.sum()), file=sys.stderr)
' 2>>"$LOG/python.log"; rc "numpy matmul loop" $?

phase "4/4 musl apk lifecycle (alpine, parity-compared: sprout vs proot-distro)"
# apk's busybox trigger currently prints '1 error' under BOTH runtimes
# (verified 2026-08-12: proot-distro = exact same line) — parity is the
# assertion, not rc=0. Divergence = fail.
i=0
while [ $i -lt 3 ]; do
  sout=$($S -r $A apk add -q jq 2>&1 | tail -1)
  pout=$(proot-distro login alpine -- apk add -q jq 2>&1 | tail -1)
  if [ "$sout" = "$pout" ]; then rc "apk add jq r$i (parity: '$sout')" 0; else note "FAIL apk add jq r$i parity: sprout='$sout' proot='$pout'"; err=$((err+1)); fi
  $S -r $A /usr/bin/jq -n '{a:1}|.a' | grep -q '^1$'; rc "jq runs r$i" $?
  $S -r $A apk del -q jq >/dev/null 2>&1
  proot-distro login alpine -- true >/dev/null 2>&1
  i=$((i+1))
done

phase "RESULT"
note "soft-failures: $err  (logs: $LOG)"
pgrep -f sprout-super | while read p; do
  exe=$(basename "$(readlink /proc/$p/exe 2>/dev/null)" 2>/dev/null)
  [ "$exe" = "sprout-super" ] && note "LEAKED supervisor pid=$p"
done
note "done $(date +%H:%M:%S)"
exit $err
