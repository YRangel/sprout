# sprout, by hand

This page is the flag-by-flag reference. For each option: what it does,
WHY that exists, and when to reach for it.

General calling convention:

```
sprout [OPTIONS] -r ROOTFS -- COMMAND [ARGS...]
```

RULE #1: **options come before the guest command. Separate with `--`.**

The `--` is what tells sprout's parser where its world ends and the guest
begins. Without it, e.g. `-w /var` in a guest command line gets eaten by
sprout instead.

---

## The one flag you need on every call

### `-r PATH` / `--rootfs PATH` — where the guest lives

The "fake chroot". sprout maps every guest-visible path under this dir on
the host. Anything a guest reads or writes under `/` lands inside this
directory.

- Why it exists: proot's `-r` parity. There is no `--` magic here — every
  single run needs it, no exceptions. This is how sprout knows which
  guests' libraries to pull up and which process gets clean knowledge of
  the host's filesystem.
- Path must exist and be at least shallowly populated (there must be
  something under `<rootfs>/lib/` for a guest to actually launch).
- Absolute paths only on the host side. `~/roots/debian` is fine; `debian/`
  relative isn't (historical constraint from the proot translation ABI,
  see ADR-0003).

```sh
sprout -r ~/myrootfs -- /bin/echo hello
sprout -r ~/myrootfs --user=0:0 -- /bin/bash        # interactive
```

---

## The interposer family — steering how syscalls are trapped

These five flags control how sprout traps syscalls. Most users never
touch them; the defaults are auto-detected from the binary.

### `--fallback preload` / `--fallback ptrace` — override the auto router

sprout inspects every invoked binary's ELF header:

| ELF what | Route | Why |
|---|---|---|
| `ET_DYN + PT_INTERP` (glibc dynamic, most binaries) | **preload** | LD_PRELOAD into guest ld.so, no syscall stops. Fast |
| `ET_EXEC` / static / Go | **ptrace** | PLT can't be interposed in a binary that has no ld.so runtime; ptrace supervisor with a static-memory trampoline |

Auto-detection is correct for ~99% of cases. Override exist for testing
and for edges (e.g. hand-assembled ELFs that lie about their ABI).

### `-L` / `--loader-fix` — accepted, prints a note, does nothing

proot used this to repin dynamic loader paths. proot's authors retired the
flag; sprout's loader chain always picks the guest's real `ld.so`, so there
is nothing left to "fix". Accepted for muscle-memory compatibility with
proot-distro.

### `--mixed-syscall` — accepted, no-op

Same story. proot had a "split" syscall handoff; sprout's preload + ptrace
lanes overlap by design so there is no seam to mix anything across.

### `--link2symlink` / `--no-link2symlink` — SELinux hardlink fallback

**DEFAULT since v0.5.** On Android, SELinux denies `link()` syscalls that
cross filesystem boundaries (rootfs mount vs APK data; guest vs host fuse,
etc). Instead of letting the caller fail with EPERM, sprout:

1. Moves the source to a private `$ROOT/.l2s/.l2s.<name>.<nonce>` stash
2. Replaces both source and destination with symlinks → via the hardlink
   registry (sp_hreg[256]) the guest's `stat()` still reports `nlink=2`
   (this is what keeps the `useradd` lock-file protocol working)
3. As a LAST resort, falls back to plain content copy

`--no-link2symlink` opts out: link() gets the raw kernel EPERM.

### `-q PATH` / `--qemu PATH` — userspace binfmt adapter

Hooks `execve` of x86/x86_64 binaries to run them through a guest-side
emulator (default: `/usr/local/bin/box64`; i386 covered by box32).
ADR-0018 spells the full mechanism.

- Env overrides per-arch: `SPROUT_BINFMT_X86_64`, `SPROUT_BINFMT_I386`
- Wrap everything (even native aarch64): `SPROUT_BINFMT_ALWAYS=1`
- This is how the x86_64 guest lane boots (box64 0.4.5 injected
  end-to-end): `sprout -r $X86ROOTFS -- ./program.elf`

---

## Identity — who the guest thinks you are

sprout implements "fake root" the same way proot does: uid/gid syscalls
return what you told them to return; kernel-side the process still runs
as your app uid.

### `-0` / `--root-id` — pretend to be root (DEFAULT)

Guest sees `uid=0(root) gid=0(root)`. Files created appear 0-owned,
`chown()` "succeeds", `getuid()` returns 0. Nothing is actually elevated —
power stays zero, this just keeps Debian package scripts happy.

### `--no-fakeroot` — drop the pretense

