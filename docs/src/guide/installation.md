# Installation

## From source (recommended while v0.1 is in flight)

```sh
pkg install rust clang binutils git   # inside Termux
git clone https://github.com/sprout-os/sprout
cd sprout
cargo install --path crates/sprout-cli
```

You also need `libsprout-core.so` built against a glibc aarch64 toolchain;
`cargo build` prints the path or fetch it from CI artifacts.

## Pick a guest rootfs

Any glibc aarch64 rootfs works — Ubuntu jammy+ and Debian bookworm+ are
the reference platforms. Easiest:

```sh
pkg install proot-distro
proot-distro install ubuntu
# now use the roootfs directory directly with sprout:
sprout -r /data/data/com.termux/files/usr/var/lib/proot-distro/installed-rootfs/ubuntu \
       -- /usr/bin/node --version
```

## Smoke test (no guest needed)

```sh
sprout -r "$PREFIX" --dry-run -- /bin/sh -c 'echo sprout-works'
# => prints the exact loader invocation sprout would run
```
