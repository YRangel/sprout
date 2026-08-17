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
cd "$(mktemp -d)"  # checksum in an EMPTY dir: stale files from older releases
                   # with the same names make -c legitimately report FAILED
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

```sh
git clone <repo> && cd sprout
cargo build --release --workspace   # produces sprout + supervisor + stub (glibc interposer skipped on bionic)
./install.sh --verify               # auto-fetches the two interposer DSOs from the latest
                                    # GitHub release (sha256-verified) when absent locally
```

If you want the interposer built *from your tree* (e.g. hacking on
`csrc/sprout_preload.c`), build it inside a glibc guest (proot-distro works):

```sh
proot-distro login debian --work-dir $PWD -- /usr/bin/sh -c \
  'gcc -std=c11 -O3 -flto -Wall -Wextra -Wpedantic -D_GNU_SOURCE -DSPROUT_INTERPOSE \
     -fPIC -shared -fvisibility=default -o target/libsprout-core.so \
     crates/sprout-preload/csrc/sprout_preload.c -ldl'
# musl flavor (same guest, needs: apt install musl-tools):
proot-distro login debian --work-dir $PWD -- /usr/bin/sh -c \
  'musl-gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -DSPROUT_INTERPOSE \
     -fPIC -shared -o target/libsprout-core-musl.so \
     crates/sprout-preload/csrc/sprout_preload.c -ldl'
./install.sh --verify               # picks tree artifacts first, verifies hashes
```

> **Trap:** `proot-distro login` appends **Termux's own `$PREFIX/bin`** to the
> guest PATH — inside the guest, `cargo`/`rustc` resolve to the *bionic* host
> toolchain unless you installed `rust` in the guest (`apt install cargo`).
> That is why a "guest" cargo build prints the same `skipping
> libsprout-core.so on Android host` warning: the target is still android. The
> `gcc` one-liners above are unaffected (they use the guest's `/usr/bin/gcc`).
> Prefer guest-native cargo (`apt install cargo rustc`), or simply let
> `./install.sh` fetch the prebuilt interposers.

### install.sh artifact-selection rules (v0.4.5+)

`install.sh` picks each artifact from the first available slot and refuses
stale candidates:

| slot tried (in order) | rule |
|---|---|
| `target/{release,debug}/<file>` (binaries) | always trusted — cargo wrote it |
| `target/*/build/*/out/<file>` (crates' build.rs exports) | newest slot wins, but **rejected when its mtime is older than the newest file in `crates/<crate>/csrc/`** — stale out/ slots used to silently ship pre-fix bytes (deploy-discipline law) |
| `$SRC/<file>`, `$SRC/target/<file>` (loose) | **ignored inside a git checkout** (tarball use only); override with `SPROUT_ALLOW_LOOSE=1` for dev rehearsals |

When no acceptable slot exists, the glibc/musl interposer pair is **fetched
from the latest GitHub release, sha256-verified against the release's own
SHA256SUMS**; `--verify` then re-checks every installed file's md5 and prints
a self-test command.

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