Guest sees the REAL uid (e.g. u0_a372). Most admin-flavored commands fail:
`apt install` → EPERM, `chown` → EPERM, `useradd` → EPERM. Useful for
read-only browsing of a rootfs, benchmarking shim overhead against
unfaked identity, or verifying a guest tool doesn't actually need root.

### `-u SPEC` / `--user SPEC` — pick a user from guest /etc/passwd

`-u steamuser`, `-u 1000:1000`, `-u steamuser:steamgroup` — all valid.
sprout resolves the name/uid against the guest's own `/etc/passwd` and
`/etc/group`, then reports the fake IDs. Kernel side stays the launcher
uid; inside the guest everything looks like that user.

- This flag also sets the fake `$HOME`, `$USER`, `$LOGNAME`, cwd, and
  default shell from /etc/passwd
- Identity syscalls, `chown`, `SO_PEERCRED` all anchor to the chosen uid

### `-i SPEC` / `--change-id SPEC` — proot `-i` alias

Identical contract to `-u`. Exists only to read proot-distro command lines
verbatim.

---

## Filesystem visibility — what the guest can see

### `-b host` or `-b host:guest` / `--bind` — extra bind mounts (repeatable)

  - `-b $PREFIX/tmp/x:/mnt` — appears inside the guest as /mnt
  - `-b $HOME/documents` — same spelling both sides
  - Repeatable.

Under the hood: sprout's path translation prepends the bind source when
the guest tries to access the destination. No ptrace, no root, no actual
mount — only a rewrite applied to every path-argument syscall.

### `--shared-tmp` — guest ↔ host socket lifeline

Bind `$PREFIX/tmp` (host) to `/tmp` (guest). Lets guest code see and
connect to:
  - `/tmp/.X11-unix/X0` — the X11 socket
  - the PulseAudio unix socket
  - `$XDG_RUNTIME_DIR/` Wayland sockets

proot-distro's `--shared-tmp` parity. Required for any GUI/audio session.

### `--host-home` — leak the host $HOME into guest

Default guest $HOME = `/root` (or /home/`<user>` from -u). This flag passes
your Termux home straight through. Footgun: anything written to guest
`$HOME` (e.g. game saves, thumbnails) lands in YOUR Termux files. Use
only when you actually want both sides seeing one dir.

### `--host-path` — append `$PREFIX/bin` to guest PATH

Default is a clean guest-only PATH. This adds the host one at the end,
so `python3` resolves to Termux's binary from inside the guest.

---

## Display / Audio — the reason most people install sprout

### `--termux-x11` — X11+audio preset

  - Exports `DISPLAY=:0` (when paired with --shared-tmp)
  - Exports `PULSE_SERVER=127.0.0.1` (always, even alone)
  - Shows a *warning* when paired WITHOUT `--shared-tmp` — the X socket
    won't reach the guest via fs path; you get pulse but no display

Most desktop invocations use precisely:

```sh
sprout -r $R --shared-tmp --termux-x11 -- $SOMEGUI
```

### …and the harness below `start-desktop.sh`

Rather than plain `--termux-x11`, the reference launcher (see
`examples/desktop/start-desktop.sh`) also:

 1. starts PulseAudio on the host (pulse-guard)
 2. starts the Termux:X11 host server and waits for a real handshake
    (x11-rescue)
 3. sweeps stale socket files that would make your guest say "already
    running" then fail to open

This is why "raw `sprout -r … startxfce4`" often fails where
`~/start-desktop.sh start` works — the difference is exactly these three
prep steps.

---

## Network — port behavior under a rootless constraint

### `-p [BASE]` / `--port-mapping` / `-P` / `--redirect-ports` / `--fix-low-ports`

Xiaomi kernel does not enforce CAP_NET_BIND_SERVICE on this device, so
default sprout behavior is: leave low ports alone. Some devices (and the
real proot guarding against these situations) map ports below 1024 to
BASE+port instead. This flag flips that behavior ON:

- Default BASE = 1024, so `-p 22` becomes host `:1046`
- sprout interposes guest `getsockname` so `sshd` *still sees itself
  listening on :22* (human log stays tidy)
- `-p` with no argument uses BASE=1024; `-p 8000` picks 8000 → guest :22
  ends up host :8022
- Valid BASE range: 1024..64512

The translation is host-visible-to-host, so your ssh/ftp/sshd works while
guest-side programs still *think* they bound 22/80/443.

### `-k VER` / `--kernel-release VER` — lie about guest uname

By default the guest reports `$ANDROID_HOST_UNAME`, e.g.
`5.4.210-android12-9-...`. A few apps (mostly enterprise installers)
block on Android-namespaced kernels. This flag spoofs `uname -r` inside
the guest. sprout tracks the spoof through `uname`/`uname -r` chains via
the preload.

