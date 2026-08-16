# Installation

## From a release (recommended)

Download the **Termux host tarball** —
`sprout-termux-host-aarch64.tar.xz` from
[GitHub Releases](https://github.com/YRangel/sprout/releases) — plus
`SHA256SUMS`. The tarball is self-contained:

```
sprout                  # launcher CLI (Rust, bionic-linked)
sprout-super            # supervisor (static-binary fallback supervisor; sprout-ptrace symlink is installed alongside)
sprout-stub             # stub for the ptrace supervision lane
install.sh              # installer / verifier
libsprout-core.so       # LD_PRELOAD interposer (glibc-linked)
libsprout-core-musl.so  # LD_PRELOAD interposer (musl-linked, musl guests like Alpine)
```

sprout discovers `libsprout-core.so` and `sprout-super` *next to the
`sprout` binary* (sibling-of-argv[0] rule), so an extracted copy runs
without installing too. Full install:

```sh
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/sprout-termux-host-aarch64.tar.xz
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/SHA256SUMS
sha256sum -c SHA256SUMS --ignore-missing
tar -xJf sprout-termux-host-aarch64.tar.xz
cd "$(dirname "$(find . -maxdepth 2 -name sprout -type f | head -1)")"
./install.sh                 # into ${PREFIX:-$HOME/.local}/bin
# or explicitly:
./install.sh /data/data/com.termux/files/usr/bin "$(pwd)"
```

Verify:

```sh
sprout --version                   # reports the release version (e.g. sprout 0.4.0)
sprout -r /path/to/rootfs /bin/echo SPROUT-OK
```

Overrides: `SPROUT_PRELOAD_PATH` and `SPROUT_PTRACE_PATH` point at
non-sibling artifacts explicitly (useful for development).

> The release also ships `sprout-guest-interposers-aarch64.tar.xz`:
> only the two interposer DSOs (CI-built), for people building the host
> binaries from source on-device (below). It contains **no** `sprout`
> launcher and **no** `install.sh`.

## From source

On Termux the interposer is a glibc-linked ELF and therefore **cannot** be
produced by the host's bionic toolchain; build it inside a glibc guest
(proot-distro works fine):

```sh
git clone <repo> && cd sprout
cargo build --release --workspace          # produces sprout + supervisor + stub
proot-distro login debian --work-dir $PWD -- /usr/bin/sh -c \
  'gcc -std=c11 -O3 -flto -Wall -Wextra -Wpedantic -D_GNU_SOURCE -DSPROUT_INTERPOSE \
     -fPIC -shared -fvisibility=default -o target/libsprout-core.so \
     crates/sprout-preload/csrc/sprout_preload.c -ldl'
# musl flavor (same guest, needs: apt install musl-tools):
proot-distro login debian --work-dir $PWD -- /usr/bin/sh -c \
  'musl-gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -DSPROUT_INTERPOSE \
     -fPIC -shared -o target/libsprout-core-musl.so \
     crates/sprout-preload/csrc/sprout_preload.c -ldl'
./install.sh                               # picks release artifacts, verifies hashes
```

The release workflow builds the exact same interposer artifacts on
`ubuntu-24.04-arm` and ships them as release downloads; local musl / bionic
hosts must always use the in-guest route (see `development.md`).

## Where files live at runtime

| path | purpose |
|------|---------|
| `$HOME/.cache/sprout/*-sanitized-<hash>.so` | cached sanitized ld.so/libc copies (ADR-0007), content-hash keyed, rebuilt automatically |

No other global state; uninstall = delete the installed files from
`$PREFIX/bin` (sprout, sprout-super, sprout-ptrace symlink, sprout-stub,
libsprout-core.so, libsprout-core-musl.so) and optionally the cache dir.
