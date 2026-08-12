# Path translation

The pure core (`sp_translate` / `sp_reverse` in `csrc/sprout_preload.c`)
runs against a fixed-size config built once at load time:

```c
typedef struct {
    char      rootfs[SP_PATH_MAX];   // e.g. /data/data/…/ubuntu
    sp_bind_t binds[SP_MAX_BINDS];    // sorted longest-guest-first
    int       nbinds;
    int       debug, fakeroot, link2symlink;
} sp_config_t;
```

Translation rules, in this exact order:

1. **Relative paths pass through.** They resolve against the process's
   real cwd, which on Android is already inside the guest view.
2. **Idempotence guard.** A path that already begins with the rootfs is
   returned unchanged. This is what makes stacking safe (an `exec` wrapper
   that re-translates an already-translated path is a no-op).
3. **Binding lookup.** First binding whose `guest` is a path-segment
   prefix of the input wins; `/mnt/sdcard/f.zip` → `/sdcard/f.zip`.
4. **Rootfs prefix.** Everything else becomes `rootfs + path`.
5. **Overflow safety.** If the result would exceed `SP_PATH_MAX` (4096),
   the path is returned unchanged and the call fails naturally with
   `ENAMETOOLONG`, which is safer than truncation.

## Reverse translation

`readlink()` in the guest must return *guest* spellings (otherwise
`ls -la` in the guest shows host paths). `sp_reverse` undoes the exact same
rules: bindings first, then rootfs-strip.

## Environment contract

| Variable | Set by | Consumed by |
|---|---|---|
| `SPROUT_ROOTFS` | launcher | interposer |
| `SPROUT_BIND` | launcher (`host=guest;...`) | interposer |
| `SPROUT_DEBUG` | `--verbose` | interposer (trace to stderr) |
| `SPROUT_FAKEROOT` | `-0` | `getuid`/`geteuid`/`getgid`/`getegid` shims |
| `SPROUT_LINK2SYMLINK` | `--link2symlink` | future `link()` interposition |
| `LD_PRELOAD` | launcher | glibc loader |
| `LD_LIBRARY_PATH` | launcher (host abs dirs) | glibc loader |

No other shared state exists between Rust and C.

## Translation-cache policy (v0.5.2+ correctness rule)

`sp_xcache` (128-entry guest→host map) caches **only follow-0 results**: a
translation that required zero symlink-chase hops. Chased results are
re-resolved on every call because guest-side symlink rotation (`ln -sf`,
rustup/npm link swaps) has no invalidation hook into the interposer; a
cached chase could silently serve the *previous* target. Pure prefix
mappings (rootfs prepend / bind / passthrough) are pure functions of the
path string and are always safe to cache. Every notify-dominated
correctness review in the future should re-check this rule.

Review log 2026-08-12 (v0.5.3): also fixed a latent -Wreturn-local-addr
dangling-pointer in the relative-path join branch (guest-relative paths
under a passthrough-prefixed cwd like guest `/proc` could return a stack
pointer), and hardened the symlink-chase snprintf writes.
