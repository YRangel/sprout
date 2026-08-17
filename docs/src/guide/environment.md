# Environment policy inside a sprout guest

What sprout **sets**, **injects**, **wipes**, and **passes through** when
building a guest process's environment. Read this if a tool inside sprout
behaves differently than on bare Debian/Alpine — nine times out of ten it
is one of these rules moving a variable through the boundary.

## What sprout sets for you (plan-time defaults)

These land in the launch plan *unless a user env already defines them*:

| variable | default when user hasn't set one | rationale |
|----------|----------------------------------|-----------|
| `HOME` | `/root` (or the `--user` account's home) | proot-login parity |
| `TERM` | host `$TERM`, fallback `xterm-256color` | colors behave |
| `USER`/`LOGNAME` | anchor user (or the `--user` account) | tools diff `$USER` |
| `SHELL` | guest `/bin/<basename-of-host-SHELL>` if found, else `/bin/bash`, else `/bin/sh` | host SHELL points at a bionic world; guests exec the WRONG libc otherwise |
| `SPROUT_BINFMT_X86_64` | (unset ⇒ default `/usr/local/bin/box64`) | guest path of the x86_64 emulator used when sprout's binfmt adapter sniffs a foreign-arch ELF (ADR-0017) |
| `SPROUT_BINFMT_I386` | (unset ⇒ falls through to `SPROUT_BINFMT_X86_64`, then `/usr/local/bin/box64`) | per-arch override for the i386 emulator (box64's built-in box32 covers it, so one binary is normal) |
| `SPROUT_BINFMT_ALWAYS` | unset | `=1` wraps even native aarch64 ELFs into the emulator (proot `-q` parity for whole-rootfs x86 usage) |
| `SPROUT_SYSVIPC_OFF` | unset | `=1` disables BOTH userspace SysV emulations: the ADR-0018 sysvipc shim DSO (`/usr/lib/sprout-sysvipc/{i386,x86_64}/libsprout-sysvipc.so`) injected into `BOX64_LD_PRELOAD` of wrapped x86 guests (steam's startup semaphores), AND the ADR-0020 `shmget/shmat/shmctl/shmdt` emulation in `libsprout-core.so` for native guests (needed by mesa's XShm present path; disabled shm family then ENOSYS as before) |
| `SPROUT_SYSVIPC_DIR` | `/tmp/sprout-sysvipc` | guest-path directory backing the emulated SysV objects (must be reachable by EVERY wrapped process — keep it the Termux-shared `/tmp` bind for cross-session reach) |
| `SPROUT_SYSVIPC_EMU_OFF` | unset | `=1` disables JUST the host-emulator arm64 SysV-IPC shim lane (session-level `shmget/shmat/shmctl/shmdt`), leaving the in-guest ADR-0020 emulation active |
| `SPROUT_EMU_SYSVIPC_PATH` | unset | explicit host path of the arm64 sysvipc-shim .so (defaults to the shipped artifact under the loader path) |
| `SPROUT_KERNEL_RELEASE` | unset | `uname(2)` override applied by the preloaded uname wrapper; set via `-k/--kernel-release` (proot parity) |
| `SPROUT_USER_NOTIFY` | (auto) | `=0` forces the classic ptrace-only supervise lane even on kernels with seccomp user-notify — the debugging switch that reproduces every notify-less-kernel report locally (v0.4.x learnings) |
| `SPROUT_ALLOW_LOOSE` | unset | `=1` lets `install.sh` install loose `$SRC/<file>` artifacts inside a git checkout (v0.4.5+ default OFF — loose slots are tarball-only); dev-rehearsal escape hatch |
| `SPROUT_PORTMAP` | unset | `=1` (set by `-p/--port-mapping`) makes`bind(2)` on ports <1024 use `1024+port` instead (proot parity for guests binding privileged ports without CAP_NET_BIND_SERVICE) |
| `SPROUT_ASHMEM_MEMFD` | unset | `=1` (set by `--ashmem-memfd`) makes the preloaded `memfd_create()` fall back to `/dev/ashmem` when the kernel says ENOSYS/ENODEV/EINVAL; tracked fd ring feeds the fstat st_size simulation (proot parity) |
| `SPROUT_KILL_TAG` | unset | `=<sprout-pid>` (set by `--kill-on-exit`) marks every descendant env; the post-run proc-sweep SIGKILLs any process whose environ carries it |
| `SPROUT_NO_SHADOW` | unset | disables the supervisor's notify-shadow fastlane (classic ptrace supervision for everything — the ultimate proot-parity fallback) |
| `SPROUT_FAKE_UID` / `SPROUT_FAKE_GID` | unset | numeric ids used by the preload fakeroot synthetics when `--fake-id N` is in use (`-0` ⇒ 0/0) |
| `SPROUT_CACHE_DIR` | `$HOME/.cache/sprout` | sanitized ld.so/libc cache.root (tmp fallback) |
| `SPROUT_KEEP_UMASK` | unset | `=1` stops sprout from re-asserting its default umask for the guest |
| `SPROUT_PRELOAD_PATH` | unset | explicit path to `libsprout-core.so` (dev); the default picks the sibling artifact |
| `BOX64_LD_LIBRARY_PATH` | (unset ⇒ box defaults injected when routing x86_64) | overrides the box64 runtime library search path |
| `BOX32_LD_LIBRARY_PATH` | (unset ⇒ box defaults injected when routing i386) | overrides the box32 runtime library search path |
| `TMPDIR` | `/tmp` | host TMPDIR escapes the guest view |
| `OLDPWD` | (dropped when host-sourced) | `$PWD`-chain confusion |
| `PULSE_SERVER` | inherit host value only — **never invented** | set it yourself, or `--termux-x11` presets `127.0.0.1` (see below) |
| `DISPLAY` | inherit host `DISPLAY` only — **never invented** | set it yourself, or `--termux-x11` presets `:0` *when a `/tmp` bind carries the X socket* |
| `LD_PRELOAD` | the active `libsprout-core{,-musl}.so` (always, in preloaded lanes) | interposer's reality |
| `LD_LIBRARY_PATH` | the sanitized-lib cache + guest lib dirs | loader must reach sanitized libc before anything weird |
| `SPROOT_*` | launcher control keys (LOADER, ROOTFS, BIND, GUEST_PRELOAD, LIBC, SUPERVISED, SHADOW, FAKEROOT, LINK2SYMLINK, PASSTHROUGH, TRACELOG…) | chain-resident state, not meant for guest apps |

## What sprout injects when it has to (chain re-injection)

Some callers spawn children with a *scrubbed* environment — notably
**apk** (which posix-spawns its package triggers with `PATH`-only env).
The interposer's exec/spawn chain re-injects `LD_` and `SPROUT_`
prefixed keys it finds in the **current process's own environment but
missing from the caller's envp** before building the loader chain.
The caller's env stays authoritative — nothing in it is overwritten;
missing is missing only.

Edge case you should know about: `env -i ...` in a `busybox` shell
wipes busybox's own environ-global before its exec call, so there is no
parent environment left to re-inject from; a scrubbed spawn of a
dynamic binary *can* legitimately fail there (musl loader chain can't
be reconstructed without `SPROUT_LOADER`). This mirrors what a real
Linux system would do if you `env -i LD_PRELOAD=...` — the interposer
simply isn't invited. Most callers (apt, apk, pip, make) are not
busybox-env and re-injection works for them.

## What sprout wipes at boundaries

- At the **launch** boundary: nothing. The plan's env is what gets
  applied.
- Inside **spawn chains**: `SPROUT_LD_*` working strings that sprout
  authored (e.g. the interposer's own `LD_PRELOAD=` chain tail)
  re-generated, never duplicated.
- `LD_DEBUG` is not added unless `SPROUT_DEBUG=1`.

## Passthrough

Everything not in the tables above: untouched. `TZ`, `LANG`, `LC_*`,
professional dotfiles secrets you leak through `env`, everything.

## `--preserve-env`-style escape

There is none, intentionally: sprout always applies its own defaults.
To pin a custom PATH/USER/HOME/DISPLAY/GUEST-PATH: set the env vars
yourself (user values win over plan defaults for HOME/TERM/USER/LOGNAME,
`SPROUT_GUEST_PATH` pins the exec-resolution PATH).

## Fake-root is honest noise (loud box)

> **The `uid=0(root)` you see under `--fakeroot` is a statement about
> the guest's address-space truth, not about host privileges.** File
> ownership metadata follows the fake anchor (guests see files as
> *theirs*), but nothing crosses the host boundary under a hoisted uid.
> If a security decision in your guest workflow depends on `id -u == 0`
> meaning "host power", the decision is already wrong on proot as well.
