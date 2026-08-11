# 0011. Emulate Android-blocked syscalls at the PLT layer, never at the kernel contract

**Status:** accepted, implemented (v0.4.2)

## Context

Interactive guests died silently during user testing:

```
sprout -r $DEBIAN -w /root /bin/bash    # exits immediately, zero output
```

Root-cause chain (verified by `strace -f`, `strace -k`, `LD_DEBUG=bindings` on Android 16 / kernel 6.12.23):

1. Interactive bash travels the glibc fast path — no supervisor, PLT interposition only.
2. bash sources `/etc/bash.bashrc` → runs `[ -r /etc/debian_chroot ]` → glibc `access()` → **faccessat2 (syscall 439)**.
3. Android's untrusted-app seccomp policy **kills 439** (SIGSYS, sysno table per-device; modern Android blocks the whole set*id + faccessat*familily).
4. Silent death: no output, rc reported as if bash ran.

Rebuilding with `access/eaccess/faccessat/faccessat2` emulated exposed **the second killer**: ncurses' `_nc_safe_fopen()` calls **`setfsuid(getuid())`** on every terminfo read (Linux has no getfsuid); syscalls 203/204 are also blocked → glibc's fsuid-sigsys choreography (`handler → rt_sigreturn → kill(self, SIGSYS)`) → silent death again.

## Decision

Emulate **inside our own interposer's exported PLT symbols**, never at the kernel syscall contract:

| family | mechanism | semantics |
|--------|-----------|-----------|
| `access`, `eaccess`, `faccessat`, `faccessat2` | `fstatat(79)` + uid/gid/group mode-bit evaluation | exact POSIX answer, incl. missing-file ENOENT, dirfd-relative, EACCESS-variant |
| `setfsuid`, `setfsgid` | identity emulation | `setfsuid(uid==current)` returns prior fsuid — the kernel's own computation for rootless guests; switching to a **different** id → `EPERM` (kernel's honest unprivileged answer) |

NOT adopted (rejected, recorded):
- Patching glibc's own `svc` sites for these numbers (would make `access()` lie globally — breaks configure/-r -w tests everywhere).
- Routing glibc fast path through the supervisor (destroys the perf story — the entire reason glibc is the fast path).

Rust-side sanitize table for glibc stays `{99, 293}` — only the two init-time chalts (set_robust_list, rseq) whose callers tolerate fabricated zero-success.

## Consequences

Verified on-device (Android 16 / Devuan-Debian 13 guests):

```
$ sprout -r $DEBIAN -w /root /bin/bash
I have no name!@localhost:/root$ echo INTERACTIVE-ALIVE; id
INTERACTIVE-ALIVE
uid=10372 gid=10372 groups=10372,3003,9997,20372,50372,99909997
```

plus: `test -r/-w/-x` semantics truthful, all batteries green (glibc/python, Go static+dyn, musl busybox+python, statics), 23/23 Rust tests.

Future blocked-syscall discoveries belong in this pattern file —
`sp_emulate_access_impl` and `setfsuid/setfsgid` wrappers in
`crates/sprout-preload/csrc/sprout_preload.c` — with the same "symbol
level, never kernel contract" rule.
