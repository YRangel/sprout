# Quickstart (Termux, on-device)

## Install

Prebuilt (recommended):

```
cd "$(mktemp -d)" && \
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/sprout-termux-host-aarch64.tar.xz && \
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/SHA256SUMS && \
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

## Unpack a downloaded rootfs (proot `--link2symlink tar` parity)

Rootless, no proot required — sprout's `upkg` subcommand extracts any
tar.gz / tar.xz / tar.bz2 / plain tar with SELinux repair (hardlinks
materialized as full-content copies, setuid/setgid stripped, device nodes
skipped with a count):


curl -LO https://example.org/distro/aarch64/rootfs.tar.xz
sprout upkg rootfs.tar.xz -C ~/myrootfs
sprout -r ~/myrootfs --user=0:0 -- bash    # boots immediately


Tested: fresh 90 MiB debian rootfs extracts in ~8s on Termux device and
boots without further fixup.

## Packages work inside guests

The apt / apk cycle (download, verification via sqv, unpack, post-install
scripts) runs rootless with no extra flags:

```
sprout -r $B /usr/bin/apt-get update
sprout -r $B -0 --link2symlink /usr/bin/apt-get install -y nodejs
sprout -r $A /sbin/apk add nodejs
sprout -r $B /usr/bin/curl -fsSL -o /tmp/x https://example.com/file
```

Need to build the rootfs from a tarball first? `sprout upkg` handles it.

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
