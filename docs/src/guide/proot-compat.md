# proot CLI compatibility

The goal is that replacing `proot` with `sprout` in any existing script
is a no-op. The table is sorted by how `proot-distro login` uses flags.

| proot flag | sprout equivalent | status |
|---|---|---|
| `-r <path>` | `-r <path>` | ✅ v0.1 |
| `-b <host>[:<guest>]` | `-b <host>[:<guest>]` | ✅ v0.1 (repeatable) |
| `-0` | `-0` / `--root-id` | ✅ v0.1 |
| `-w <dir>` | `-w <dir>` / `--cwd` | ✅ v0.1 |
| `--link2symlink` | `--link2symlink` | env-plumbed; `link()` shim v0.2 |
| `-q <qemu>` | — | intentionally unsupported (host is native aarch64) |
| `-i <id>` | implicit (always the launcher uid) | ✅ equivalent |
| `--kernel-release` | -- | untracked; guests see host kernel |
| `--mixed-mode` | `--fallback=auto` | ✅ v0.3 (semantics satisfied) |
| `--root-id` | `-0` | alias |
| `--verbose <n>` | `-v` / `--verbose` | sprout is less chattery by design |
| `--help` / `--version` | same | ✅ |

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


### Environment hygiene

sprout, like proot-distro, substitutes a guest-sane `PATH` (`/usr/local/sbin:.../sbin:/bin`) unless `SPROUT_GUEST_PATH` overrides it; learning host PATH habits (`which`-less slim guests notwithstanding).


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