Default without `-k`: branded `Sprout-Android-5.4.x-...` so you know
here-now (and so you can disable the brand with `SPROUT_KERNEL_RELEASE=''`
env).

---

## Behavior switches most people never touch

### `-v [LEVEL]` / `--verbose` — log every path translation

Logs syscall name, input path, translated path. Terse at level 1, chatty
at level 3. Chain-injection rules are printed at level 2 and up as they
bind.

### `--dry-run` — print the plan without executing

Shows the final wrapped invocation: LD_PRELOAD paths, LD_LIBRARY_PATH,
env additions, loader to spawn, all without actually starting the guest.
The best place to debug "why did my shim not load" without fighting
real process IDs.

### `--kill-on-exit` — sweep guest processes on parent exit

When the `sprout` parent dies, anything tagged (env-set, marker on
process) gets SIGKILL. Use when running long-lived daemons you want
closed when the terminal closes. Draws the line at "anything I launched
directly" — grandchildren or things forked from a DIFFERENT sprout
invocation are out of scope.

### `--sysvipc` — acceptance no-op

proot-distro `--sysvipc` parsed as accepted. sprout's SysV emulation
(shm+sem via libandroid-shmem protocol + LD_PRELOAD wrappers) is
**always on** for x86/box64 lanes (kernel has no CONFIG_SYSVIPC).
Switch off explicitly with `SPROUT_SYSVIPC_OFF=1` env.

### `--ashmem-memfd` — acceptance no-op

proot's equivalent hint when the kernel lacks `memfd_create`. sprout
falls back to /dev/ashmem internally regardless; taking this flag is a
parse-safe no-op for proot-distro compat.

---

## The out-of-band subcommand

### `sprout upkg TARBALL [-C DIR]`

Extract a `.tar` / `.tar.gz` / `.tar.xz` / `.tar.bz2` onto the FS with
SELinux-aware write-time policy (hardlinks → content copies, setuid
bits stripped, dev/fifo skipped with warning, path-traversal rejected).
Replaces proot's `--link2symlink tar -xJf …` bootstrap; does not require
any existing guest. Detailed mechanics + ADR-0022.

---

## Flag summary table

| Flag | What it does | When you need it |
|---|---|---|
| `-r PATH` | pick guest rootfs | every time |
| `-w DIR` | guest cwd | when init shell cwd is wrong |
| `-b H:G` | extra bind | exposing host fs slice to guest |
| `-0` | fake root (DEFAULT) | apt/installs/desktop sessions |
| `--no-fakeroot` | real uid | benchmarking fake-id cost |
| `-u U[:G]` | pick guest user from /etc/passwd | pretending to be steamuser / limited user |
| `-i U[:G]` | proot -i alias for -u | copy-pasting user-id proot recipes |
| `--shared-tmp` | $PREFIX/tmp ↔ guest /tmp | X11/Wayland/audio/ssh-agent hosts |
| `--termux-x11` | DISPLAY=:0 + PULSE_SERVER | with --shared-tmp for a desktop |
| `-L` | (note-only no-op) | paste-legacy compat |
| `--mixed-syscall` | (no-op) | paste-legacy compat |
| `--sysvipc` | (no-op; ADR-0018 always-on) | paste-legacy compat |
| `--ashmem-memfd` | (no-op) | paste-legacy compat |
| `--link2symlink` | SELinux link fallback (DEFAULT) | rust-toolchain installs, apt postinsts |
| `--no-link2symlink` | disable link fallback | never unless you're chasing a file |
| `-p [B]` | low-port remap | devices where CAP_NET_BIND_SERVICE bites |
| `-q PATH` | x86 emulator binfmt | x86_64/i386 guests on arm64 phones |
| `--fallback P` | force preload or ptrace | hand-assembled/weird ELFs, debug |
| `--dry-run` | print plan only | debugging wrapper composition |
| `-v [N]` | translation log | chasing a path translation bug |
| `--kill-on-exit` | sweep guest procs on parent exit | daemon jobs |
| `-k REL` | spoof uname -r | picky enterprise installers |
| `--host-home` | reuse host $HOME in guest | stray cases needing one writable zone |
| `--host-path` | append host PATH | ad-hoc tooling bridging |
| `upkg TAR [-C DIR]` | rootfs bootstrapping extractor | unpacking a tarball into a new rootfs |

---

## See also

- [From zero to desktop](./getting-started.md) — full onboarding
- [Troubleshooting](./troubleshooting.md) — every error and its fix
- [FAQ](./faq.md) — long-form maintenance / architecture questions
- [Environment policy](./environment.md) — what env sprout injects
- [proot compat chart](./proot-compat.md) — proot ↔ sprout flag mapping
