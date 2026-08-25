# ADR-0019: proot runtime-flag parity (-k/-p/-v/-V/-h/--kill-on-exit/--sysvipc/--ashmem-memfd)

Status: Accepted (shipped 2026-08-14)

## Context

proot-distro users carry shell profiles and scripts written for proot's
full flag surface. sprout's `-r/-w/-b/-0/-q` covered the working set, but
eight runtime flags were missing and scripts tripped on clap's "unrecognized
option" before anything else could run. This ADR ships all eight with
honest semantics — accepted-or-implemented, documented where we diverge.

## Decisions

1. **`-k, --kernel-release <REL>`** — preload-side `uname(2)` wrapper
   swaps `uts.release` post-syscall when `$SPROUT_KERNEL_RELEASE` is in
   env. Sysname/nodename/machine fields stay kernel-truthful (proot
   parity: proot also only patches release). Env is `pushed` onto
   `plan.env` once, inherited through the chain precisely so nested execs
   `bash -c uname` see the same spoof.

2. **`-p, --port-mapping [BASE]`** — preload `bind(2)` wrapper: for
   AF_INET/AF_INET6 with `sin_port ∈ [1, 1023]`, the kernel call gets
   `BASE + port`. BASE defaults to 1024 (proot parity formula); explicit
   `-p 8000` remaps `:22`→`:8022` for example — useful when the host's
   `1000-Series` db stack or another guest is already parked on the
   `10xx` low-ports. `SPROUT_PORTMAP_BASE` env overrides for script use.
   Guards: BASE ∈ [1024, 64512] rejected otherwise (must leave room for
   guest :1023 → host BASE+1023 under 65535).
   Connect(2) is NOT remapped (client intent stays pure), but
   `getsockname(2)` on a remapped listen-fd reports the guest-intended
   port (not BASE+port), tracked by a per-process fd→orig-port registry
   dropped on close. The guest's self-model (sshd logs, avahi announcements,
   dhcpd checks) stays consistent with config. Validated 2026-08-25 on
   HyperOS 6.12.23: guest binds :22, host opens 1046 (default) / 8022
   (-p 8000), guest getsockname reports 22 in both cases.

3. **`-v/--verbose [LEVEL]`** — clap `num_args 0..=1` + `default_missing
   = 1`: both `-v` and `-v 3` parse; level > 0 = debug on (SPROUT_DEBUG=1
   in plan env). Levels >1 reserved — the interposer trace has one depth
   today.

4. **`-V`** — clap's `--version` keeps short banner; `long_version` on
   `-V` adds copyright/license/contact (same payload proot -V prints).

5. **`-h`** — clap's auto-help (usage+description); proot's -h prints
   "version then usage", `:0` diverges trivially (clap layout, never
   frozen in scripts).

6. **`--kill-on-exit`** — tag-sweep: `SPROUT_KILL_TAG=<sprout-pid>` is
   pushed onto `plan.env`; after `plan.run()` returns, `sweep_kill_tagged()`
   walks `/proc/*/environ` and SIGKILLs any process whose environ carries
   the tag. Env survives `exec`, so the tag infects the whole invited
   session without the launcher tracing anything. Gaps (documented
   honestly): processes launched by handlers overlapping OTHER sprout
   sessions (tag is per-launcher-pid — safe), permission-denied procfs
   parses (skipped).
   Divergence vs proot: sprout's supervisor lanes always kill the
   lineage when the main child exits (banked EXITKILL ring semantics).
   The flag is belt-and-suspenders for bare lanes (missing supervisor
   binary on PATH) where the lineage would otherwise escape — a
   documented best-effort layer on top of a default that already runs
   the full sweep.

7. **`--sysvipc`** — semantic no-op compat acceptance: sprout's SysV IPC
   emulation is ALWAYS-ON under box64/box32 (ADR-0018), inverse control
   via `SPROUT_SYSVIPC_OFF=1`. Flag accepted so proot-distro lines parse.

8. **`--ashmem-memfd`** — preload `memfd_create()`: native syscall first
   (GKI 6.12.23 accepts it; ENOSYS/EINVAL/ENODEV only then falls back to
   `/dev/ashmem` + `ASHMEM_SET_NAME/SIZE` ioctls when `$SPROUT_ASHMEM_MEMFD=1`).
   Tracked fd ring (16) lets `fstat`/`fstat64` patch `st_size = lseek-end`
   (offset restored so callers can't observe the probe), since ashmem's
   fstat reports 0. Parity with proot: only weak-fidelity on OLD kernels;
   on modern GKI the flag is invisible (native wins).

## Consequences / validation

- 34/34 base battery (`bench/flags-matrix.sh`), new cells covering each
  flag end-to-end: `-k` spoof (nested exec), `-V` banner, `-h` usage,
  `-v 2` parse+run, `-p` :80→:1080 remap (python probe), `--sysvipc`
  no-op parse, `--ashmem-memfd` memfd fd, `--kill-on-exit` sleep-stub
  dead-after-exit.
- 73/73 extended battery; clippy clean; `install.sh --verify` green
  (the glibc `libsprout-core.so` is REBUILT IN-GUEST per deploy law —
  `cc …` was the lane this session used: `gcc -std=c11 -O2 -fPIC
  -shared -DSPROUT_INTERPOSE … -ldl`, rsync'd to
  `target/release/` + `target/`, then install.sh picks them).
- All env vars documented in `docs/src/guide/environment.md`;
  feature rows in `README.md`.
