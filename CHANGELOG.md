# Changelog

All notable changes to sprout, grouped by release version. The four-eyes rule: any change that modifies `crates/sprout-preload/csrc/sprout_preload.c` or `crates/sprout-ptrace/csrc/sprout_ptrace.c` gates on the full battery suite before an artifact swap.

## [0.5.1]

### Fixed

- **preload/`env -i` exec-wipe chain** (`sp_snapshot_chain_env`): coreutils `env -i` REPLACES the entire `environ` global before `execve`; every chain-feed site (`sp_build_loader_argv`, `sp_abi_cleanse`, both `sp_real_execve` call sites, `sp_chain_env`) read `SPROUT_LOADER`/`SPROUT_PRELOAD_PATH`/`SPROUT_LIBRARY_PATH`/`SPROUT_LIBC` via plain `getenv()` and committed `[sprout] argv-build fail: SPROUT_LOADER unset` (errno=EIO) for the child. Reproduced as gdk-pixbuf/glycin loader launches aborting `xfce4-panel` + `xfdesktop` at boot. Constructor now snapshots every `SPROUT_`-/`LD_`-prefixed row into a static blob; feed sites fall back via `sp_snap_get`, and `sp_chain_env` appends ctor-snapshot rows missing from the child env (existing rows always win). `test_translate: all OK`; deliberately double-wiped chains (`env -i env -i …`) come up rc=0 with preload mapped.
- **launcher per-exec constant** (`library_path` normalize stamping): every launch re-scanned the giant libdirs (1466 symlinks in `/usr/lib/aarch64-linux-gnu` on trixie) and rewrote absolute→relative alternatives links: 1464 `readlinkat` + 2x `canonicalize` per dir measured under strace, ≈27ms of the ≈44ms launcher constant. The rewrite is idempotent/content-deterministic and a dir's mtime bumps exactly when package scripts touch a symlink, so a `$cache_dir/norm-stamps/<rootfs-hash>.stamp` snapshot lets later launches skip it when untouched. Result: 1554 → 78 `readlinkat` per `/bin/true` launch; per-launch constant 44ms → 39ms.
- **bench/run.sh self-harm**: (1) `median()` returning 1 inside command substitution made `set -e` kill the whole suite on the first FAILED case — the notify-vs-ptrace A/B section silently never finished on any miss since introduction; (2) the git-local section's "pre-seeded" repo was never seeded by anything. `median` now yields an empty measurement on a FAILED case (suite continues); `seed_repo()` plants an identical 50-file repo in both lanes' `/tmp` before the git cases.
- **preload/QPluginLoader-Qt-x-KDE boot chain** (`__realpath_chk`): Qt6Core binds the **FORTIFY variant** of `realpath`, whose internal fd-chase loop resolved `/proc/self/fd` targets out of the supervisor's ADDFD virtual-fd plane — host-spelled strings (`/bin/sh` → `/system/bin/sh`). `QFileInfo::canonicalFilePath() = ""` → `QPluginLoader.locatePlugin()` returned empty → EVERY Qt plugin "not found", plasma could never initialize (misleading "xcb not found even though it was found"). Preload now exports `__realpath_chk`: forward-translate → genuine fortified call on a `PATH_MAX+1` scratch → `sp_reverse` back to guest spelling. Verified with the resolute KDE roster (kwin_x11 + plasmashell + kded6 + dbus).
- **CI artifact toolchain gap** (`libsprout-core.so` release bytes): the GitHub build (ubuntu-24.04-arm, stock gcc-13) deterministically crashed the digest/GL/aa-kcm family at startup in ubuntu-resolute guests (glibc 2.43 comapiled). Identical source rebuilt with gcc-15 = green. `.github/workflows/release.yml` now pins `gcc-14` (CC + CC_aarch64_unknown_linux_gnu) with the rationale inline; the lane advances with the toolchain as glibc ages.
- **cache_dir() EROFS** — no longer fatal on read-only `$HOME`. Cascade: `SPROUT_CACHE_DIR` (authoritative) → `$HOME/.cache/sprout` → `$TMPDIR/sprout-$UID` → `/tmp/sprout-$UID`, first probe-file-writable wins.
- **AT_EXECFN self-exe repair vs cache-dir naming** — the auxv slot rewrite gated on substring `sprout/ldso-`, which silently skipped the repair for ANY cache dir not spelled `*/sprout/*` (`SPROUT_CACHE_DIR` users, the uid-suffixed cascade). Resolute's uutils multicall (echo/md5sum/ls/…) then dispatched on the ld.so's basename and aborted. Now matches `SPROUT_LOADER` exactly first, `ldso-sanitized-*` basename family as fallback.
- **`--bind=/proc` silent death** — proot-distro-style identity binds translated fine at the string level but a bind HIT on the notify lane takes the ADDFD path: the **SUPERVISOR** opens the target, so every `/proc/self/*` returned the supervisor's state to the tracee → SIGSEGV with zero output. Identity binds on /proc,/sys,/dev are skipped at config ingest; the passthrough+native-cont lane was the correct route all along.
- **PREFIX-bind host/cwd anchor flip** — `--bind=/data/data/com.termux/files/usr` (whose host side ancestors the rootfs) hijacked every guest-chased path via `sp_reverse`: the guest's `getcwd()` spelled the full HOST rootfs path instead of `/root`. `sp_reverse` is now *longest-host-prefix-wins* across binds ∪ rootfs; rootfs auto-wins over containing binds; binds deeper than rootfs (the `/tmp→/dev/shm` shim) keep winning. The friend's full original shim shape (`--bind=/proc --bind=$PREFIX --user=0:0`, host cwd under `$PREFIX`) now yields clean `/root`.
- **ddash "$( )" hang under debug lanes** — SPROUT_DEBUG/-v lazy `open("/tmp/sp-auxfix.log")` retuned out of the guest-side init for plain commands (no behavior change for actual debugging).

### Known / app-broken (verified against proot control lane, identical crash there)

`kcm-touchpad-list-devices`, `glxdemo`/`glxinfo` without `DISPLAY`, `aa-features-abi --version`, `aa-*` on kernels without AppArmor FS. These crash natively under `proot` too. See the FAQ for the triage doctrine.

### Test discipline

cargo test 26/26 (workspace), the C-side `test_translate` suite (covers the ancestor-bind reverse regression), cargo fmt + clippy clean, bench `run.sh` + `run-statics.sh` green on the local resolute guest.

## Historical

- **0.5.0** — first tagged release with the resolute guest (26.04, glibc 2.43) as the live acceptance target, v0.4.x wargame-era features stabilized (AF_UNIX-X + shm protocol, sysvipc-mainline, FAKEROOT stat-chain, helium-RPCS3-dwarfs family).
- **0.4.10** — first release with the full v0.4 worklog closed (php-fpm, openat2 propagate, fake-proc chain for `ps`/`time`, statics lane).
- **0.4.x** — pivot era: gcc → libc-sanitized + preload interposer as the working fast-lane.
