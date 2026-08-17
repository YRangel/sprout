#!/bin/sh
# install.sh — install sprout locally (Termux-first).
#
# Copies the runtime artifacts side-by-side into ${PREFIX:-$HOME/.local}/bin
# (trio + optional libsprout-core-musl.so for Alpine guests)
# so sprout's sibling-of-argv[0] discovery finds them:
#   sprout            — Rust launcher (CLI)
#   libsprout-core.so — C interposer (glibc-linked; must come from CI artifact
#                       or an in-guest build, NOT from a bionic host build)
#   sprout-super      — supervisor (static-binary fallback; v0.5.1 rename,
#                       legacy `sprout-ptrace` symlinked for compat)
#
# Usage: ./install.sh [--verify|--check] [dest-dir] [source-dir]
#   --verify  install, then verify copies match source hashes (default verify=on)
#   --check   verify EXISTING install only; exit 1 on mismatch, no copying
set -eu

VERIFY=0
CHECK=0
case ${1:-} in
    --verify) VERIFY=1; shift ;;
    --check)  CHECK=1;  shift ;;
esac

DEST=${1:-${PREFIX:-$HOME/.local}/bin}
SRC=${2:-$(cd "$(dirname "$0")" && pwd)}

# Freshness guard (stale-slot trap, deployed twice on 2026-08-16):
# build/*/out accumulates hashed dirs keyed by build-input hash; a stale slot
# from an EARLIER compile of the same crate can shadow the just-built fresh
# one if the glob returns them in the wrong order. Guard by comparing each
# slot's mtime against the newest source file of its crate; a slot older than
# the csrc is rejected with an actionable warning instead of silently
# deploying pre-fix bytes.
csrc_max_mtime() {
    # $1 = crate dir containing csrc/ (or Rust src/)
    find "$SRC/crates/$1/csrc" "$SRC/crates/$1/src" \
         -name '*.c' -o -name '*.h' -o -name '*.rs' -o -name 'Cargo.toml' 2>/dev/null \
        | xargs -r $STAT_MTIME 2>/dev/null | sort -rn | head -1
}
crate_of() {
    case "$1" in
        sprout)          echo sprout-cli ;;
        sprout-super|sprout-ptrace|sprout-stub) echo sprout-ptrace ;;
        libsprout-core.so|libsprout-core-musl.so) echo sprout-preload ;;
        *)               echo "" ;;
    esac
}
STAT_MTIME='stat -c %Y'
command -v stat >/dev/null 2>&1 || STAT_MTIME='ls -lT'

pick() {
    # $1 = filename to locate under $SRC
    case ${2:-} in
        bin) if [ -f "$SRC/target/release/$1" ]; then echo "$SRC/target/release/$1"; return; fi
             if [ -f "$SRC/target/debug/$1" ]; then echo "$SRC/target/debug/$1"; return; fi ;;
    esac
    # build/*/out accumulates per-source-changes: newest must win
    # (deploy-discipline law — first-glob can silently downgrade, and did).
    local newest crate nmt smt
    newest=$(ls -t "$SRC"/target/release/build/*/out/"$1" "$SRC"/target/debug/build/*/out/"$1" 2>/dev/null | head -1)
    if [ -n "$newest" ]; then
        crate=$(crate_of "$1")
        if [ -n "$crate" ] && command -v stat >/dev/null 2>&1; then
            smt=$(csrc_max_mtime "$crate")
            nmt=$($STAT_MTIME "$newest" 2>/dev/null)
            if [ -n "$smt" ] && [ -n "$nmt" ] && [ "$nmt" -lt "$smt" ]; then
                echo "install.sh: STALE $newest (mtime $nmt < csrc mtime $smt)" >&2
                echo "install.sh: run 'cargo build --release' first, or rm -rf target/release/build/*/out/" >&2
            else
                echo "$newest"; return
            fi
        else
            echo "$newest"; return
        fi
    fi
    # Loose-root fallback: only when NOT in a git checkout (tarball use),
    # or with explicit SPROUT_ALLOW_LOOSE=1 — a stale stray .so from an old
    # verify rehearsal must NOT silently re-install (deploy-discipline law).
    for d in "$SRC" "$SRC"/target; do
        if [ -f "$d/$1" ]; then
            if [ -d "$SRC/.git" ] && [ "${SPROUT_ALLOW_LOOSE:-0}" != "1" ]; then
                echo "install.sh: ignoring loose $d/$1 inside git checkout" >&2
                echo "install.sh: SPROUT_ALLOW_LOOSE=1 to override, else the release fetch will supply it" >&2
                continue
            fi
            echo "$d/$1"; return
        fi
    done
    echo ""
}

SP=$(pick sprout bin)
MS=$(pick libsprout-core-musl.so)
SO=$(pick libsprout-core.so)
PX=$(pick sprout-super); [ -n "$PX" ] || PX=$(pick sprout-ptrace)
PS=$(pick sprout-stub)

