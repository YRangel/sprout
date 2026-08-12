# ADR-0016: Pure-notify lane for static guests (sprout-stub) — ptrace reduced to execve-only vehicle

## Status

**Accepted** (T1/T2/T3 implemented, shipped; default ON for kind=1/2
guests; `SPROUT_NOTIFY_STATICS=0` reverts to the legacy TRACEME lane).

## Context

The M1–M3 architecture used ptrace as the outer harness even for
`SPROUT_SHADOW=1` guests: TRACEME + SETOPTIONS + a seccomp filter gave
us lifecycle events (EXEC/CLONE) and the places where registers must be
read/written. But kind=1 (static ELF) and kind=2 (Go static) guests
still paid per-syscall ptrace stops for path translation. Measured on
this device: a 20k×(open+read+close) static workload costs 6.44s under
ptrace — ~10.7µs of stop overhead for *every* syscall — while the same
workload under seccomp user-notify costs 1.62s (**~4.0× faster**).

Research phase established the moving pieces:

- `SECCOMP_RET_USER_NOTIF` notifications carry nr+args, so path
  translation needs no register access.
- Tracee memory is readable/writable with `process_vm_readv/writev`
  (same-uid children pass `PTRACE_MODE_*_REALCREDS` checks).
- Filters are *inherited across fork* and *preserved across execve*, so
  guest lifecycle (clone/exit) is free and the installed filter keeps
  streaming events after any exec.
- **Hard kernel precedent**: `KILL_PROCESS > KILL_THREAD > TRAP > ERRNO
  > USER_NOTIF > TRACE > ALLOW`. Android's app-level bionic policy
  delivers `TRAP` for set_robust_list(99), rseq(293), set*id(143–152),
  setgroups(159): those can *never* be captured by notify. They need an
  in-guest handler.
- execve rewrites (static→dynamic loader chain) are register surgery
  (`x0..x2`, memory layout); notify cannot do them. ptrace is the only
  vehicle — but an `ATTACH+INTERRUPT+GETREGSET+rewrite+SETREGSET+DETACH`
  cycle while the task sits seccomp-parked costs ~0.02ms per exec and is
  legal mid-notification.

## Decision

A new companion: **`sprout-stub`** — a freestanding, `-nostdlib`,
static ELF (host Termux clang suffices) mapped at image base
`0x70000000` (above the standard `0x400000` ET_EXEC guest range so
guest mapping collisions are impossible) that owns the guest process
from its first instruction:

1. **Hand-maps the guest** (ET_EXEC *and* static-PIE ET_DYN, fixed
   static-PIE base `0x40000000`): `MAP_FIXED_NOREPLACE`, zero-BSS,
   apply final page protections, build a fresh 8 MB stack with
   argc/argv/env/auxv rewritten (AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY/
   AT_BASE), then `br` into the guest entry. No execve — the process is
   born unfiltered and stays unfiltered through the loader phase.
2. **Installs the seccomp user-notify filter itself** (`SPROUT_STUB_SOCK`
   handshake: 4-byte fdnum + ACK rendezvous, identical wire protocol to
   the existing child-install path). The filter installs AFTER guest
   mapping — a stub-side filter during its own loader phase deadlocks
   with nobody serving.
3. **Installs an in-guest `SA_SIGINFO` SIGSYS emulation handler** to
   cover the Android-bionic TRAP set that notify can never see:
   glibc guests emulate {99 set_robust_list, 293 rseq}; musl guests
   {48, 143–152, 159}. Handler writes `ucontext->x0 = 0; pc += 4`
   (chromium-sandbox pattern).
4. **`run_notify_statics()`** on the supervisor: socketpair, fork (**no
   TRACEME**), exec stub, steal the listener with `pidfd_getfd`, then a
   plain `poll + serve + waitpid` loop. No ptrace anywhere in steady
   state.

### T3: execve lazy-attach rewrite

`execve(221)` is in the stub's trap list (safe: the supervisor child
exec'd the stub *before* any filter existed). On the notify the
supervisor:

- static target, translated path fits the original buffer → in-place
  `process_vm_writev` + `CONTINUE`;
