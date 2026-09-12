# Changelog

All notable changes to sprout, grouped by release version. The four-eyes rule: any change that modifies `crates/sprout-preload/csrc/sprout_preload.c` or `crates/sprout-ptrace/csrc/sprout_ptrace.c` gates on the full battery suite before an artifact swap.
## [Unreleased]

### Added - virtio-fs: coherent host filesystem for the guest (ADR-0025 D5)
- `sprout uml up` now auto-discovers a `virtiofsd` binary
  ($SPROUT_UML_VIRTIOFSD → sibling of the sprout binary → PATH), spawns
  it as a vhost-user daemon serving `$SPROUT_UML_VFS_ROOT`
  (default <uml-dir>/vfs-root) and attaches `virtio_uml.device=<sock>:26`
  to the guest. After boot, sproutfs0 auto-mounts at /virtiofs.
- Verified end-to-end on device: mount, reads, writes, **mmap read+write
  (host-visible)**, and BOTH coherence directions (host→guest via FUSE
  invalidation; guest→host via writeback). This kills the hostfs
  stale-read class for the share root (hostfs remains for the agent dir).
- scripts/build-virtiofsd.sh + patches/virtiofsd-android.patch: the
  Android build recipe (bionic binding shims; seccomp optional;
  nr_open EACCES tolerance; name_to_handle_at → ENOSYS because the raw
  syscall is seccomp-TRAPped; preadv2/pwritev2 kernel quirks: flags==0
  answers EOPNOTSUPP — route through preadv/pwritev; RWF_HIPRI/NOWAIT
  stripped — f2fs rejects them).
- New kernel config: CONFIG_FUSE_FS=y + CONFIG_VIRTIO_FS=y in
  uml/sprout-uml.fragment (rebuilt kernel required).
## [0.6.1]

### Fixed - stub lane + supervisor correctness (field-found on 6.12.23-android16)
- **sprout-stub frame corruption**: seccomp-TRAP frames on this kernel
  arrive with pc ALREADY PAST the trapped svc; the #74-era `pc+=4`
  skipped the next instruction, cascading to re-traps with stale x8 and
  SIGBUS at pc=0x32. The stub now detects the convention per frame
  (svc-word check) and advances/rewinds accordingly; the accept(202)
  pivot rewinds a past-svc pc so the rewritten svc re-executes.
- **unknown trapped nrs**: forge -ENOSYS + one-line diagnostic instead
  of relying on death-by-re-execution (which returned garbage registers
  on past-svc kernels). openat2/umount2 callers now fall back cleanly.
- **mount/privilege class** (umount2/mount/pivot_root/chroot/syslog/
  reboot/set*time/swapon/module-load...): forge -EPERM in both lanes —
  guests report "permission denied" and survive instead of dying SIGSYS.
- **glibc set*id family**: forge -EPERM (the truth) while musl keeps
  its fake-success; previously a glibc static calling setuid died SIGSYS.
- **dev-loop staleness**: build.rs mirrors sprout-super/sprout-stub into
  the profile dir every build. A Sep-7 stale sprout-super in target/debug
  had been silently preferred by sibling discovery — dropping ALL dev-loop
  runs to the legacy ptrace lane (notify-statics default invisible).
- **SYSEMU evaluated and rejected** (ADR-0025 D1): works on this host
  but cannot interleave with syscall execution (probed) — unusable for a
  translate-and-execute supervisor. Documented in the ADR + policy map.
- **policy map**: docs/src/architecture/android-syscall-policy.md — full
  raw-scan of Android's baseline trap set (0-460), incl. the corrections:
  seccomp user-notify/ADDFD/prctl ALL WORK on this host; nr 39 is
  umount2 not getpid (arm64!); statx/openat2 arg-conditional traps.

### Fixed - journal replay of guest hostfs mounts works end-to-end
Six independent defects stacked into "replay fails":
- **hostfs mount data semantics** (was errno 79/ELIBACC): with UML's 6.16+
  fsconfig-era hostfs, mount(2) monolithic data is appended verbatim to the
  `hostfs=` boot-arg root, so the data string must be the share-dir-relative
  path (`/x`), not a guest-visible path. `hostfs_to_guest()` normalises all
  four spellings (`hostfs/x`, `/run/sprout/x`, `/x`, `x`) to that form.
