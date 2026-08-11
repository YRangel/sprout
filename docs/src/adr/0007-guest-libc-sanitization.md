# 0007. Sanitize guest libc/ld.so defeat Android seccomp (fast path)

**Status:** accepted, implemented (supersedes "preload-only" v0.1 plan on Android ≥15)

## Context

ADR-0006 records the blocker: glibc 2.41 calls `set_robust_list` (sysno 99)
unconditionally from `__tls_init_tp` before any `LD_PRELOAD` library loads;
Android ≥15's untrusted-app seccomp kills the process (SIGSYS, exit 159)
as soon as that instruction executes — before any signal handler, constructor,
or interposer could intercept it. Two further realizations:

1. `ld.so` is statically linked: it embeds its own `__libc_early_init` with
   its own blocked-syscall sites (99, and 293 `rseq`), in addition to libc's.
2. The device's seccomp disposition is `SECCOMP_RET_TRAP` (SIGSYS delivered as
   signal), not `SECCOMP_RET_KILL_PROCESS` — so a ptrace parent *can* swallow
   it (proved working) — but we want ptrace to be the last resort, not the
   fast path (per user direction and ADR-0002).

## Decision

At plan time (host side, Rust, `sprout_core::sanitize`), produce **cached
derivative copies** of the guest `ld-linux-aarch64.so.1` and `libc.so.6`:

- scan each executable `PT_LOAD` for the AArch64 pattern
  `mov x8, #<sysno>` **… up to 12 instructions …** `svc #0`;
- patch the `svc` to `mov x0, xzr` (LE `e0 03 1f aa`) — i.e. the call returns
  0 ("success") instead of trapping;
- only for the emulated sysnos `{99 set_robust_list, 293 rseq}` — both safely
  ignored by their callers (glibc falls back gracefully for each);
- name cached copies `{kind}-sanitized-{FNV-1a64(hash):016x}.so` in
  `$SPROUT_CACHE_DIR` (default `$HOME/.cache/sprout`);
- roots of the guest rootfs are **never modified** (read-only contract).

The sanitized **loader** becomes the exec target and receives
`--argv0 / --inhibit-cache / --library-path`; the sanitized **libc** is
preloaded *after* the interposer (`LD_PRELOAD=libsprout-core.so:sanitized-libc`)
because `LD_PRELOAD` resolves left-to-right for symbol overrides. The launcher
also exports `SPROUT_LOADER` / `SPROUT_LIBRARY_PATH` so exec-chained children
re-enter the same loader chain (see 0002 sibling amendment).

If sanitization yields no patch sites or decoding fails, plan-building falls
back to the **ptrace supervisor** (`sprout-ptrace`, last resort).

## Consequences

- End-to-end dynamic glibc execution on Android 15+/16 with **zero ptrace**
  in the common case (measured 2.2–3.7× faster than proot-distro, see
  benchmarks.md).
- The emulation surface is purpose-built and auditable: two sysnos, two files,
  deterministic cache. New blocked syscalls discovered on new Android/glibc
  pairs need only join the whitelist set.
- Static guest binaries cannot be sanitized this way? (they can — same scan
  works on ET_EXEC/ET_DYN static too — but their un-instrumented syscall sites
  can't be *redirected to translation*, so they still fall back to the
  supervisor for path translation.)
- Cache invalidation is content-hash based: guest upgrades automatically
  re-sanitize.

## Alternatives considered

- Preload installing a SIGSYS handler — impossible: the fatal call precedes
  any preload init.
- Fork/exec+a handler — signal dispositions don't survive execve.
- Patch the launcher to mask seccomp — requires kernel cooperation (ptrace,
  no_new_privileges games) or .text patching (ADR-0003 forbids).
- ptrace-everything — works (supervisor exists, SIGSYS swallow proved) but
  defeats the "no ptrace fast path" goal.
