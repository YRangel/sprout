# Installation

## From a release (recommended)

Download the release tarball from GitHub Releases. It contains three files
that must stay side-by-side:

```
sprout              # launcher CLI (Rust)
libsprout-core.so   # LD_PRELOAD interposer (glibc-linked)
sprout-ptrace       # supervisor (static binary fallback)
```

sprout discovers `libsprout-core.so` and `sprout-ptrace` *next to the
`sprout` binary* (sibling-of-argv[0] rule). Install with:

```sh
./install.sh                 # into ${PREFIX:-$HOME/.local}/bin
# or explicitly:
./install.sh /data/data/com.termux/files/usr/bin <path-to-extracted-release>
```

Verify:

```sh
sprout --version
sprout -r /path/to/rootfs /bin/echo SPROUT-OK
```

Overrides: `SPROUT_PRELOAD_PATH` and `SPROUT_PTRACE_PATH` point at
non-sibling artifacts explicitly (useful for development).

## From source

On Termux the interposer is a glibc-linked ELF and therefore **cannot** be
produced by the host's bionic toolchain; build it inside a glibc guest
(proot-distro works fine):

```sh
git clone <repo> && cd sprout
cargo build --release --workspace          # produces sprout + supervisor
proot-distro login debian --work-dir $PWD -- /usr/bin/sh -c \
  'gcc -std=c11 -O3 -flto -Wall -Wextra -Wpedantic -D_GNU_SOURCE -DSPROUT_INTERPOSE \
     -fPIC -shared -fvisibility=default -o target/libsprout-core.so \
     crates/sprout-preload/csrc/sprout_preload.c -ldl'
./install.sh                               # picks release artifacts
```

CI builds the exact same artifacts on `ubuntu-24.04-arm`; local musl /
bionic hosts must always use the in-guest route (see `development.md`).

## Where files live at runtime

| path | purpose |
|------|---------|
| `$HOME/.cache/sprout/*-sanitized-<hash>.so` | cached sanitized ld.so/libc copies (ADR-0007), content-hash keyed, rebuilt automatically |

No other global state; uninstall = delete the three installed files +
`~/.cache/sprout`.