- **ring echo**: `ring_handle_slot()` fed the request into a socketpair but
  had the feeder child close both ends and call `handle_conn` on the dead
  fd; the parent's capture loop then read its own request back as the
  "response". Restructured: feeder child only writes the request and exits;
  parent runs `handle_conn(sv[0], sv[1])`.
- **mount wedge**: a hung mount(2) killed the single-threaded ring server
  (PING died with it). Mount now runs in a fork+waitpid child.
- **capture-loop EOF**: the parent held sv[1] open after `handle_conn`
  returned, so the response capture loop never saw EOF and blocked forever.
  sv[1] is closed immediately after the handler returns.
- **misplaced confinement**: the bridge rejected guest mount destinations
  outside the hostfs share. Destination confinement removed for the mount
  subop (the kernel-side `hostfs=` boot arg already confines the host data
  path); PROC_READ/FILE_META keep their checks.
- **CLI reply framing** (phantom "errno 5"): the holder wraps agent frames in
  `[u32 total_len]`; the CLI read the total-length low byte as the status,
  so every successful mount reported EIO=5. The CLI now parses the outer
  frame, then the inner `[u8 status][u32 len][payload]`.

### Changed - journal mounts are durable state, not one-shot intents
- Mount rows stay in the journal and are re-applied on **every** `sprout uml
  up` (the guest mount table is per-boot); only `unbind` consumes them
  (`drop_mount_by_dst`). Umount rows remain one-shot and consume the matching
  mount row.
- The session-owner holder **seeds the shadow table from journal mount rows
  at start**, so L0 shadow binds survive holder restarts/reboots.
- Re-bind of an existing dst is replace semantics: old shadow entry
  tombstoned, old journal row dropped.
- `bind` without a hostsrc is a pure L0 shadow bind - no guest-mount row is
  journaled (an empty data string would mount the hostfs root at dst).
- Replay is gated on a ring PING probe (the agent's ring loop attaches a
  moment after the files transport goes live) and retries each mount 3x with
  500 ms backoff.

## [0.6.0]

### Added - ADR-0024 bridge architecture (four-layer ladder)
- **Layer 0, shadow memfd**: single-writer bind table (56-byte header, 28-byte
  entries, strtab blob) shared with the LD_PRELOAD interposer. Reader is
  lock-free (seqlock gen + 2 s heartbeat) and fail-open on any inconsistency;
  empty-table lookup overhead ~150 ns vs 48 ns baseline (T1/T2 receipts in
  `test_shadow.c`). Writer lives in the new session-owner module
  (`crates/sprout-cli/src/session_owner.rs`); discovery prefers a shared file
  (`SPROUT_SHADOW_FILE`) with a pidfd-fetched fd (`SPROUT_SHADOW_FD`) as
  fallback - Android SELinux denies `/proc/<pid>/fd/<n>` across processes, but
  pidfd_getfd works.
- **Session owner daemon** (sprout-uml-hold): per-instance flock
  (`session.lock`), 250 ms heartbeat thread, and a ctl unix socket
  (`shadow.ctl`) with `bind`/`unbind`/`dump`/`quiesce`/`ping`. Owner death
  drops the flock; the next `sprout uml up` claims it and rebuilds the table
  (T8 semantics).
- **Agent bridge opcodes 0x03-0x09** (uml/sprout-uml-agent.c): MOUNT,
  PROC_READ, FILE_META, SIGNAL_FWD, SIGNAL_REV, RELAY_UNIX, EXEC_MIGRATE -
  each behind a 16-byte session-token header and realpath+prefix confinement
  inside the guest.
- **Intent journal + replay** (`crates/sprout-cli/src/journal.rs`): ctl
  bind/unbind append rows to `<uml-dir>/journal.log`; `sprout uml up` replays
  pending intents into the guest through PROTO_MOUNT and confirms them
  (T9). Token provisioning per boot (`provision_token`) with reuse.
