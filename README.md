# sprout

**Rootless glibc/musl Linux userspace for Android. Fast, auditable, open.**

`sprout` runs full Linux userspaces — interactive shells, apt/apk, Python, Node,
toolchains, **XFCE4 desktops and Firefox ESR** — on Android without root. It is a
drop-in replacement for [proot](https://proot-me.github.io/)/proot-distro with an
`LD_PRELOAD` fast path that avoids ptrace syscall-stop overhead, plus automatic
fallback lanes for static/Go binaries (ptrace supervisor, or the pure-notify
`sprout-stub` when the kernel allows it).

MIT OR Apache-2.0. Every architectural decision documented in ADRs.

- **Docs**: `docs/src` (mdBook — `cd docs && mdbook build`)
- **Benchmarks**: `docs/src/benchmarks.md` (vkmark GPU/CPU suites + syscall hyperfine pairs)
- **GitHub**: <https://github.com/YRangel/sprout>

## Quick start

**Prerequisites**: Termux on Android (aarch64), a guest rootfs (e.g.
`pkg install proot-distro && proot-distro install debian`).

### Fastest: prebuilt release tarball

Tagged releases ship a self-contained Termux tarball (host launcher +
supervisor built on-device by the maintainer + both guest interposers +
installer):

```sh
cd "$(mktemp -d)"  # fresh dir: older downloads lying around with the SAME names
                   # (e.g. from a previous release) make sha256sum --ignore-missing
                   # legitimately fail — always checksum in an empty directory
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/sprout-termux-host-aarch64.tar.xz
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/SHA256SUMS
sha256sum -c SHA256SUMS --ignore-missing   # must print: sprout-termux-host-aarch64.tar.xz: OK
tar -xJf sprout-termux-host-aarch64.tar.xz
./install.sh --verify          # installs into ${PREFIX:-$HOME/.local}/bin, verifies hashes
```

### From source

```sh
git clone https://github.com/YRangel/sprout.git
cd sprout
cargo build --release --workspace  # needs: pkg install rust (pinned toolchain)
./install.sh --verify              # installs into $PREFIX/bin; fetches the two
                                   # guest interposer DSOs from the latest release
                                   # (sha256-verified) when absent from the tree
```

Tagged releases also ship the two **guest-side** interposer DSOs standalone
(`sprout-guest-interposers-aarch64.tar.xz`; no launcher, no installer) for
source builds that want to skip the in-guest interposer compile step
(`cargo build --release` alone covers only the host .so).

Then run anything:

```sh
# interactive shell in the debian rootfs
sprout -r $PREFIX/var/lib/proot-distro/containers/debian/rootfs /bin/sh

# GUI desktop over termux-x11 (DISPLAY/PULSE_SERVER preset)
sprout -r $PREFIX/var/lib/proot-distro/containers/debian/rootfs --shared-tmp --termux-x11 startxfce4
```

Desktop-capable today: xfce4-session, firefox-esr, Thunar, etc. See
`QUICKSTART.md` and `docs/src/guide/x11-gpu.md`.

## Command-line reference

Rule #1: **sprout options go BEFORE the guest command**. The guest command is
everything after the last option; use `--` when the command itself begins
with a dash or when you want to be explicit:

```sh
sprout -r ROOTFS --user=0:0 -- /bin/sh -c 'echo hello'
```

| flag | what it does |
|---|---|
| `-r, --rootfs PATH` | guest root directory (the fake chroot) — REQUIRED |
| `-w, --cwd DIR` | cwd inside the guest |
| `-b, --bind HOST[:GUEST]` | bind a host path into the guest (repeatable) |
| `COMMAND [ARGS...]` | program + args, guest-spelled (PATH-searched) |

### Identity
| `-u, --user USER[:GROUP]` | fake uid/gid to a named guest user (implies fakeroot at that anchor) |
| `-i, --change-id USER[:GROUP]` | proot compat alias of `--user` |
| `-0, --root-id` | fake uid/gid 0 — the DEFAULT |
| `--no-fakeroot` | kernel-truthful identities (mostly EPERM for privileged ops) |
| `--host-home`, `--host-path` | pass host $HOME / append $PREFIX/bin to guest PATH |

### Rootfs & layout
| `--shared-tmp` | bind host `$PREFIX/tmp` at guest `/tmp` (proot-distro parity; X11/audio/ssh-agent sockets transfer) |
| `--link2symlink` / `--no-link2symlink` | hardlink → symlink fallback for SELinux (DEFAULT ON) |
| `--kill-on-exit` | kill launched processes when the command exits (proot parity) |

### Desktop
| `--termux-x11` | preset DISPLAY=:0 + PULSE_SERVER=127.0.0.1 — combine with `--shared-tmp` |

### Networking
| `-p / -P, --port-mapping / --redirect-ports / --fix-low-ports` | bind(2) on ports <1024 hops to 1024+port (all four spellings are one knob) |

### Emulation & compat
| `-q, --qemu PATH` | wrap x86_64 (and i386 via box32) ELF execs through this emulator; PATH resolves guest-first, then host (bionic box64/qemu-user allowed — sprout detects bionic emulators and direct-spawns them) |
| `-k, --kernel-release RELEASE` | what `uname(2)` reports |
| `-L, --loader-fix` | proot compat: OBSOLETE — accepted with a note, does nothing (loader resolution is always correct) |
| `--mixed-syscall` | proot compat: no-op note (glibc wrappers are preload-native) |
| `--sysvipc`, `--ashmem-memfd` | acceptance no-ops (sprout's equivalents are always-on; knobs: `SPROUT_SYSVIPC_OFF=1` etc) |

### Debugging
| `-v, --verbose [LEVEL]` | path-translation tracing |
| `--fallback preload\|ptrace` | force a strategy (default auto) |
| `--dry-run` | print the launch plan without executing |

Unknown flags **fail loudly** with a clap "unexpected argument" hint —
they are never silently eaten into the guest argv (that was a real reported
confusing-UX bug class; fixed by removing trailing-hyphen capture).

### Migrating from proot / proot-distro

| proot/proot-distro habit | sprout equivalent | status |
|---|---|---|
| `proot -0 -r R -b /dev -b /proc bash` | `sprout -r R --user=0:0 -- bash` (+`-b` binds as needed) | native |
| `proot-distro login debian` | `sprout -r $PREFIX/var/lib/proot-distro/containers/debian/rootfs --user=0:0 -- /bin/bash` | native |
| `proot-distro login debian --shared-tmp` | add `--shared-tmp --termux-x11` | native |
| `proot -q qemu-aarch64 cmd` | `sprout -q /usr/bin/box64 -- cmd` | native (+host-bionic detectors) |
| `proot -k 6.6.0 cmd` | `-k/--kernel-release` | native |
| `proot -p` / `--fix-low-ports` | `-p/--port-mapping` (all spellings) | native |
| `proot -i 0` / `--change-id 0:0` | `-i/--change-id` (alias of `--user`) | alias |
| `proot -L` | `-L` accepted, note printed, no action | compat-no-op |
| `proot --mixed-syscall` | `--mixed-syscall` accepted as no-op | compat-no-op |
| `proot --sysvipc` / `--ashmem-memfd` | accepted; always-on in sprout | compat-no-op |

## Features

- **Three interception lanes, chosen at launch**: LD_PRELOAD (glibc, fastest),
  ptrace supervisor (static/Go/mixed trees), pure-notify stub (AArch64 statics).
- **Zero `.text` patching** of installed files; cached *derivative* ld.so/libc
  (in `~/.cache/sprout`) carry the Android seccomp workarounds.
- **proot-compatible CLI**: `-r -b -w -0 --link2symlink --shared-tmp
  --termux-x11 --kill-on-exit --dry-run`; ADR-0019 parity flags:
  `-k/--kernel-release` (uname release spoof), `-p/--port-mapping`
  (bind(2) ports <1024 -> 1024+p), `-v [LEVEL]`, `-V` (version+license
  banner), `-h` (usage), `--sysvipc` (accepted no-op: emulation always-on
  per ADR-0018 + ADR-0020), `--ashmem-memfd` (memfd_create fallback to
  /dev/ashmem + fstat st_size simulation).
- **-q / x86 emulation without root**: userspace binfmt adapter (ADR-0017)
  sniffs x86_64/i386 ELFs at exec and routes them through a guest emulator
  (`/usr/local/bin/box64` by default; set `SPROUT_BINFMT_X86_64` /
  `SPROUT_BINFMT_I386`, or `-q PATH`); the emulator may equally be a
  HOST-side absolute path (Termux-native qemu/box builds outside the
  rootfs) — guest resolution is tried first, the host path second.
  `SPROUT_BINFMT_ALWAYS=1` wraps every exec proot-`-q`-style for
  whole-rootfs emulation.
- **SysV IPC, userspace-emulated (Android has none)**: for native guests,
  `libsprout-core.so` interposes `shmget/shmat/shmdt/shmctl` using the termux
  libandroid-shmem wire contract (ashmem segments + `/dev/shm/<sockid>`
  SCM_RIGHTS fd-hydration; ADR-0020) — that makes mesa's XShm present path
  work, un-freezing llvmpipe Vulkan/GL under termux-x11. For x86+binfmt
  guests (steam's live runtime), a guest-ABI `libsprout-sysvipc.so` injected
  into `BOX64_LD_PRELOAD` additionally emulates `semget/semop/semctl/shm*`
  (ADR-0018). Both disable with `SPROUT_SYSVIPC_OFF=1`; the arm64-host
  emulator lane ({FEX,qemu-*,box64,box32} basenames) disables separately
  with `SPROUT_SYSVIPC_EMU_OFF=1`.
- **Environment policy**: never invents vars; `HOME` defaults guest when you ask
  to live inside; `--termux-x11` is the explicit preset for GUI sessions.
- **Reproducible batteries**: `bench/flags-matrix.sh` (34 cells),
  `bench/flags-matrix-extended.sh` (73), hyperfine benchmark pairs.
- **Audited surface**: every major decision written down in `docs/src/adr/`.

## Found a bug?

Issues welcome: <https://github.com/YRangel/sprout/issues>. If sprout can be
observably falsified by a workload that worked under proot, the harness to
prove it is usually `bench/run-statics.sh`, `bench/run-hyperfine.sh` or
(GUI/GPU regressions) `bench/run-vkmark.sh`.

## Project layout

```
crates/
  sprout-cli/     CLI + argv0 dispatch (Rust)
  sprout-core/    ELF classification, strategy, plan builder (Rust)
  sprout-preload/ C11 LD_PRELOAD interposer + GLIBC ABI capture (Rust + C)
  sprout-ptrace/  supervisor + statics stub (Rust + C)
bench/            benchmark suites + guarded matrices
docs/             mdBook — user guide, architecture, ADRs, benchmarks
install.sh        install/self-verify (<10 s)
```

## Benchmarks (vkmark 2025.01, 1280x720, default present mode, live X11)

| lane | sprout | proot-distro | delta |
|------|--------|--------------|-------|
| Adreno 840 (Turnip, real GPU) | **7883** | 755 | **10.4×** |
| llvmpipe (CPU / XShm)         | **339**  | 190 | **1.8×**  |

Repro: `bench/run-vkmark.sh`. Full per-scene tables + old hyperfine pairs
(4.88×–16.03× on syscall-heavy workloads) in `docs/src/benchmarks.md`.

## Status

Post-v0.1: glibc guest shells, static/Go binaries, musl, apt/apk, X11 desktop,
Firefox ESR and GPU-rendered Vulkan (Turnip native + llvmpipe via ADR-0020
sysv-shm emulation) all work on-device; performance beats proot-distro on
every published cell. See **Roadmap** in `docs/src/roadmap.md` for what's next.

## License

Dual-licensed under MIT OR Apache-2.0, at your option.
