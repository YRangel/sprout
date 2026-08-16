# Quickstart (Termux, on-device)

## Install

Prebuilt (recommended):

```
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/sprout-termux-host-aarch64.tar.xz
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/SHA256SUMS
sha256sum -c SHA256SUMS --ignore-missing && tar -xJf sprout-termux-host-aarch64.tar.xz && ./install.sh --verify
```

From source:

```
pkg install rust git
git clone https://github.com/YRangel/sprout.git && cd sprout
cargo build --release --workspace
bash install.sh                    # $PREFIX/bin: sprout + interposer + supervisor
```

## Run

```
B=$PREFIX/var/lib/proot-distro/containers/debian/rootfs   # or any glibc rootfs
A=$PREFIX/var/lib/proot-distro/containers/alpine/rootfs   # or any musl rootfs

sprout -r $B /bin/echo hello
sprout -r $B -w /root /bin/bash                    # interactive shell
sprout -r $B -b $PREFIX/tmp/x:/mnt /bin/cat /mnt/f # binds, proot-style
sprout -r $B --shared-tmp --termux-x11 /usr/bin/xterm # X11+audio preset
sprout -r $A /bin/busybox sh -c 'ls /etc/apk'      # musl automatic (supervisor)
sprout -r $B ./static-binary; echo $?              # exit codes pass through
```

Debug the launch without running: `--dry-run`. Trace: `-v`.
Force supervisor: `--fallback ptrace` (for testing).

## Packages work inside guests

The apt / apk cycle (download, verification via sqv, unpack, post-install
scripts) runs rootless with no extra flags:

```
sprout -r $B /usr/bin/apt-get update
sprout -r $B -0 --link2symlink /usr/bin/apt-get install -y nodejs
sprout -r $A /sbin/apk add nodejs
sprout -r $B /usr/bin/curl -fsSL -o /tmp/x https://example.com/file
```

Verified: cowsay run, node--binaries execute, Go run, static / musl /
Go-static / Go-dynamic binaries, X11 (termux-x11) handshake, cloudflared
static binary executing, and full vkmark 2025.01 suites on both Adreno-840
Turnip (native; ≈10.4× proot-distro) and llvmpipe (≈1.8×) — see
`bench/run-vkmark.sh`. On devices whose SELinux blocks `/dev/kgsl-3d0`,
GPU access gates/fallbacks keep behaving exactly like proot-distro.

Semantics match proot (`-r -b -w -0 --link2symlink`) plus `--shared-tmp`,
`--termux-x11` (DISPLAY/PULSE preset — sprout itself never invents those
exports; without the flag it only inherits what you set)
parity with proot-distro login. See docs/src/guide/.
