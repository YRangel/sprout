#!/usr/bin/env bash
# test-sysvipc-shim.sh — build + runtime-assert the SysV-IPC LD_PRELOAD shim.
#
# Modes:
#   --build-only            compile shim DSOs (x86_64 + i386) + test ELFs; no run.
#                           For ubuntu CI: asserts the sources stay warning-clean.
#                           Needs HOST cross gcc (apt gcc-x86-64-linux-gnu ...).
#   --run ROOTFS            host-builds, then runs asserts INSIDE the sprout guest
#                           with the shim preloaded (needs host cross gcc). Deprecated
#                           in favor of --run-in-guest on-device.
#   --run-in-guest ROOTFS   build AND run inside the sprout guest. The guest has the
#                           cross toolchains + box64 + the real Android-kernel ENOSYS
#                           path. This is the on-device / Termux-runner mode (no host
#                           cross gcc needed).
#
# The test ELFs are x86 binaries (that's the steam/box64 ABI the shim exists
# for); they run under the guest's box64 with the matching-arch shim DSO.
# semtest exercises create/GETVAL/P/V/RMID + IPC_PRIVATE.
# semfork  exercises P/V across a fork (file-backed state, two processes).
# semxab   exercises one semaphore across x86_64-waiter × i386-poster.

set -euo pipefail

mode="${1:-}"
rootfs="${2:-}"

here="$(cd "$(dirname "$0")" && pwd)"
# script lives in <repo>/scripts/, so the shim sources sit one level up.
repo="$(cd "$here/.." && pwd)"
sysvdir="$repo/csrc/sprout-sysvipc"
work="$sysvdir/build"

log() { printf '[sysvipc-test] %s\n' "$*"; }
die() { printf '[sysvipc-test] FAIL: %s\n' "$*" >&2; exit 1; }

build_all() {
    log "building shim DSOs (x86_64, i386)"
    make -C "$sysvdir" all >/dev/null
    log "building test ELFs (semtest x86_64+i386, semfork x86_64, semxab x86_64+i386)"
    ( cd "$sysvdir" && \
      x86_64-linux-gnu-gcc -O2 -Wall -Wextra -o build/x86_64/semtest semtest.c && \
      i686-linux-gnu-gcc   -O2 -Wall -Wextra -o build/i386/semtest   semtest.c && \
      x86_64-linux-gnu-gcc -O2 -Wall -Wextra -o build/x86_64/semfork semfork.c && \
      x86_64-linux-gnu-gcc -O2 -Wall -Wextra -o build/x86_64/semxab  semxab.c && \
      i686-linux-gnu-gcc   -O2 -Wall -Wextra -o build/i386/semxab    semxab.c )
    log "build ok"
}

case "$mode" in
  --build-only)
    command -v x86_64-linux-gnu-gcc >/dev/null || die "missing x86_64-linux-gnu-gcc"
    command -v i686-linux-gnu-gcc   >/dev/null || die "missing i686-linux-gnu-gcc"
    build_all
    ;;
  --run)
    [ -n "$rootfs" ] || die "--run needs a ROOTFS path"
    [ -d "$rootfs" ] || die "rootfs not found: $rootfs"
    build_all
    # stage shim + tests into the guest at /tmp/sprout-sysvipc-it
    it="$rootfs/tmp/sprout-sysvipc-it"
    rm -rf "$it"; mkdir -p "$it"/{x86_64,i386}
    cp "$work/x86_64/libsprout-sysvipc.so" "$it/x86_64/"
    cp "$work/i386/libsprout-sysvipc.so"   "$it/i386/"
    cp "$work/x86_64/semtest" "$work/x86_64/semfork" "$work/x86_64/semxab" "$it/x86_64/"
    cp "$work/i386/semtest"   "$work/i386/semxab"    "$it/i386/"

    # guest-side driver: preload shim matching the ELF arch, via box64.
    cat >"$it/run.sh" <<'EOS'
