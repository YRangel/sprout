#!/bin/sh
# install.sh — install sprout locally (Termux-first).
#
# Copies the three runtime artifacts side-by-side into ${PREFIX:-$HOME/.local}/bin
# so sprout's sibling-of-argv[0] discovery finds them:
#   sprout            — Rust launcher (CLI)
#   libsprout-core.so — C interposer (glibc-linked; must come from CI artifact
#                       or an in-guest build, NOT from a bionic host build)
#   sprout-ptrace     — supervisor (static-binary fallback)
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
    for d in "$SRC"/target/release/build/*/out "$SRC"/target/debug/build/*/out "$SRC"/target; do
        if [ -f "$d/$1" ]; then echo "$d/$1"; return; fi
    done
    echo ""
}

SP=$(pick sprout bin)
SO=$(pick libsprout-core.so)
PX=$(pick sprout-ptrace)

missing=""
[ -n "$SP" ] || missing="$missing sprout"
[ -n "$SO" ] || missing="$missing libsprout-core.so"
[ -n "$PX" ] || missing="$missing sprout-ptrace"
if [ -n "$missing" ]; then
    echo "install.sh: missing artifacts:$missing" >&2
    echo "  build with: cargo build --release --workspace && rebuild the interposer in a guest (see docs/src/development.md)," >&2
    echo "  or download the release tarball from GitHub." >&2
    exit 1
fi

mkdir -p "$DEST"
cp "$SP" "$DEST/sprout"
cp "$SO" "$DEST/libsprout-core.so"
cp "$PX" "$DEST/sprout-ptrace"
chmod 755 "$DEST/sprout" "$DEST/sprout-ptrace"
echo "installed sprout + libsprout-core.so + sprout-ptrace -> $DEST"
echo "verify:  $DEST/sprout --version && $DEST/sprout -r <rootfs> /bin/echo SPROUT-OK"
