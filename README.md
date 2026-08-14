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
- **Benchmarks**: `docs/src/benchmarks.md` (median-of-N + hyperfine crosscheck)
- **GitHub**: <https://github.com/YRangel/sprout>

## Quick start

**Prerequisites**: Termux on Android (aarch64), a guest rootfs (e.g.
`pkg install proot-distro && proot-distro install debian`).

```sh
git clone https://github.com/YRangel/sprout.git
cd sprout
./install.sh --verify          # places sprout into $PREFIX/bin, verifies hashes
                                  # (or: cargo build --release && ./install.sh --verify)
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
  --termux-x11 --kill-on-exit --dry-run`.
- **-q / x86 emulation without root**: userspace binfmt adapter (ADR-0017)
  sniffs x86_64/i386 ELFs at exec and routes them through a guest emulator
  (`/usr/local/bin/box64` by default; set `SPROUT_BINFMT_X86_64` /
  `SPROUT_BINFMT_I386`, or `-q PATH`); `SPROUT_BINFMT_ALWAYS=1` wraps every
  exec proot-`-q`-style for whole-rootfs emulation.
- **SysV IPC for x86+binfmt guests** (steam's live runtime): guest-ABI
  `libsprout-sysvipc.so` is injected into `BOX64_LD_PRELOAD` of every wrapped
  exec, emulating `semget/semop/semctl/shm*` in userspace against a shared
  `/tmp/sprout-sysvipc` backing dir (ADR-0018). Needed because stock Android
  GKI ships `CONFIG_SYSVIPC=n`; disable with `SPROUT_SYSVIPC_OFF=1`.
- **Environment policy**: never invents vars; `HOME` defaults guest when you ask
  to live inside; `--termux-x11` is the explicit preset for GUI sessions.
- **Reproducible batteries**: `bench/flags-matrix.sh` (25 cells),
  `bench/flags-matrix-extended.sh` (73), hyperfine benchmark pairs.
- **Audited surface**: every major decision written down in `docs/src/adr/`.

## Found a bug?

Issues welcome: <https://github.com/YRangel/sprout/issues>. If sprout can be
observably falsified by a workload that worked under proot, the harness to
prove it is usually `bench/run-statics.sh` or `bench/run-hyperfine.sh`.

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

## Status

Post-v0.1: glibc guest shells, static/Go binaries, musl, apt/apk, X11 desktop
and Firefox ESR all work on-device; performance beats proot-distro on every
published cell. See **Roadmap** in `docs/src/roadmap.md` for what's next.

## License

Dual-licensed under MIT OR Apache-2.0, at your option.
