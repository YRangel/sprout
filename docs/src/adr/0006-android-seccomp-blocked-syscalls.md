# ADR-0006: Android's seccomp policy forces ptrace supervisor for glibc guests

- **Status:** accepted (blocks v0.1 preload-only fast path)
- **Date:** 2026-08-11

## Context

When sprout was prototyped on Android 16 (API 36, kernel 6.12),
the LD_PRELOAD fast path — which proroot pioneered and which the whole ADR-0002
strategy assumes — turned out not to reach the point where the .so gets mapped:

1. Supervisor execs the guest loader
   `ld-linux-aarch64.so.1 --argv0 ... --library-path ... prog`
2. Guest loader runs rtld bootstrap in the same address space
3. glibc's `__pthread_initialize_minimal` calls `set_robust_list`
4. Android's untrusted-app seccomp profile does not allow
   `set_robust_list` → kernel delivers SIGSYS → guest dies at kill even
   before `main()`

This was verified against glibc 2.41 from a Devuan 6 guest. Older glibc
(≤ 2.39) only calls set_robust_list lazily when threads are used; newer
glibc always calls it during early init.

## Decision

Because there is no config knob in Android's policy and no glibc tunable
that suppresses the call (verified: `init_robust` is unconditional on
glibc ≥ 2.41 and proot's solution for the same class of bug is not a
tunable), sprout must *always* run a lightweight ptrace supervisor when
the host is Android, even for dynamically linked guests:

- The supervisor (`sprout-ptrace`) is a tiny host-side C binary that
  forks + `PTRACE_TRACEME` + `PTRACE_SYSCALL` walkers.
- Its ONLY job is to (a) recognize seccomp-blocked syscalls and (b)
  swap them for an allowed equivalent (`set_robust_list` → `getpid` +
  forged `0` return) so glibc's bootstrap completes.
- After init, the guest continues via our LD_PRELOAD `libsprout-core.so`
  doing fast in-process path translation as designed in ADR-0002/0003.

This preserves the fast path's throughput: once startup completes the
supervisor is no longer involved (it's a tracer, not a seccomp-bpf wait
point), and all subsequent syscalls are handled normally.

**The supervisor is also the natural location of the future v0.3 static/Go
support**, so nothing about the architecture changes — v0.1 just ships the
supervisor earlier than planned.

Heuristics for later work:

- SIGSYS can arrive from several other syscalls on various Android kernel
  versions; the supervisor's policy table is currently
  `{ set_robust_list } only. Extend as new blocked calls are observed.
- proot's own approach (injecting a seccomp SIGSYS handler into the
  tracee before startup; see `src/tracee/seccomp.c` upstream) is a
  backup design if the syscall-swap route proves too brittle.

## Consequences

- Adds a hard dependency on `PTRACE_O_TRACEFORK`/`CLONE` handling
  correctness (forked children of the guest must be traced too, or their
  robust-list calls will kill them).
- Slightly increases startup latency (one extra ptrace dance per guest
  process); amortized to zero after init.
- Means sprout's preload-only design is a guest-libc-version-sensitive
  path; on glibc ≤ 2.39 it would work without the supervisor.
