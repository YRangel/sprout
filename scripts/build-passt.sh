#!/bin/bash
# Build passt (rootless user-mode networking) for Android/Termux (ADR-0025 D6).
# Clones upstream passt, applies patches/passt-android.patch (bionic struct
# guards, resolv.h fallbacks, vring_need_event, netlink/userns/setid/
# namespace/mount degradations for untrusted_app), installs next to sprout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SPROUT_PASST_WORK:-$HOME/passt-build}"
SRC="$WORK/passt"
mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 https://passt.top/passt/ "$SRC"
fi
cd "$SRC"
if ! git diff --quiet 2>/dev/null; then
    echo "note: $SRC has local modifications; NOT re-applying the patch" >&2
else
    git apply "$ROOT/patches/passt-android.patch"
fi
make -j"$(nproc)"
DEST="$ROOT/target/debug"
mkdir -p "$DEST"
cp "$SRC/passt" "$DEST/passt"
echo "passt installed to $DEST/passt"
