# Interception strategy

The core promise and constraint of sprout. For the full comparison table,
see ADR-0002. For the Android-specific correction to the strategy, see
ADR-0006 (ptrace supervisor for seccomp-blocked guest init).

## Fast path: `LD_PRELOAD` (default)

`sprout` execs the guest program **through the guest's own glibc loader**
with `LD_PRELOAD=libsprout-core.so` in the environment. From that moment
on, every glibc path-taking entry point in the process — `open`, `stat`,
`execve`, `readlink`, `opendir`, and friends — is first handled by our
interposer, which:

1. translates the guest path to its host equivalent (pure table lookup,
   no allocation)
2. tail-calls the *real* libc symbol via `dlsym(RTLD_NEXT, …)`
3. returns exactly what the real call returned

This is not ptrace. There are no signal stops, no `PTRACE_SYSCALL`
round-trips, no seccomp-bpf dispatcher latency. Node starting up against a
sprout rootfs executes the same syscall sequence it would execute natively.

## Qualified: init on Android is supervised (ADR-0006)

On Android 15+ untrusted-app seccomp blocks the syscall glibc uses for
robust futex bookkeeping; the loader can't get to its own first real
instruction under Sigsys. sprout therefore spawns the guest through a tiny
ptrace parent (`sprout-ptrace`) whose only job during startup is:

```
on syscall entry stop:
    if sysno ∈ { set_robust_list, … }:      # blocked-by-Android table
        replace sysno with getpid           # idempotent guest no-op
on exit stop:
    return 0                                 # glibc expects success
```

Once init is done the supervisor stays attached to trace fork'd children
but has **zero impact on steady-state throughput**: nothing stops, nothing
is inspected, the preload core does the work.

## What this intentionally does not cover

- **static binaries** — no PLT, nothing for `LD_PRELOAD` to shadow.
- **Go binaries and raw-`svc` users** — same reason; they make syscalls
  directly via `svc #0`, bypassing libc's symbol table.
- **`dlopen`-then-`syscall`** — anything that `dlsym`s the syscall stub
  from libc and calls it directly.

Those route through the same supervisor's extended table in v0.3.

## Why this is auditable

The interposer is a single C file + a header defining pure translation
functions. There is no patching of `.text`, no PLT redirection beyond
what `LD_PRELOAD` already does, no self-modifying anything. The
supervisor's policy table is a single `switch` statement. Both are
Deterministic and easy to argue about.
