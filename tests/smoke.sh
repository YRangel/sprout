#!/usr/bin/env bash
# tests/smoke.sh — integration smoke for regressions unit tests can't see.
#
# History: exec-name cache + env-snapshot fixes were caught at runtime by a
# user, never by cargo test. This harness drives the real binary against a
# real rootfs and asserts the observable contract end to end.
#
# Usage:  tests/smoke.sh ROOTFS
# Exit:   0 = all gates pass; non-zero = first failing gate.
# Skips:  prints "[smoke] SKIP …" and rc 0 when sprout/rootfs are absent so
#         callers on machines without a test rootfs don't go red.

set -u

rootfs="${1:-}"
[ -n "$rootfs" ] || { echo "usage: $0 ROOTFS" >&2; exit 2; }

log()  { printf '[smoke] %s\n' "$*"; }
pass() { printf '[smoke] PASS %s\n' "$*"; }
fail() { printf '[smoke] FAIL %s\n' "$*" >&2; exit 1; }
skip() { printf '[smoke] SKIP %s\n' "$*"; exit 0; }

command -v sprout >/dev/null 2>&1 || skip "sprout not on PATH"
[ -d "$rootfs" ] || skip "rootfs missing: $rootfs"
[ -x "$rootfs/bin/sh" ] || skip "rootfs has no /bin/sh: $rootfs"

run() { # run <guest-cmd...> -> stdout; rc carried in $rc
    out="$(sprout -r "$rootfs" --user=0:0 -- "$@" 2>&1)"; rc=$?
}

# 1. basic exec + identity
run /bin/sh -c 'echo SPROUT_ALIVE; id -u'
[ $rc -eq 0 ] || fail "basic exec rc=$rc: $out"
printf '%s' "$out" | grep -q SPROUT_ALIVE || fail "no SPROUT_ALIVE marker"
pass "guest exec + id"

# 2. path translation: a file that exists ONLY in the guest rootfs
run /bin/sh -c 'cat /etc/os-release | head -1'
[ $rc -eq 0 ] || fail "guest /etc/os-release rc=$rc"
printf '%s' "$out" | grep -qiE 'debian|ubuntu|alpine|PRETTY' \
    || fail "os-release not guest-translated: $out"
pass "guest path translation (/etc/os-release readable)"

# 3. exec chain survives env -i (the gdk-pixbuf regression class)
run /usr/bin/env -i /bin/sh -c 'echo POST_WIPE'
[ $rc -eq 0 ] || fail "env -i chain rc=$rc: $out"
printf '%s' "$out" | grep -q POST_WIPE || fail "env -i exec-wipe broke chain: $out"
# double-wipe (ctor snapshot must seed the second too)
run /usr/bin/env -i /usr/bin/env -i /bin/sh -c 'echo POST_WIPE2'
printf '%s' "$out" | grep -q POST_WIPE2 || fail "double env -i broke chain"
pass "exec chain survives env -i (single + double wipe)"

# 4. fork/exec loop (exec-name cache regression class)
run /bin/sh -c 'i=0; while [ $i -lt 8 ]; do /bin/true || exit 1; i=$((i+1)); done; echo LOOPED'
printf '%s' "$out" | grep -q LOOPED || fail "8x /bin/true loop: $out"
pass "repeated fork/exec loop"

# 5. unknown flags fail loudly (never silently handed to guest)
sprout --definitely-not-a-flag >/dev/null 2>&1
bad_rc=$?
[ $bad_rc -ne 0 ] || fail "unknown flag accepted"
pass "unknown flags rejected (rc=$bad_rc)"

# 6. help tiers: -h is compact (≤64 lines), --help is the full prose variant.
#    Both print ONE Usage block (regression insurance for the width-doubling bug).
s="$(sprout -h 2>&1)"; l="$(sprout --help 2>&1)"
uc_s="$(printf '%s' "$s" | grep -c '^Usage')"; uc_l="$(printf '%s' "$l" | grep -c '^Usage')"
[ "$uc_s" -eq 1 ] || fail "-h prints Usage $uc_s times (want 1)"
[ "$uc_l" -eq 1 ] || fail "--help prints Usage $uc_l times (want 1)"
ls_="$(printf '%s' "$s" | wc -l)"; ll="$(printf '%s' "$l" | wc -l)"
[ "$ls_" -le 64 ] || fail "-h is $ls_ lines — the compact tier drifted (want ≤64)"
[ "$ll" -ge "$ls_" ] || fail "--help ($ll lines) is shorter than -h ($ls_) — tier inversion"
pass "help tiers: -h ${ls_}lines ≤ 64, --help ${ll}lines ≥ -h, both one Usage block"

log "ALL SMOKE GATES PASS (6 pre-upkg)"

# 7. upkg extracts a tarball with hardlinks and guests boot afterwards
TD=$(mktemp -d)
mkdir -p "$TD/in"
printf "alpha" > "$TD/in/base"
ln "$TD/in/base" "$TD/in/dup" 2>/dev/null || cp "$TD/in/base" "$TD/in/dup"
tar -czf "$TD/pkg.tar.gz" -C "$TD/in" .
mkdir "$TD/out"
sprout upkg "$TD/pkg.tar.gz" -C "$TD/out" 2>/dev/null > /dev/null || fail "upkg exited non-zero"
[ -f "$TD/out/base" ] || fail "upkg: base missing"
[ -f "$TD/out/dup" ]  || fail "upkg: dup missing"
cmp -s "$TD/out/base" "$TD/out/dup" || fail "upkg: hardlink replicated as copy but contents differ"
pass "upkg extracts tarball with hardlinks as content-equivalent copies"
rm -rf "$TD"

log "ALL SMOKE GATES PASS"
