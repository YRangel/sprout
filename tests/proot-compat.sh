#!/usr/bin/env bash
# tests/proot-compat.sh — assert the advertised proot / proot-distro flag
# surface actually parses and behaves. This is the v0.1 contract ("drop-in
# proot CLI compat") turned into an executable check.
#
# Usage:  tests/proot-compat.sh ROOTFS
# Exit:   0 = every compat gate passes; non-zero = first failing gate.
# Skips:  prints "[compat] SKIP …" rc 0 when sprout/rootfs absent.

set -u
rootfs="${1:-}"
[ -n "$rootfs" ] || { echo "usage: $0 ROOTFS" >&2; exit 2; }

log()  { printf '[compat] %s\n' "$*"; }
pass() { printf '[compat] PASS %s\n' "$*"; }
fail() { printf '[compat] FAIL %s\n' "$*" >&2; exit 1; }
skip() { printf '[compat] SKIP %s\n' "$*"; exit 0; }

command -v sprout >/dev/null 2>&1 || skip "sprout not on PATH"
[ -x "$rootfs/bin/sh" ] || skip "rootfs has no /bin/sh: $rootfs"

run() { out="$(sprout -r "$rootfs" "$@" 2>&1)"; rc=$?; }

# -0 / --root-id : fakeroot default is documented; explicit flag must parse.
run -0 --user=0:0 -- /bin/sh -c 'id -u'
printf '%s' "$out" | grep -q '^0' || fail "-0 rc=$rc: $out"
pass "-0 / --root-id parses and yields uid 0"

# -i / --change-id : proot-distro muscle-memory alias for --user.
# Use uid form so no /etc/passwd lookup dependency creeps in.
run -i 0:0 -- /bin/sh -c 'echo UID_ALIVE'
[ $rc -eq 0 ] && printf '%s' "$out" | grep -q UID_ALIVE \
    || fail "-i alias rc=$rc: $out"
pass "-i UID:GID alias (proot --change-id) parses and executes"

# -L : obsolete proot flag must ACCEPT (parse + note) not error.
sprout -r "$rootfs" -L --user=0:0 -- /bin/true >/dev/null 2>&1 \
    || fail "-L rejected (proot compat acceptance broken)"
pass "-L accepted as obsolete no-op"

# -k / --kernel-release : uname override must take effect.
run -k 99.88.77-sprout --user=0:0 -- /bin/sh -c 'uname -r'
printf '%s' "$out" | grep -q '99.88.77-sprout' \
    || fail "-k release not echoed by uname: $out"
pass "-k / --kernel-release overrides uname -r"

# -b : bind a host file into the guest path space.
H="$rootfs/tmp/.compat-bind-src-$$"
echo "BIND_MARKER_$$" > "$H"
sprout -r "$rootfs" -b "$H:/compat-bind-dst" --user=0:0 -- \
    /bin/sh -c 'cat /compat-bind-dst' 2>&1 | grep -q "BIND_MARKER_$$" \
    || fail "-b host:guest bind not visible"
rm -f "$H"
pass "-b HOST:GUEST bind visible inside guest"

# -b RELATIVE host path: resolves against launcher cwd (proot canonicalizes
# silently with realpath; sprout prints a note + takes the absolute form)
mkdir -p "$PREFIX/tmp/compat-rel-$$"
echo "REL_CONTENT_$$" > "$PREFIX/tmp/compat-rel-$$/rel.txt"
(cd "$PREFIX/tmp/compat-rel-$$" &&
    sprout -r "$rootfs" -b rel.txt:/compat-rel-dst --user=0:0 -- cat /compat-rel-dst 2>&1) \
    | grep -q "REL_CONTENT_$$" \
    || fail "-b relative host path bind"
rm -rf "$PREFIX/tmp/compat-rel-$$"
pass "-b relative host path resolves against launcher cwd"

# -b nonexistent host path: warn + skip, command still runs (proot parity)
out=$(sprout -r "$rootfs" -b "/definitely/not/here-$$" -- /bin/true >/dev/null 2>&1 && echo ok)
[ "$out" = "ok" ] || fail "-b nonexistent must not kill the launch"
pass "-b nonexistent host path warn+skip, command still runs"

# -w : guest working directory honored. Use /var (real dir, no /tmp ->
# /dev/shm symlink chase on this rootfs) for a stable pwd.
run -w /var --user=0:0 -- /bin/sh -c 'pwd'
printf '%s' "$out" | grep -qE '^/var$' || fail "-w /var -> pwd gave: $out"
pass "-w GUEST_DIR sets guest cwd"

# --link2symlink / --no-link2symlink must both parse (default ON).
sprout -r "$rootfs" --link2symlink    --user=0:0 -- /bin/true >/dev/null 2>&1 || fail "--link2symlink rejected"
sprout -r "$rootfs" --no-link2symlink --user=0:0 -- /bin/true >/dev/null 2>&1 || fail "--no-link2symlink rejected"
pass "--link2symlink / --no-link2symlink both parse"

# unknown flags must FAIL loudly (never silently handed to the guest).
sprout -r "$rootfs" --some-proot-flag-that-never-existed -- /bin/true >/dev/null 2>&1
[ $? -ne 0 ] || fail "unknown proot-style flag silently accepted"
pass "unknown flags fail loudly"

log "ALL PROOT-COMPAT GATES PASS"