- static target, longer path → lazy attach, park the string in the
  below-SP scratch arena, rewrite `x0`, detach, `CONTINUE`;
- dynamic target (`PT_INTERP`) → lazy attach, run the existing
  `sp_rewrite_exec_to_loader` surgery verbatim, detach, `CONTINUE`;
- rewrite failure → `-ENOSYS` (a half-translated native exec would start
  an ungoverned loader).

ptrace customers per static-guest exec: exactly one attach/detach pair.
Everything else — path translation, stat serving, AF_UNIX serve,
relative-path fast-CONT — is unchanged supervisor code from ADR-0013.

## Consequences

### Positive

- **~4.0× syscall-rate ceiling** for static guests (measured; see
  docs/src/benchmarks.md). This is the cloudflared-style long-lived
  static workload the whole program was sized for.
- Steady state has **zero ptrace process attachments**: no TRACEME, no
  SETOPTIONS, no signal-swallow EMI, no stop bookkeeping. The supervisor
  is a poll loop on one fd plus waitpid.
- Filter + notifications survive guest exec ⇒ exec chains keep full
  translation fidelity (verified: static→static and static→dynamic
  chains both land translated in the *new* image).
- The legacy TRACEME lane is untouched and selectable per-invocation.

### Negative / limits

- **In-guest emulation is a table, not a verifier**: a guest calling
  set*id(105..) *meaning it* gets a fake success — same honesty position
  as the ptrace SIGSYS-emulation, and identical tables between lanes, so
  the security surface did not move; documented in ADR-0006.
- `PTRACE_ATTACH` inside a notify serve is a dependency of last resort:
  an Android kernel that refuses attach-on-parked-task would degrade
  every static→dynamic exec to ENOSYS. Not observed; guarded by `SPROUT_
  NOTIFY_STATICS=0` per-invocation.
- Stat-family (79/291/78) serving writes structs into guest memory via
  `process_vm_writev`; the ADR-0013 race note (heap-corruption under
  Debug build) was never reproduced — left documented, knobbed, and
  unchanged here.
- Scripts (`#!`) from notify-statics guests fall back to native exec for
  now; ptrace lane resolves the interpreter chain (future work, small).
- `SPROUT_NOTIFY_STATICS=0` keeps the old lane byte-for-byte; CI should
  keep exercising both.

### Debugging scar tissue (recorded to save the next reader hours)

- `SECCOMP_RET_USER_NOTIF` is **0x7fc00000**. The stub's initial
  definition (0x40000000) made the kernel interpret every trap entry's
  return word as an undefined action → instant SIGSYS kill, empty queue.
  Empty queue shows as **POLLHUP** on the listener fd (`seccomp_notify_
  poll` returns HUP when the queue is empty — it is NOT an error). A
  wrong constant is invisible until the first trap fires; the filter
  installs cleanly. Cheap amulet: a one-file C harness (mn*/sp_asm in
  this repo's history) that installs the exact filter bytes and watches
  one RECV/SEND cycle before wiring the full lane.
- `AUDIT_ARCH_AARCH64` is 0xC00000B7 (EM_AARCH64 | 64BIT | LE). Hand-
  rolled `0x400000b7` silently mismatches → TRAP storm on allowed calls.
- `strace -f` on the supervisor is unreliable for lane selection: the
  notify-statics child's own TRACEME-era behavior and strace's clone
  following distort which lane gets picked; prefer in-source debug
  prints behind `SPROUT_DEBUG`.

## References

- ADR-0006 (Android seccomp-blocked syscalls), ADR-0007 (sanitized
  loader), ADR-0008 (supervisor design), ADR-0013 (user-notify fast
  path), ADR-0009 (musl guests; shadow supervision).
- kernel: `Documentation/userspace-api/seccomp_filter.rst`,
  `seccomp_unotify(2)`, `seccomp(2)`.
- Prior art: gVisor Systrap (ptrace-free seccomp supervision, 2023),
  zpoline (USENIX ATC'23; in-memory svc rewrite — deferred, ADR-0003
  spirit).
- Commits: 6fb41a0 (lane + stub), 828968d (T3 execve rewrite).
