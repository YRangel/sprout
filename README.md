# sprout

**Rootless glibc/musl Linux userspace for Android. Fast, auditable, open.**

`sprout` runs full Linux userspaces — shells, `apt`/`apk`, Python, Node,
gcc/rust toolchains, **XFCE4 desktops, Firefox ESR, LibreOffice** — on stock
Android, no root. It is a drop-in replacement for
[proot](https://proot-me.github.io)/[proot-distro](https://github.com/termux/proot-distro)
that replaces ptrace-every-syscall with an `LD_PRELOAD` fast path, keeping
proot's semantics everywhere it's observable.

MIT OR Apache-2.0. Every architectural decision documented in `docs/src/adr/`.

- **Docs**: `docs/src` (mdBook — `cd docs && mdbook build`)
- **Repo**: <https://github.com/YRangel/sprout>

---

## Table of contents

1. [Why sprout exists](#why-sprout-exists)
2. [How it works (30 seconds)](#how-it-works-30-seconds)
3. [Install](#install)
4. [Quick start](#quick-start)
5. [Command-line reference](#command-line-reference)
6. [Migrating from proot / proot-distro](#migrating-from-proot--proot-distro)
7. [Desktops & GPU (X11)](#desktops--gpu-x11)
8. [x86 apps under box64 (-q)](#x86-apps-under-box64--q)
9. [What sprout fakes for you](#what-sprout-fakes-for-you)
10. [Fundamental limits](#fundamental-limits)
11. [Performance](#performance)
12. [Status & support](#status--support)
13. [Project layout](#project-layout)
14. [License](#license)

---

## Why sprout exists

- **Performance.** proot pays ~2 context switches per guest syscall (ptrace
  freeze + re-arm). sprout's preload lane answers path translation at the
  PLT with zero traps, and uses seccomp-user-notify only where the kernel
  makes a cheap round trip possible. On a HyperOS/Android-16 device: git
  4–4.6×, `find /usr` 27×, `tar czf` 7×, statics 14–15× vs proot (see
  [docs/src/benchmarks.md](docs/src/benchmarks.md)).
- **Notable proot-era pain is gone by construction.** proot-distro
  [#567](https://github.com/termux/proot-distro/issues/567) ("extreme
  slowdown after Android 16, all desktop environments", Oct 2025 → live in
  2026) is a ptrace-class overhead spiral on newer GKI kernels — sprout's
  hot path doesn't pay it. proot [#122](https://github.com/termux/proot/issues/122)
  (statx unsupported) — sprout serves statx with an emulated
  answer when policy blocks the raw call, and fakes the HyperOS `EACCES`
  `/proc` table (the LibreOffice "ERROR: /proc not mounted" abort).
- **Auditable.** No binary patching of installed files (content-addressed
  derivatives in `~/.cache/sprout` only), ADRs for every design choice,
  C + Rust only for the two hot artifacts.

## How it works (30 seconds)

Three lanes, picked per ELF at exec:

1. **preload (fast)** — a glibc `LD_PRELOAD` interposer rewrites
   open/exec/stat/chdir/* paths from guest spelling to host-resolved ones.
   Zero ptrace traps.
2. **supervisor (ptrace)** — static or Go binaries can't carry the `.so`;
   a supervisor with `seccomp-user-notify` translates at syscall level,
   answering fakes (`/proc/*`, statx, overflows) inline when it can.
3. **`sprout-stub` (pure notify)** — when the kernel is new enough for the
   whole fast path from userspace alone.

Details: [docs/src/architecture](docs/src/architecture/).

---

## Install

**Prerequisites**: Termux on Android (aarch64), a guest rootfs (e.g.
`pkg install proot-distro && proot-distro install debian`).

### Prebuilt tarball (recommended)

```sh
cd "$(mktemp -d)"  # empty dir: same-named old downloads make
                    # sha256sum --ignore-missing legitimately fail
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/sprout-termux-host-aarch64.tar.xz
curl -sLO https://github.com/YRangel/sprout/releases/latest/download/SHA256SUMS
sha256sum -c SHA256SUMS --ignore-missing   # must print: ... tar.xz: OK
tar -xJf sprout-termux-host-aarch64.tar.xz
./install.sh --verify          # installs into ${PREFIX:-$HOME/.local}/bin + verifies hashes
```

### From source

```sh
git clone https://github.com/YRangel/sprout.git
cd sprout
cargo build --release --workspace   # needs: pkg install rust
./install.sh --verify
```

Requires a glibc guest for the `.so` subprocess stage (`glibc gcc` inside
the guest is enough — no toolchain on the host needed).

---

## Quick start

```sh
# interactive shell in a debian rootfs
D=$PREFIX/var/lib/proot-distro/containers/debian/rootfs    # or ~/roots/debian
sprout -r "$D" --user=0:0 -- /bin/bash -l

# full GUI desktop over termux-x11 (DISPLAY + PULSE_SERVER preset)
sprout -r "$D" --shared-tmp --termux-x11 --user=0:0 -- startxfce4

# run an x86_64 binary under box64 (guest box64 installed via apt)
sprout -r "$D" -q /usr/bin/box64 -- ./my-x86-program

# plan-only, no execution (debugging)
sprout -r "$D" --dry-run --user=0:0 -- /bin/echo hi
```

**Rule #1 — sprout options go BEFORE the guest command.** clap now
hard-errors on unknown `-`/`--` tokens instead of silently feeding them to
the guest (friend-reported; was the worst of the old UX bugs). Separate
with `--` when the guest command itself starts with a dash or as a habit:

```sh
sprout -r "$D" --user=0:0 -- /bin/sh -c 'ls -la /root'
```

---

## Command-line reference

### Core

| flag | what it does |
|---|---|
| `-r, --rootfs PATH` | guest root directory (the fake chroot) — REQUIRED |
| `-w, --cwd DIR` | working directory inside the guest |
| `-b, --bind H[:G]` | bind a host path into the guest (repeatable) |
| `COMMAND [ARGS...]` | program + args, guest-spelled (PATH-searched inside the rootfs) |

### Identity

| `-u, --user USER[:GROUP]` | fake uid/gid to a named guest user (implies fakeroot at that anchor) |
| `-i, --change-id USER[:GROUP]` | proot compat alias of `--user` |
| `-0, --root-id` | fake uid/gid 0 — DEFAULT |
| `--no-fakeroot` | kernel-truthful identities (mostly EPERM for privileged ops) |
| `--host-home` | pass host `$HOME` into the guest |
| `--host-path` | append `$PREFIX/bin` to the guest PATH |

### Rootfs & layout

| `--shared-tmp` | bind host `$PREFIX/tmp` at guest `/tmp` (X11/audio/ssh-agent sockets carry) |
| `--link2symlink` / `--no-link2symlink` | hardlink→symlink fallback for SELinux (DEFAULT ON) |
| `--kill-on-exit` | kill launched processes when the command exits |

### Desktop

| `--termux-x11` | export `DISPLAY=:0` + `PULSE_SERVER=127.0.0.1` preset (best with `--shared-tmp`) |

### Networking

| `-p / -P, --port-mapping, --redirect-ports, --fix-low-ports` | bind(2) on ports <1024 hops to port+1024 (all spellings = one knob) |

### Emulation

| `-q, --qemu PATH` | route x86_64 (and i386 via box32) ELF execs through this emulator binary; PATH is resolved **guest-first, host-second** — termux-native bionic box64/qemu-user outside the rootfs is detected and direct-spawned |
| `-k, --kernel-release R` | what `uname(2)` reports to guests |
| `-L, --loader-fix` | proot compat: obsolete, accepted with a note |
| `--mixed-syscall` | proot compat: no-op note |
| `--sysvipc`, `--ashmem-memfd` | accepted compat no-ops (always-on equivalents; the knobs are `SPROUT_SYSVIPC_OFF=1` etc.) |

### Debugging

| `-v, --verbose [LEVEL]` | path-translation tracing |
| `--fallback preload\|ptrace` | force the interception strategy |
| `--dry-run` | print the resolved launch plan without executing |

### Env knobs

| var | effect |
|---|---|
| `SPROUT_PRELOAD_PATH`, `SPROOT_PTRACE_PATH` | override paths for dev A/B (`*.so` gloss-over itch: a missing override falls back silently) |
| `SPROUT_CACHE_DIR` | pin the derivative/param cache root (default `~/.cache/sprout`) |
| `SPROUT_BINFMT_X86_64` / `_I386` / `_ALWAYS=1` | binfmt lanes per arch; ALWAYS wraps even native aarch64 |
| `SPROUT_SYSVIPC_OFF=1`, `SPROUT_SYSVIPC_EMU_OFF=1` | disable shims (full or emulator-only) |
| `MOZ_DISABLE_WASM_SIGHANDLERS=1` | required for Firefox ESR under sprout (wasm callee-saved regs ABI mismatch class, documented) |

---

## Migrating from proot / proot-distro

| proot habit | sprout equivalent | status |
|---|---|---|
| `proot -0 -r R -b /dev -b /proc bash` | `sprout -r R --user=0:0 -- bash -l` (+`-b` binds as needed) | native |
| `proot-distro login debian` | `sprout -r $PREFIX/var/lib/proot-distro/containers/debian/rootfs --user=0:0 -- /bin/bash -l` | native |
| `proot-distro login debian --shared-tmp` | add `--shared-tmp --termux-x11` | native |
| `proot -q qemu-aarch64 cmd` | `sprout -q /usr/bin/box64 -- cmd` | native (+host-bionic detection) |
| `proot -i 0` / `--change-id 0:0` | `-i/--change-id` | alias |
| `proot -k 6.6 cmd` | `-k/--kernel-release` | native |
| `proot -p` / `--redirect-ports` | `-p/--port-mapping` (all 4 spellings) | native |
| `proot -L`, `--mixed-syscall`, `--sysvipc`, `--ashmem-memfd` | accepted as no-ops with notes (sprout's equivalents are structural/always-on) | compat-no-op |

---

## Desktops & GPU (X11)

XFCE4 on the debian guest is the reference desktop (validated daily on the
dev machines). Minimum envelope:

```sh
am start -S   # native termux-x11 app first (or it'll swallow the display)
sprout -r "$D" --shared-tmp --termux-x11 --user=0:0 -- startxfce4
```

- **GPU**: Adreno hardware GL/Vulkan via turnip/zink Mesa — install the
  lfdevs android-container Mesa build inside the guest and pin
  `VK_ICD_FILENAMES` (guide: [docs/src/guide/x11-gpu.md](docs/src/guide/x11-gpu.md)).
- **Zombie X displays**: `x11-rescue.sh` (shell) rescues a dead-display
  socket/lock pair; never `rm` an `.X11-unix/XN` file whose server thread
  is alive.
- **Audio**: pulse+AAudio TCP server on the host, guest targets it at
  `PULSE_SERVER=127.0.0.1`; `pulse-guard.sh` watchdog heals HyperOS pulse
  rot automatically.

## x86 apps under box64 (-q)

- Install box64 **inside the guest** (`apt install box64` in debian) and
  point: `sprout -r $D -q /usr/bin/box64 -- ./x86-app`.
- Host-Termux box64 also works via the same path (bionic emulator detected
  and direct-spawned), but two old walls stay: static x86 guests die
  against Android's `set_robust_list` policy block, and dynamic ones need
  the rootfs's `ld-linux-x86-64.so.2` discoverable. Guest-installed box64
  is the lane that fully works today.
  Details: [docs/src/adr/0017](docs/src/adr/0017-userspace-binfmt-adapter.md).
- Env knobs: `SPROUT_BINFMT_X86_64`, `SPROUT_BINFMT_I386`,
  `SPROUT_BINFMT_ALWAYS=1`, `BOX64_LD_PRELOAD` forwarding for arch-shim
  injection (`/usr/lib/sprout-sysvipc/x86_64/...`).

## What sprout fakes for you

`/proc` (HyperOS policy hides it from untrusted uids):
`/proc/stat`, `/proc/loadavg`, `/proc/version`, `/proc/uptime`,
`/proc/sys/kernel/overflowuid`, `/proc/sys/kernel/overflowgid` — served as
parseable content at every enforcement level (`LD_PRELOAD` PLT, notify
ADDFD, classic scratch-file, memfd, materialized-file reroute). This is
what unbreaks LibreOffice (proot-reagent for that class: issue
[termux/proot#175](https://github.com/termux/proot/issues/175)).

`statx(2)` — answered from `newfstatat(262)` when the raw call is policy-
degraded/`ENOSYS` off the stack; `stx_mask` advertises exactly the fields
populated (no btime on pre-4.16 kernels).

Identity (uid/gid/groups/env), `/etc/{hostname,resolv.conf}` housework,
hardlink→symlink translation for SELinux paths, SysV IPC in userspace.

## Fundamental limits

Not bugs, walls every rootless runner shares:

- **The Android filter is the final word.** `RET_KILL`-policy syscalls
  (e.g. raw `riscv`/`sys_pidfd-good-timing` classes on some devices)
  out-rank any refusal answer from us. Only PLT interception (preload lane,
  i.e. *glibc-dynamic guests*) can be rescued; raw `svc` callers die exactly
  as under proot.
- **No nested ptrace debuggers.** `gdb/strace -p` INSIDE a guest is
  impossible by design (the supervisor owns ptrace).
- **No mount(2)/loop devices/namespaces.** Kernel-level: apps that require
  them (docker, some systemd apps) are out of this model's reach.
- **64-bit only** (aarch64 host + 64-bit guests; 32-bit via box32 in the
  emulation lane only).

## Performance

Measured on-device, pre-warm cache; `×` = sprout/proot speedup:

| workload | × |
|---|---|
| `git status` / `git log` (dwarfs repo) | 4.1–4.6× |
| `find /usr -type f` | 27.5× |
| `tar czf` on /usr | 7.1× |
| statics lane (`run-statics.sh`) | up to 15.3× |
| spawn-heavy traversal (notify ± ptrace) | ~1.1× (spam-class bound) |

Full tables + methodologies: [docs/src/benchmarks.md](docs/src/benchmarks.md),
including `bench/run{,-statics,-hyperfine,-alpine,-vkmark}.sh` harnesses.

## Status & support

- **Works daily on the dev machines**: Debian trixie guest w/ XFCE4,
  Firefox ESR, LibreOffice, flatpak. POCO X3-class 4.14 kernel phone
  validates the classic ptrace lane every tag.
- **Not a sandbox**: [docs/src/architecture/threat-model.md](docs/src/architecture/threat-model.md).
- **Issues**: <https://github.com/YRangel/sprout/issues>. First diagnostic:
  pair the failure against the `proot` control lane (`proot-control.sh`)
  — identical-under-proot = app broken, not sprout.
- **Batteries**: `cargo test` + `test_translate` + `bench/*` +
  `~/projeto/flagmatrix.sh` + `~/projeto/healthcheck.sh` (3-layer gate).

## Project layout

```
sprout/                 # launcher (rust, spawns the supervisor inside the guest)
├── crates/
│   ├── sprout-cli/     # CLI + plan assembly
│   ├── sprout-core/    # rootfs, translate, upsert, sanitization, ELF classes
│   ├── sprout-preload/ # glibc .so interposer (crates/sprout-preload/csrc)
│   └── sprout-ptrace/  # supervisor (notify-loop, classic ptrace, stub)
├── csrc/sprout-sysvipc/
├── bench/              # all numbers: harnesses + raw results
├── docs/src/           # mdBook: guide/architecture/ADR
├── install.sh
└── .github/workflows/  # self-hosted termux CI
```

## License

MIT OR Apache-2.0, at your option. `LICENSE-MIT` and `LICENSE-APACHE`.
