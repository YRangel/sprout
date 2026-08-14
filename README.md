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

```sh
git clone https://github.com/YRangel/sprout.git
cd sprout
cargo build --release          # needs: pkg install rust (pinned toolchain)
./install.sh --verify          # places sprout into $PREFIX/bin, verifies hashes
```

### Faster install: prebuilt guest interposers

Tagged releases ship the two **guest-side** interposer DSOs from CI
(`libsprout-core.so` + `libsprout-core-musl.so`; SHA256SUMS + tarball).
The **host-side** binaries (`sprout`, `sprout-super`) link against Android's
bionic and cannot be produced by GitHub-hosted runners — every cross route
(cargo-zigbuild abi suffix, setup-android sdkmanager on cmdline-tools 16.0,
cross-rs x86 container under qemu) failed structurally; the intended
production lane is a self-hosted Termux runner (`.github/workflows/
termux-selfhosted.yml`). Until then: `cargo build --release` on the device
is the launcher install path.

```sh
# optional: prebuilt guest interposers (then cargo build only covers host .so)
cd $PREFIX/tmp
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/sprout-guest-interposers-aarch64.tar.xz
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/SHA256SUMS
tar -xf sprout-guest-interposers-aarch64.tar.xz
sha256sum --ignore-missing -c SHA256SUMS
cp libsprout-core.so libsprout-core-musl.so $PREFIX/bin/
# then:
cd sprout && cargo build --release && ./install.sh --verify
```

Then run anything:

```sh
# interactive shell in the debian rootfs
sprout -r $PREFIX/var/lib/proot-distro/containers/debian /bin/sh

# GUI desktop over termux-x11 (DISPLAY/PULSE_SERVER preset)
sprout -r $PREFIX/var/lib/proot-distro/containers/debian --shared-tmp --termux-x11 startxfce4
```

Desktop-capable today: xfce4-session, firefox-esr, Thunar, etc. See
`QUICKSTART.md` and `docs/src/guide/x11-gpu.md`.

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
  `SPROUT_BINFMT_I386`, or `-q PATH`); `SPROUT_BINFMT_ALWAYS=1` wraps every
  exec proot-`-q`-style for whole-rootfs emulation.
- **SysV IPC, userspace-emulated (Android has none)**: for native guests,
  `libsprout-core.so` interposes `shmget/shmat/shmdt/shmctl` using the termux
  libandroid-shmem wire contract (ashmem segments + `/dev/shm/<sockid>`
  SCM_RIGHTS fd-hydration; ADR-0020) — that makes mesa's XShm present path
  work, un-freezing llvmpipe Vulkan/GL under termux-x11. For x86+binfmt
  guests (steam's live runtime), a guest-ABI `libsprout-sysvipc.so` injected
  into `BOX64_LD_PRELOAD` additionally emulates `semget/semop/semctl/shm*`
  (ADR-0018). Both disable with `SPROUT_SYSVIPC_OFF=1`.
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
