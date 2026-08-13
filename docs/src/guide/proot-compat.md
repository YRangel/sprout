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
| `-q <qemu>` | — | intentionally unsupported (host is native aarch64) |
| (HOME passthrough) | `--host-home` | v0.5.1+: default is proot's `HOME=/root`; flag carries the host `$HOME` in |
| (host PATH append) | `--host-path` | v0.5.1+: default is the clean guest-only PATH; flag appends `$PREFIX/bin` (proot-distro's opt-in shape) |
| `-i <id>` | implicit (always the launcher uid) | ✅ equivalent |
| `--kernel-release` | -- | untracked; guests see host kernel |
| `--mixed-mode` | `--fallback=auto` | ✅ v0.3 (semantics satisfied) |
| `--root-id` | `-0` | alias |
| `--verbose <n>` | `-v` / `--verbose` | sprout is less chattery by design |
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
  distrusts the spoofed uid. proot-distro's TCP default is applied by
  sprout (`PULSE_SERVER=127.0.0.1` unless you override).

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

sprout needs *none* of proot-distro's wrapper flags (`--sysvipc`,
`--kernel-release`, `l2s` symlinks): the loader chain plus the interposer
handle everything the dynamic fast path covers, and static binaries route
through the supervisor automatically.

Known deltas vs `proot-distro login`: sprout does not (yet) -b system
paths by default (pass `-b /dev -b /proc -b /sys` if a tool needs them)
and does not fake `/proc/...` contents (loadavg/stat/uptime files that
proot-distro bind-mounts from its own `sysdata/`).

## CLI parity table

| proot / proot-distro flag | sprout flag | notes |
|---------------------------|-------------|-------|
| `-r rootfs` | `-r rootfs` | identical |
| `-b host[:guest]` | `-b host[:guest]` | identical semantics |
| `-w dir` | `-w dir` | guest cwd |
| `-0` (fake root id) | `-0` | exit-code passthrough only |
| `--link2symlink` | `--link2symlink` | |
| `proot-distro login --shared-tmp` | `--shared-tmp` | binds `$PREFIX/tmp` → guest `/tmp`; carries X11/Wayland/VirGL sockets |
| proot bind of host `/dev/*` | `-b /dev/...` | only pseudo files (`/dev/null`, `/dev/urandom`); **real device nodes (kgsl/mali) are SELinux-blocked for ALL unprivileged apps, proot included** |
| proot's fake `/proc` files | — (not implemented) | sprout passes the host `/proc` variant through; doc'd divergence |

