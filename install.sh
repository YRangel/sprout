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
# Usage: ./install.sh [dest-dir] [source-dir]
set -eu

DEST=${1:-${PREFIX:-$HOME/.local}/bin}
SRC=${2:-$(cd "$(dirname "$0")" && pwd)}

pick() {
    # $1 = filename to locate under $SRC
    case ${2:-} in
        bin) if [ -f "$SRC/target/release/$1" ]; then echo "$SRC/target/release/$1"; return; fi
             if [ -f "$SRC/target/debug/$1" ]; then echo "$SRC/target/debug/$1"; return; fi ;;
    esac
    for d in "$SRC" "$SRC"/target/release/build/*/out "$SRC"/target/debug/build/*/out "$SRC"/target; do
        if [ -f "$d/$1" ]; then echo "$d/$1"; return; fi
    done
    echo ""
}

SP=$(pick sprout bin)
MS=$(pick libsprout-core-musl.so)
SO=$(pick libsprout-core.so)
PX=$(pick sprout-super); [ -n "$PX" ] || PX=$(pick sprout-ptrace)
PS=$(pick sprout-stub)

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

mkdir -p "$DEST"
cp "$SP" "$DEST/sprout"
cp "$SO" "$DEST/libsprout-core.so"
cp "$PX" "$DEST/sprout-super"
ln -sf sprout-super "$DEST/sprout-ptrace"  # legacy name (fastfetch-style comm reads, scripts)
[ -n "$PS" ] && { cp "$PS" "$DEST/sprout-stub"; chmod 755 "$DEST/sprout-stub"; }
if [ -n "$MS" ]; then cp "$MS" "$DEST/libsprout-core-musl.so"; fi
chmod 755 "$DEST/sprout" "$DEST/sprout-super"
echo "installed sprout + libsprout-core.so + sprout-super ${MS:++ libsprout-core-musl.so} -> $DEST"
echo "verify:  $DEST/sprout --version && $DEST/sprout -r <rootfs> /bin/echo SPROUT-OK"
