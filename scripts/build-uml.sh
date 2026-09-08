#!/data/data/com.termux/files/usr/bin/bash
# build-uml.sh — reproducible x86_64 UML kernel build for the sprout sidecar.
# Runs INSIDE the debian rootfs via sprout (needs the x86_64 kbuild toolchain).
# Pinned to v6.16; the fragment is validated against that tree's Kconfig.
#
# Usage (from repo root):
#   sprout -r ~/roots/debian -b ~/uml-kernel:/uml-kernel -- \
#       /bin/sh /uml-kernel/build-uml.sh
#   # or locally: scripts/build-uml.sh  (expects /uml-kernel bind or $UML_SRC)
#
# Output: $UML_SRC/linux-6.16/linux  (the UML guest binary)
set -eu
UML_SRC="${UML_SRC:-/uml-kernel/linux-6.16}"
FRAG="${FRAG:-$UML_SRC/sprout-uml.fragment}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"
if [ ! -d "$UML_SRC" ]; then
    echo "build-uml: no source dir $UML_SRC (bind it: -b ~/uml-kernel:/uml-kernel)" >&2
    exit 1
fi
if [ ! -f "$FRAG" ]; then
    echo "build-uml: no fragment $FRAG" >&2
    exit 1
fi
cd "$UML_SRC"
echo "=== build-uml: defconfig (ARCH=um SUBARCH=x86_64) ==="
make ARCH=um SUBARCH=x86_64 x86_64_defconfig
echo "=== build-uml: merge fragment ==="
./scripts/kconfig/merge_config.sh .config "$FRAG"
make ARCH=um SUBARCH=x86_64 olddefconfig
echo "=== build-uml: build (-j$JOBS, ~10-20 min on 8 cores) ==="
make ARCH=um SUBARCH=x86_64 -j"$JOBS" linux
echo "=== build-uml: done ==="
ls -la linux
./linux --help 2>&1 | head -5 || true