#!/bin/bash
set -euo pipefail
export SPROUT_SYSVIPC_DIR=/tmp/sprout-sysvipc-it/state
mkdir -p "$SPROUT_SYSVIPC_DIR"
it=/tmp/sprout-sysvipc-it
run() { # run <arch> <elf> [args...]  -> preload arch-matched shim under box64
  local arch="$1" elf="$2"; shift 2
  LD_PRELOAD="$it/$arch/libsprout-sysvipc.so" box64 "$it/$arch/$elf" "$@"
}
echo "== semtest x86_64 =="; run x86_64 semtest
echo "== semtest i386   =="; run i386   semtest
echo "== semfork x86_64 =="; run x86_64 semfork
echo "== semxab cross   =="
run i386 semxab waiter & wp=$!
sleep 1
run x86_64 semxab poster
wait "$wp"
echo "ALL-SYSVIPC-OK"
EOS
    chmod +x "$it/run.sh"

    command -v sprout >/dev/null || die "sprout not on PATH"
    log "running shim asserts inside guest $rootfs"
    out="$(sprout -r "$rootfs" --user=0:0 -- /bin/bash /tmp/sprout-sysvipc-it/run.sh 2>&1)"
    printf '%s\n' "$out"
    printf '%s' "$out" | grep -q 'ALL-SYSVIPC-OK' || die "guest asserts did not complete"
    log "RUNTIME ASSERTS PASS"
    ;;
  --run-in-guest)
    [ -n "$rootfs" ] || die "--run-in-guest needs a ROOTFS path"
    [ -d "$rootfs" ] || die "rootfs not found: $rootfs"
    command -v sprout >/dev/null || die "sprout not on PATH"
    # stage sources into the guest and let the guest build + assert.
    it="$rootfs/tmp/sprout-sysvipc-it"
    rm -rf "$it"; mkdir -p "$it"
    cp "$sysvdir"/sprout_sysvipc.c "$sysvdir"/semtest.c \
       "$sysvdir"/semfork.c "$sysvdir"/semxab.c "$it/"
    cat >"$it/run.sh" <<'EOS'
#!/bin/bash
set -euo pipefail
cd /tmp/sprout-sysvipc-it
it=/tmp/sprout-sysvipc-it
mkdir -p x86_64 i386
echo "== build shim (x86_64, i386) =="
x86_64-linux-gnu-gcc -O2 -Wall -Wextra -Wpedantic -fPIC -shared -o x86_64/libsprout-sysvipc.so sprout_sysvipc.c
i686-linux-gnu-gcc   -O2 -Wall -Wextra -Wpedantic -fPIC -shared -o i386/libsprout-sysvipc.so   sprout_sysvipc.c
echo "== build test ELFs =="
x86_64-linux-gnu-gcc -O2 -Wall -Wextra -o x86_64/semtest semtest.c
i686-linux-gnu-gcc   -O2 -Wall -Wextra -o i386/semtest   semtest.c
x86_64-linux-gnu-gcc -O2 -Wall -Wextra -o x86_64/semfork semfork.c
x86_64-linux-gnu-gcc -O2 -Wall -Wextra -o x86_64/semxab  semxab.c
i686-linux-gnu-gcc   -O2 -Wall -Wextra -o i386/semxab    semxab.c
export SPROUT_SYSVIPC_DIR=/tmp/sprout-sysvipc-it/state
mkdir -p "$SPROUT_SYSVIPC_DIR"
run() { # run <arch> <elf> [args...] -> arch-matched shim must NOT go through
        # host LD_PRELOAD (host loader is aarch64 and rejects an x86 DSO with
        # "is for EM_X86_64 ... instead of EM_AARCH64" then SIGSEGVs the guest).
        # box64's BOX64_LD_PRELOAD injects the DSO into the emulated guest
        # loader for BOTH x86_64 and i386 (box32 honors the same var).
  local arch="$1" elf="$2"; shift 2
  BOX64_LD_PRELOAD="$it/$arch/libsprout-sysvipc.so" box64 "$it/$arch/$elf" "$@"
}
echo "== semtest x86_64 =="; run x86_64 semtest
echo "== semtest i386   =="; run i386   semtest
echo "== semfork x86_64 =="; run x86_64 semfork
# cross-ABI: one semaphore shared between an i386 waiter and an x86_64
# poster (steam's supervise × client pair). Both resolve key→file the same.
echo "== semxab cross =="
run i386 semxab waiter & wp=$!
sleep 1
run x86_64 semxab poster
wait "$wp"
echo "ALL-SYSVIPC-OK"
EOS
    chmod +x "$it/run.sh"
    log "building + asserting shim inside guest $rootfs"
    out="$(sprout -r "$rootfs" --user=0:0 -- /bin/bash /tmp/sprout-sysvipc-it/run.sh 2>&1)"
    printf '%s\n' "$out"
    printf '%s' "$out" | grep -q 'ALL-SYSVIPC-OK' || die "guest asserts did not complete"
    log "RUNTIME ASSERTS PASS (in-guest build)"
    ;;
  *)
    echo "usage: $0 --build-only | --run ROOTFS | --run-in-guest ROOTFS" >&2; exit 2;;
esac
