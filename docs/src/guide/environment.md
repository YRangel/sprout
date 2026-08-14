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
