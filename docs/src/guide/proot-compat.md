# proot CLI compatibility

> **FAKE-ROOT IS HONEST NOISE.** `id -u == 0` inside a sprout guest
> describes the *guest's* view of its own address space, never a host
> privilege. If a security decision trusts `id -u`, it was already wrong
> under proot too. See [environment policy](./environment.md) for the
> full env/env-injection table.

The goal is that replacing `proot` with `sprout` in any existing script
is a no-op. The table is sorted by how `proot-distro login` uses flags.

| proot flag | sprout equivalent | status |
|---|---|---|
| `-r <path>` | `-r <path>` | ✅ v0.1 |
| `-b <host>[:<guest>]` | `-b <host>[:<guest>]` | ✅ v0.1 (repeatable) |
| `-0` | `-0` / `--root-id` | ✅ v0.1; **DEFAULT since v0.5** (opt out with `--no-fakeroot`) |
| `-w <dir>` | `-w <dir>` / `--cwd` | ✅ v0.1 |
| `--link2symlink` | `--link2symlink` | ✅ v0.4.3+; **DEFAULT since v0.5** (opt out with `--no-link2symlink`). EPERM hardlinks degrade like proot's `.l2s` (content relocated to `$ROOT/.l2s/.l2s.<name>.<nonce>` + symlinks at both src and dst — survives `link-then-write-through-fd` like glibc's locale-archive builder), then content-copy, symlink as last resort |
| `-q <qemu>` | `-q <emu>` | userspace binfmt adapter: x86_64/i386/box64 execs rewritten through the given guest-side emulator (default `/usr/local/bin/box64`); env `SPROUT_BINFMT_X86_64`/`SPROUT_BINFMT_I386` overrides per arch, `SPROUT_BINFMT_ALWAYS=1` wraps every exec (ADR-0017) |
| (HOME passthrough) | `--host-home` | v0.5.1+: default is proot's `HOME=/root`; flag carries the host `$HOME` in |
| (host PATH append) | `--host-path` | v0.5.1+: default is the clean guest-only PATH; flag appends `$PREFIX/bin` (proot-distro's opt-in shape) |
| `-i <id>` | implicit (always the launcher uid) | ✅ equivalent |
| `--kernel-release <rel>` | `-k` / `--kernel-release <rel>` | ✅ (ADR-0019; preload uname wrapper patches uts.release, env-inherited through nested execs) |
| `--mixed-mode` | `--fallback=auto` | ✅ v0.3 (semantics satisfied) |
| `--root-id` | `-0` | alias |
| `--verbose <n>` | `-v` / `--verbose` | sprout is less chattery by design |
| *(no proot equivalent)* | **`sprout upkg TARBALL [-C DIR]`** | host-mode rootfs bootstrap (aka proot `--link2symlink tar -xJf …`). In-Rust extractor with SELinux-aware policy: hardlinks replicated as full-content copies, setuid/setgid dropped, dev/fifo skipped, `..` / absolute path traversal rejected. See ADR-0022. |
| `-v <n>` | `-v <n>` | ✅ levels accepted (reserved >1) |
| `-V` | `-V` | ✅ banner + copyright/license/contact |
| `-h` | `-h` | ✅ auto-clap usage |
| `--kill-on-exit` | `--kill-on-exit` | ✅ v1 : env-tag sweep over /proc (bare lanes belt; supervisor lineage-kill always runs) |
| `--sysvipc` | `--sysvipc` | ✅ accepted (semantic no-op; SysV IPC emulated always-on: ADR-0020 shm for native guests, ADR-0018 sem/shm for box64/box32) |
| `--ashmem-memfd` | `--ashmem-memfd` | ✅ preload memfd_create fallback + fstat st_size sim |
| `-p` | `-p` / `--port-mapping` | ✅ preload bind remap, ports <1024 -> 1024+port |
| `--help` / `--version` | same | ✅ |

## apt/dpkg runtime compatibility (v0.4.3+)

Full cycle `apt-get update` / `apt-get install <pkg>` is verified working
inside sprout. Like proot, this needs the special pieces of the stack:

- dpkg-deb's rename/linkat-style staging needs rewrites; the interposer
  adds renameat / renameat2 / link / linkat / symlinkat
- `/bin/sh` Post-Invoke scripts use execvp() — our wrapper rewrites them
- apt's sqv verification stage needs mkstemp(), mkdtemp() and tmpfile()
  wrapping so its `/tmp/apt.sig.XXXXXX` style staging lands in the shared
  tmp bind, not the host /tmp
- `--link2symlink` passes proot semantics: chown() reports success without
  touching inodes, and hardlink clones of artifacts become symlinks
- Debian's Alpine trigger side (`busybox r31.trigger`) uses the same
  `execvp` chain and the DB write uses chmod fake-passing

sqv staging `mkstemp`-backed, chmod fake-passing, execvp() chain covers
the Post-Invoke shell — users don't need any special flags:
`sprout -r <debian> apt-get update` works out of the box, no proot-distro
plugin needed.

### Reading proot-distro's `.l2s` stubs

proot-distro rootfses often contain `--link2symlink` relics from older
boots: symlink targets spelled into `$ROOT/.l2s/.l2s.<name>.<nonce>`
(dpkg's `status-old`/`diversions-old`, alpine's `/usr/bin/{ar,as,ld,...}`
binutils, `/etc/alternatives` chains). Both stub species load correctly
since v0.4.5: `sp_translate` (preload + supervisor) walks the link chain
at translate time and `guest_real()` strips the rootfs prefix from
host-absolute targets. Plain `readlink(2)` guests still see the stub
spelling, matching proot's own view.

## Differences worth knowing

1. **Errors are noisier on purpose.** If a guest binary is static and
   `v0.3` ptrace isn't in the build yet, sprout says so instead of
   silently falling back to weird behavior.
2. **`--dry-run` doesn't exist in proot.** Use it; it is the single most
   useful debugging tool.
3. **Termux reads environment.** sprout exports `SPROUT_*` variables;
   proot never exposed its configuration — this is a feature, not a leak.

`proot-distro login` can be pointed at sprout by symlinking
`$PREFIX/bin/proot` → `sprout`; a `$PREFIX/bin/sprout` alias exists too.


### Known quirks (proot-identical unless noted)

- **tmux `capture-pane` content for detached sessions is empty on BOTH
  runtimes.** The pane and its child run fine; the screen-grid read
  back returns nothing. Workaround (works in sprout AND proot): use
  `tmux pipe-pane -t <sess> -o 'cat > /tmp/out'` to stream pane output,
  or drive deterministic output through files. Root cause is tmux's
  screen-buffer handshake on ptrace-class runtimes; not a sprout gap.
- **`ls /dev` fails EACCES for any unprivileged uid on Android**
  (SELinux characteristic; proot shows the same error). Device *nodes*
  work when accessed directly by name.
- **pulseaudio's unix socket is unreachable** for fake-id runs (both
  runtimes): the server authenticates the peer via SO_PEERCRED and
  distrusts the spoofed uid. proot-distro's TCP default is available on
  request: `--termux-x11` presets `PULSE_SERVER=127.0.0.1` (or export it
  yourself — sprout never invents it).