# Source builds on a bionic host (Termux) can never produce the glibc-linked
# interposer (build.rs skips it deliberately) — fetch the pair from the
# matching GitHub release instead, hash-verified. Never shadow artifacts
# that DO exist locally (deploy-discipline law: freshness beats presence).
fetch_interposers() {
    [ -n "$SO" ] && [ -n "$MS" ] && return 0
    command -v curl >/dev/null 2>&1 || return 1
    command -v sha256sum >/dev/null 2>&1 || return 1
    local base=https://github.com/YRangel/sprout/releases/latest/download
    local dlm
    dlm=$(mktemp -d "${TMPDIR:-/tmp}/sprout-fetch.XXXXXX")
    mkdir -p "$dlm"
    curl -sfL "$base/sprout-guest-interposers-aarch64.tar.xz" -o "$dlm/sprout-guest-interposers-aarch64.tar.xz" || return 1
    curl -sfL "$base/SHA256SUMS" -o "$dlm/SHA256SUMS" || return 1
    (cd "$dlm" && sha256sum --ignore-missing -c SHA256SUMS >/dev/null) || return 1
    (cd "$dlm" && tar -xJf sprout-guest-interposers-aarch64.tar.xz) || return 1
    [ -f "$dlm/libsprout-core.so" ] && [ -f "$dlm/libsprout-core-musl.so" ] || return 1
    [ -n "$SO" ] || SO="$dlm/libsprout-core.so"
    [ -n "$MS" ] || MS="$dlm/libsprout-core-musl.so"
    printf 'install.sh: fetched prebuilt interposers from GitHub release (hash-verified)\n'
    return 0
}
fetch_interposers || true

missing=""
[ -n "$SP" ] || missing="$missing sprout"
[ -n "$SO" ] || missing="$missing libsprout-core.so"
[ -n "$PX" ] || missing="$missing sprout-super"
if [ -n "$missing" ]; then
    echo "install.sh: missing artifacts:$missing" >&2
    echo "  build with: cargo build --release --workspace && rebuild the interposer in a guest (see docs/src/development.md)," >&2
    echo "  or download the release tarball from GitHub." >&2
    exit 1
fi

verify_pair() { # verify_pair SRC DEST — md5 equality, loud fail
    local s d; s=$(md5sum "$1" 2>/dev/null | awk '{print $1}')
    d=$(md5sum "$2" 2>/dev/null | awk '{print $1}')
    if [ "$s" != "$d" ] || [ -z "$s" ]; then
        echo "install.sh: HASH MISMATCH $2 (src=$s dst=$d)" >&2; return 1
    fi
}

mkdir -p "$DEST"
if [ $CHECK -eq 1 ]; then
    rc=0
    for pair in "$SP sprout" "$SO libsprout-core.so" "$PX sprout-super"; do
        set -- $pair; verify_pair "$1" "$DEST/$2" || rc=1
    done
    [ -n "$PS" ] && { [ -f "$DEST/sprout-stub" ] && verify_pair "$PS" "$DEST/sprout-stub" || rc=1; }
    [ -n "$MS" ] && { [ -f "$DEST/libsprout-core-musl.so" ] && verify_pair "$MS" "$DEST/libsprout-core-musl.so" || rc=1; }
    [ "$(readlink "$DEST/sprout-ptrace" 2>/dev/null)" = "sprout-super" ] || rc=1
    [ $rc -eq 0 ] && echo "install.sh --check: ALL artifacts match source ($DEST)"
    exit $rc
fi
# Compatibility: --verify keeps working after the install below makes DEST
# mirror SRC (the post-install hash pass is the actual verification).
: $VERIFY

cp "$SP" "$DEST/sprout"
cp "$SO" "$DEST/libsprout-core.so"
cp "$PX" "$DEST/sprout-super"
ln -sf sprout-super "$DEST/sprout-ptrace"  # legacy name (fastfetch-style comm reads, scripts)
[ -n "$PS" ] && { cp "$PS" "$DEST/sprout-stub"; chmod 755 "$DEST/sprout-stub"; }
if [ -n "$MS" ]; then cp "$MS" "$DEST/libsprout-core-musl.so"; fi
chmod 755 "$DEST/sprout" "$DEST/sprout-super"

# deploy self-check (H5): an install whose hashes diverge from source is a
# stale-artifact footgun (one violation almost shipped 2026-08-12).
fails=""
verify_pair "$SP" "$DEST/sprout"        2>/dev/null || fails="$fails sprout"
verify_pair "$SO" "$DEST/libsprout-core.so" 2>/dev/null || fails="$fails libsprout-core.so"
verify_pair "$PX" "$DEST/sprout-super"  2>/dev/null || fails="$fails sprout-super"
[ -n "$PS" ] && { verify_pair "$PS" "$DEST/sprout-stub" 2>/dev/null || fails="$fails sprout-stub"; }
[ -n "$MS" ] && { verify_pair "$MS" "$DEST/libsprout-core-musl.so" 2>/dev/null || fails="$fails libsprout-core-musl.so"; }
if [ -n "$fails" ]; then echo "install.sh: POST-INSTALL HASH CHECK FAILED:$fails" >&2; exit 1; fi
echo "installed sprout + libsprout-core.so + sprout-super ${MS:++ libsprout-core-musl.so} -> $DEST (hashes verified)"
echo "verify:  $DEST/sprout --version && $DEST/sprout -r <rootfs> /bin/echo SPROUT-OK"
