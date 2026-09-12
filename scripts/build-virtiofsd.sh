#!/bin/bash
# Build virtiofsd for Android/Termux (ADR-0025 D5).
#
# Clones upstream virtiofsd (virtio-fs/virtiofsd on GitLab), applies
# patches/virtiofsd-android.patch (bionic binding shims, seccomp made
# optional, nr_open EACCES tolerance, preadv2/pwritev2 kernel-quirk
# routing — see the patch header comments), builds with cargo, and
# installs the binary next to the sprout CLI so `sprout uml up` finds it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SPROUT_VFSD_WORK:-$HOME/virtiofsd-build}"
SRC="$WORK/virtiofsd"

mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 https://gitlab.com/virtio-fs/virtiofsd.git "$SRC"
fi
cd "$SRC"
if ! git diff --quiet 2>/dev/null; then
    echo "note: $SRC has local modifications; NOT re-applying the patch" >&2
else
    git apply "$ROOT/patches/virtiofsd-android.patch"
fi
# seccomp feature is off: the bin is patched to build without it
# (bionic libc bindings are incomplete; the daemon runs unconfined
# same-uid anyway — see ADR-0025).
cargo build --release --no-default-features

DEST="$ROOT/target/debug"
mkdir -p "$DEST"
cp "$SRC/target/release/virtiofsd" "$DEST/virtiofsd"
echo "virtiofsd installed to $DEST/virtiofsd"