### Environment hygiene

sprout, like proot-distro, substitutes a guest-sane `PATH` (`/usr/local/sbin:.../sbin:/bin`) unless `SPROUT_GUEST_PATH` overrides it; `--host-path` appends `$PREFIX/bin`. `HOME=/root` by default (proot parity) — `--host-home` carries the host `$HOME` in. `TERM` is inherited from the host (with an `xterm-256color` fallback when the host didn't set any). The supervisor binary is `sprout-super` since v0.5.1 (`sprout-ptrace` stays as a legacy symlink): the name tools like fastfetch report comes straight from the process tree.


## Using with proot-distro rootfses

sprout runs proot-distro guests without proot. Locate the rootfs
(`proot-distro` v5 layout):

```sh
ROOTFS=$PREFIX/var/lib/proot-distro/containers/debian/rootfs
sprout -r $ROOTFS /bin/bash              # interactive shell
sprout -r $ROOTFS /usr/bin/python3 ...   # any tool
```

sprout still needs no special flags to import proot-distro environments;
`--sysvipc` is a semantic no-op lens on the always-on emulation of
ADR-0020 (native shm) + ADR-0018 (binfmt-lane sem/shm),
and the loader chain plus the interposer handle everything the dynamic
fast path covers (static binaries route
through the supervisor automatically).

Known deltas vs `proot-distro login`: sprout does not (yet) -b system
paths by default (pass `-b /dev -b /proc -b /sys` if a tool needs them).

## What sprout fakes for you (new in the 2026-08 cycle)

/proc entries HyperOS (SDK-36-era) hides behind `EACCES` from untrusted
uids — served through EVERY enforcement level (preload memfd answers,
translate-level materialized-file reroute, notify ADDFD inject, classic
scratch-file) so whichever lane carries the process, the file opens:

| path | content | consumer class |
|---|---|---|
| `/proc/version` | `Linux version R (sprout-build) (gcc (sprout)) #1 SMP PREEMPT M` (host `uname(2)` painted) | LibreOffice oosplash probe → this was proot#175's full fix |
| `/proc/stat`, `/proc/loadavg`, `/proc/uptime` | synthetic but parseable/moving | htop/top/ps class, glibc sysinfo() fallback |
| `/proc/sys/kernel/overflowuid`, `/proc/sys/kernel/overflowgid` | `65534` | bwrap/pressure-vessel range validation |

statx(2) emulation: raw `syscall(291)` is policy-killed or ENOSYS-able
on policy-strict devices (HyperOS: `RET_KILL` outranks our notify answer —
all rootless runners die identically), but glibc `statx(3)` callers are
interposed at PLT and answered via newfstatat(262) emulation, stx_mask
advertising exactly the populated fields.

## CLI parity additions (2026-08-22, friend-reported class)

| proot habit | sprout | status |
|---|---|---|
| `-i NUM` / `--change-id UID:GROUP` | `-i/--change-id` | **ALIAS of `--user`** (numeric + named both; conflicts_with=user) |
| `-L` (obsolete ld loader-fix) | `-L/--loader-fix` | accepted with one-line note, no action — loader resolution is always correct |
| `--mixed-syscall` | `--mixed-syscall` | accepted as no-op note; preload lane handles glibc wrappers natively |
| `--redirect-ports`, `--fix-low-ports`, `-P` | same three spellings + `-p/--port-mapping` | all four are ONE knob |

### Unknown-flag discipline (same change)

Older builds let any unrecognized dash-token "slide through" and
the whole rest of argv: `sprout -L --rootfs=R bash` ended with clap
mystifyingly saying *rootfs was missing* (the token had eaten it);
`sprout -r R -L bash` treated `-L` as the guest program name. Today: hard
errored on contact WITH the `--` hint; only tokens after an EXPLICIT
`--` are allowed to start with dashes. `sprout --help` prints the full
compat map at its tail.

## CLI parity table

| proot / proot-distro flag | sprout flag | notes |
|---------------------------|-------------|-------|
| `-r rootfs` | `-r rootfs` | identical |
| `-b host[:guest]` | `-b host[:guest]` | identical semantics |
| `-w dir` | `-w dir` | guest cwd |
| `-0` (fake root id) | `-0` | exit-code passthrough only |
| `--link2symlink` | `--link2symlink` | |
| `proot-distro login --shared-tmp` | `--shared-tmp` | binds `$PREFIX/tmp` → guest `/tmp`; carries X11/Wayland/VirGL sockets |
| proot-distro login profile's `DISPLAY`/`PULSE_SERVER` exports | `--termux-x11` (opt-in) | presets `DISPLAY=:0` (when a `/tmp` bind exists) + `PULSE_SERVER=127.0.0.1`; without the flag sprout only inherits host-set values |
| proot bind of host `/dev/*` | `-b /dev/...` | real device nodes are SELinux-blocked *on most devices*; on the Xiaomi 25102PCBEG `/dev/kgsl-3d0` is world-open, and sprout's `-b /dev/kgsl-3d0` (or the default `/dev` visibility) gives guests native freedreno/Turnip Vulkan (vkmark ≈ 10.4× proot) |
| proot's fake `/proc` files | — (not implemented) | sprout passes the host `/proc` variant through; doc'd divergence |


### Known quirks (workarounds)

- ~~`CANNOT LINK EXECUTABLE "sh": library "libc.so" not found` during XFCE
  startup~~ **FIXED (2026-08-13, commit 88c6ed0)**: glibc's `popen()` execs
  its `/bin/sh` child through a raw internal syscall, bypassing preload
  interposition — the child landed on HOST bionic `sh`, whose linker then
  walked `/system|/odm|/vendor` lib64 through our notify translation and
  complained about missing bionic `libc.so`. `popen()`/`pclose()` are now
  interposed (pipe2+fork+chain-exec, same skeleton as our `system()`).
  Other glibc spawn sinks already covered: `system()`, `posix_spawn`,
  `execve` family.
- **XFCE4 desktop session works** (panel, wallpaper, xfwm4, plugins — verified
  on-device 2026-08-13). Launch pattern: `sprout -r $ROOTFS --shared-tmp sh -c
  'eval "$(dbus-launch --sh-syntax --exit-with-session)"; exec xfce4-session'`
  (pre-launched dbus; the `--autolaunch` fallback inside components can see a
  stale `~/.dbus/session-bus` after killed sessions — delete it if components
  fail with `dbus-launch ... Child process exited with code 2`). Two sprout
  bugs fixed to reach the desktop: (1) SysV-IPC family (`shmget` et al.,
  Android always SIGSYS-kills it) now fakes `-ENOSYS` in BOTH lanes so
  MIT-SHM/libxcb users fall back instead of dying mid-session; (2) sprout's
  exec chain used to return a blanket `ENOEXEC` for *missing* paths, which
  silently broke glib's g_spawn PATH walker (`sh cand` dash-fallback →
  "/bin/sh: 0: cannot open /usr/local/sbin/...") → xfce4-session spawned no
  components; `ENOENT`/`EACCES` parity restored.
- D-Bus under fake-root: sprout rewrites guest `SCM_CREDENTIALS` ancillary
  data to the REAL uid/gid in `sendmsg` (the guest believes uid=0; the
  kernel would otherwise EPERM the impersonation and every GDBus client died
  with `Error sending credentials`). `dbus-send` (libdbus, no creds cmsg)
  was always fine; the GDBus path required this surgery.
- gdbus CLI quirk (host-independent GLib behavior): `DBUS_SESSION_BUS_ADDRESS`
  is honored only with an explicit `--session` / `--system` / `--address`
  — the bare default connects to the STARTER bus. Not sprout's doing.
- tmux detached `capture-pane` = empty (proot-parity; use `pipe-pane`).
- PulseAudio unix-socket: use `PULSE_SERVER=127.0.0.1` (preset by
  `--termux-x11`; otherwise your own export).
- AT-SPI bus (`org.a11y.Bus exited 1`): cosmetic, proot-parity.