- Kernel-side (sprout-arm64 fork, uncommitted there): `/dev/sprout-shm` gains
  write/poll/ioctl wake doorbell for the ring.

### Fixed
- **Shadow table layout mismatch**: the Rust writer initially used a 64-byte
  header / 24-byte entries while the C reader (via real `sizeof`) uses 56/28 -
  attach silently failed in the guest. Constants now measured, C header
  comments corrected.
- **Cross-process fd fetch**: `/proc/<pid>/fd/<n>` open returns EACCES on
  Android even same-uid; replaced with pidfd_open+pidfd_getfd (434/438) plus
  the shared-file fallback above.
- **Agent token gate**: bridge ops now fail open only while no session token
  has ever been provisioned (bootstrap); once `/run/sprout/session.token`
  exists the check is strict.
- **Duplicate dead structs** in sprout-uml-hold (local Journal/Intent) removed;
  `.harness/` untracked.

### Known limits
- Journal-replayed hostfs mounts fail inside the guest with a non-standard
  errno (68/79 classes) - the mount data-path negotiation with the UML hostfs
  driver needs one more pass; rows stay pending and retry on the next up.
- Demotion (L3) is a documented stub; TTY migration intentionally refused
  (returns ENOTSUP) per ADR-0024.

## [0.5.4]
### Fixed
- **shadow fast-path no longer re-injects SIGTRAP**: the v0.5.3 shadow opt-in ran `PTRACE_CONT(sig)` for every stop including `SIGTRAP` / `SIGTRAP|0x80`, which are ptrace event reports (syscall/exec/seccomp stops), not real signals. Re-injecting them queued a genuine trap into the tracee and froze the syscall-stop machinery — every shadowed guest exec wedged forever on 6.12-class kernels (observed: glibc-dynamic main child stuck after openat+openat, `wait4 status=857f`, launcher timeout 124; POCO's 4.14 never delivered the stop so it stayed green). Non-shadow tracees were already safe (generic tail resumes TRAP stops with signo 0). Now the shadow branch only fires for continuable signal stops and falls through to the generic tail otherwise. Verified: single guest exec, full `tests/smoke.sh` (8 gates), `tests/proot-compat.sh` (11 gates) on 6.12.23-android16.

## [0.5.3]

### Fixed

- **ptrace siginfo replay**: SpiderMonkey firefox wasm SIGSEGV handlers now work un-broken under classic-ptrace supervision (kernel-4.14 hosts incl. POCOs). Before: the tracer swallowed every wasm guard-page trap then re-injected with bare `PTRACE_CONT(signo)` — full tracer round-trip per trap *and* `si_code`/`si_addr` fidelity lost on 4.14 → Firefox cascaded into crashreporter-on-relaunch (the misleading "safe-mode" dialog). Now the supervisor fetches and replays siginfo identically before resuming, both on the shadow-tracee fast path and the traced-tracee deliver path. POCO 3-cycle launch→TERM→relaunch produces zero new crashes; the user-land `MOZ_DISABLE_WASM_SIGHANDLERS=1` wrapper is no longer required.

## [0.5.2]

### Added

- **Getting-started guide reworked manual-first** (`docs/src/guide/getting-started.md`): each of the six Steps 0-5 opens with what the layer does and why, then the exact command, then a one-line check before moving on. Step 5 = "the four daemons a desktop needs" diagram (pulseaudio / Termux:X11 / dbus session / xfce4-session) with the dependency chain spelled out. Step 6 = **launch the desktop by hand** (6a pulseaudio with `--exit-idle-time=-1`, 6b termux-x11 with stale-socket sweep, 6c am start to foreground, 6d runtime-root mkdir 0700, 6e full sprout launch with a per-flag table). Step 7 = the start-desktop.sh shortcut as a cumulative wrapper for Step 6, every script statement mapped back to its manual counterpart.
- **New user-guide pages**: `docs/src/guide/commands.md` (every sprout flag with what / why / when-to-use, 26-row summary table), `docs/src/guide/troubleshooting.md` (symptom-indexed — startup, DNS, dpkg, X11, permissions, emulation, performance).
- **mdbook theme polish** (`docs/theme/custom.css` + `book.toml` adjustments): ayu dark default, zebra-striped tables, blockquote callouts, mobile code-wrap fix, boosted search indexing on flag names.
- **`sprout upkg --help`** now prints a policy card (hardlinks → content copies, suid/sgid stripped, dev nodes skipped, traversal rejected) plus supported-extension list (.tar/.tar.gz/.tgz/.tar.xz/.txz/.tar.bz2/.tbz2).

### Fixed

- **`-b <relative-host>`**: resolves against the launcher's cwd via `realpath` exactly proot-style (was hard-error `invalid binding 'etc'`). Nonexistent / malformed binds now warn-once + skip + continue the launch; was: hard-error that killed the guest before boot. Two new proot-compat gates pin both behaviors (relative cwd-bind content probe + nonexistent-host warn+launch green).
- **docs: proot-compat `-b` row** now states the relative-resolution rule and `cwd-relative guest-dir` semantics for `-w`; matches implementation post-fix.
- **docs: `-h` / `--help` after_help** gains `sprout upkg rootfs.tar.xz -C ~/myrootfs` example block.

## [0.5.1]

### Fixed

- **preload/`env -i` exec-wipe chain** (`sp_snapshot_chain_env`): coreutils `env -i` REPLACES the entire `environ` global before `execve`; every chain-feed site (`sp_build_loader_argv`, `sp_abi_cleanse`, both `sp_real_execve` call sites, `sp_chain_env`) read `SPROUT_LOADER`/`SPROUT_PRELOAD_PATH`/`SPROUT_LIBRARY_PATH`/`SPROUT_LIBC` via plain `getenv()` and committed `[sprout] argv-build fail: SPROUT_LOADER unset` (errno=EIO) for the child. Reproduced as gdk-pixbuf/glycin loader launches aborting `xfce4-panel` + `xfdesktop` at boot. Constructor now snapshots every `SPROUT_`-/`LD_`-prefixed row into a static blob; feed sites fall back via `sp_snap_get`, and `sp_chain_env` appends ctor-snapshot rows missing from the child env (existing rows always win). `test_translate: all OK`; deliberately double-wiped chains (`env -i env -i …`) come up rc=0 with preload mapped.
- **exec-name + translate caches** (`sp_ecache`, `sp_exec_caches_clear`): shells hash commands; dpkg/make/python-multiprocessing exec loops did not — every `execvp`-class call re-scanned PATH (translate + `access(X_OK)` × N dirs). Positive-only memo keyed by name+PATH-fingerprint (64-entry); chain-level ENOENT (stale entry after mid-process binary swap) clears BOTH caches and retries cold ONCE. `test_translate: all OK`; python abs/path forms + gcc + chain probes green under the canonical `-DSPROUT_INTERPOSE` recipe.
- **build hygiene note (no code)**: hand-rolled `.so` builds skipping `-DSPROUT_INTERPOSE` produce a HALF-INERT artifact whose chains misroute (measured: python abs-form getpath confusion, guest-inner `ls` resolving to the HOST binary). Canonical recipe lives in `crates/sprout-preload/build.rs` — dev builds must replicate it exactly: `-std=c11 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -fPIC -shared -DSPROUT_INTERPOSE -ldl`.
- **launcher per-exec constant** (`library_path` normalize stamping): every launch re-scanned the giant libdirs (1466 symlinks in `/usr/lib/aarch64-linux-gnu` on trixie) and rewrote absolute→relative alternatives links: 1464 `readlinkat` + 2x `canonicalize` per dir measured under strace, ≈27ms of the ≈44ms launcher constant. The rewrite is idempotent/content-deterministic and a dir's mtime bumps exactly when package scripts touch a symlink, so a `$cache_dir/norm-stamps/<rootfs-hash>.stamp` snapshot lets later launches skip it when untouched. Result: 1554 → 78 `readlinkat` per `/bin/true` launch; per-launch constant 44ms → 39ms.
- **bench/run.sh self-harm**: (1) `median()` returning 1 inside command substitution made `set -e` kill the whole suite on the first FAILED case — the notify-vs-ptrace A/B section silently never finished on any miss since introduction; (2) the git-local section's "pre-seeded" repo was never seeded by anything. `median` now yields an empty measurement on a FAILED case (suite continues); `seed_repo()` plants an identical 50-file repo in both lanes' `/tmp` before the git cases.
- **preload/QPluginLoader-Qt-x-KDE boot chain** (`__realpath_chk`): Qt6Core binds the **FORTIFY variant** of `realpath`, whose internal fd-chase loop resolved `/proc/self/fd` targets out of the supervisor's ADDFD virtual-fd plane — host-spelled strings (`/bin/sh` → `/system/bin/sh`). `QFileInfo::canonicalFilePath() = ""` → `QPluginLoader.locatePlugin()` returned empty → EVERY Qt plugin "not found", plasma could never initialize (misleading "xcb not found even though it was found"). Preload now exports `__realpath_chk`: forward-translate → genuine fortified call on a `PATH_MAX+1` scratch → `sp_reverse` back to guest spelling. Verified with the resolute KDE roster (kwin_x11 + plasmashell + kded6 + dbus).
- **CI artifact toolchain gap** (`libsprout-core.so` release bytes): the GitHub build (ubuntu-24.04-arm, stock gcc-13) deterministically crashed the digest/GL/aa-kcm family at startup in ubuntu-resolute guests (glibc 2.43 comapiled). Identical source rebuilt with gcc-15 = green. `.github/workflows/release.yml` now pins `gcc-14` (CC + CC_aarch64_unknown_linux_gnu) with the rationale inline; the lane advances with the toolchain as glibc ages.
- **cache_dir() EROFS** — no longer fatal on read-only `$HOME`. Cascade: `SPROUT_CACHE_DIR` (authoritative) → `$HOME/.cache/sprout` → `$TMPDIR/sprout-$UID` → `/tmp/sprout-$UID`, first probe-file-writable wins.
- **AT_EXECFN self-exe repair vs cache-dir naming** — the auxv slot rewrite gated on substring `sprout/ldso-`, which silently skipped the repair for ANY cache dir not spelled `*/sprout/*` (`SPROUT_CACHE_DIR` users, the uid-suffixed cascade). Resolute's uutils multicall (echo/md5sum/ls/…) then dispatched on the ld.so's basename and aborted. Now matches `SPROUT_LOADER` exactly first, `ldso-sanitized-*` basename family as fallback.
- **`--bind=/proc` silent death** — proot-distro-style identity binds translated fine at the string level but a bind HIT on the notify lane takes the ADDFD path: the **SUPERVISOR** opens the target, so every `/proc/self/*` returned the supervisor's state to the tracee → SIGSEGV with zero output. Identity binds on /proc,/sys,/dev are skipped at config ingest; the passthrough+native-cont lane was the correct route all along.
- **PREFIX-bind host/cwd anchor flip** — `--bind=/data/data/com.termux/files/usr` (whose host side ancestors the rootfs) hijacked every guest-chased path via `sp_reverse`: the guest's `getcwd()` spelled the full HOST rootfs path instead of `/root`. `sp_reverse` is now *longest-host-prefix-wins* across binds ∪ rootfs; rootfs auto-wins over containing binds; binds deeper than rootfs (the `/tmp→/dev/shm` shim) keep winning. The friend's full original shim shape (`--bind=/proc --bind=$PREFIX --user=0:0`, host cwd under `$PREFIX`) now yields clean `/root`.
- **ddash "$( )" hang under debug lanes** — SPROUT_DEBUG/-v lazy `open("/tmp/sp-auxfix.log")` retuned out of the guest-side init for plain commands (no behavior change for actual debugging).

- **statx emulation over host policy** (proot issue termux/proot#122 owns every rootless runner): raw `syscall(291)` is wiped out by policy or answer-degraded on policy-strict devices, and the supervisor's own answer path used to issue a raw 291 through `sp_statx()`. preload: glibc `statx(3)` callers falling on ENOSYS now get an emulated answer built via newfstatat(262) — `stx_mask` advertises exactly the populated fields (`STATX_BASIC_STATS` minus the unrecoverable btime); supervisor: `sp_statx()` is the same emulation, NEVER the raw call. Verified: guest answers `mode=100600 size=7` via the fallback path on HyperOS, and `mask=7ff` native on the POCO 4.14 lane.
- **fake-proc coverage widened to 6 paths** (`/proc/uptime` joins stat/loadavg/version/overflowu{g,u}id on BOTH lanes + preload materializers): the 5-path table was built off an old-time host scan; the 2026-08-22 sweep measured 13 EACCES files on this HyperOS device — uptime(1)/glibc sysinfo() is the one real consumer class in the delta. Statics keep the classic scratch-file serve; preload guests get the memfd + materialized-file reroute (translate-level, so open/stat/access/readlink all agree). CONSUMER-PROOF: LibreOffice oosplash went from *"ERROR: /proc not mounted"* instant-abort to mapping "Untitled 1 — LibreOffice Writer" on termux-x11 tonight.

### Added

- **`sprout upkg TARBALL [-C DIR]` subcommand** (CLI peeked before clap, ADR-0022): in-Rust tar.gz/xz/bz2/plain extractor with SELinux-aware write-time policy — hardlinks replicated as full-content copies (never libc `link()`), setuid/setgid stripped, device/fifo skipped with warning, `..`/absolute paths rejected. Closes the proot `--link2symlink tar -xJf rootfs.tar.xz` bootstrap gap: `sprout upkg rootfs.tar.xz -C ~/myrootfs && sprout -r ~/myrootfs -- bash` boots directly on the result. Real-world probe: 90 MiB debian rootfs → 12.5k files in ~8s. `sprout upkg --help` prints the policy card.

- **`-q/--qemu` accepts HOST-installed bionic emulators** (box64/qemu-user in TERMUX, not just guest-side): the launcher detects `PT_INTERP='/system/bin/linker*'` on the resolved emulator path and DIRECT-SPAWNS it — previously the glibc loader chain force-fed the guest glibc into the bionic image and box64 died at birth with `invalid ELF header`. Child-exec lane: preload's binfmt wrap scrubbed the chain-injected `LD_PRELOAD`/`LD_LIBRARY_PATH`, passes HOST-spelled targets + libdir defaults, and skips the arm64 sysvipc shim-inject. Guest-glibc emulators keep the old contract. Verified E2E: dynamic x86_64 hello prints "hx86-ok" rc=0 via guest box64 wrap; the HOST bionic lane runs (limits documented in the commit: static x86 dies on Android's set_robust_list block, dynamic needs guest-visible ld-linux).

### Known / app-broken (verified against proot control lane, identical crash there)

`kcm-touchpad-list-devices`, `glxdemo`/`glxinfo` without `DISPLAY`, `aa-features-abi --version`, `aa-*` on kernels without AppArmor FS. These crash natively under `proot` too. See the FAQ for the triage doctrine.

### Test discipline

cargo test 26/26 (workspace), the C-side `test_translate` suite (covers the ancestor-bind reverse regression), cargo fmt + clippy clean, bench `run.sh` + `run-statics.sh` green on the local resolute guest.

## Historical

- **0.5.0** — first tagged release with the resolute guest (26.04, glibc 2.43) as the live acceptance target, v0.4.x wargame-era features stabilized (AF_UNIX-X + shm protocol, sysvipc-mainline, FAKEROOT stat-chain, helium-RPCS3-dwarfs family).
- **0.4.10** — first release with the full v0.4 worklog closed (php-fpm, openat2 propagate, fake-proc chain for `ps`/`time`, statics lane).
- **0.4.x** — pivot era: gcc → libc-sanitized + preload interposer as the working fast-lane.
