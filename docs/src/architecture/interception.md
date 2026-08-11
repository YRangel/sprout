# Interception strategy

The core promise and constraint of sprout. For the full comparison table,
see ADR-0002.

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

## What this intentionally does not cover

- **static binaries** — no PLT, nothing for `LD_PRELOAD` to shadow.
- **Go binaries and raw-`svc` users** — same reason; they make syscalls
  directly via `svc #0`, bypassing libc's symbol table.
- **`dlopen`-then-`syscall` patterns** — anything that `dlsym`s the syscall
  stub from libc and calls it directly.

Those cases route to the **ptrace fallback** (v0.3, ADR-0002), which sees
every syscall regardless of how it was issued.

## Why this is auditable

The interposer is a single C file + a header that defines pure
translation functions. There is no patching of `.text`, no PLT redirection
beyond what `LD_PRELOAD` already does, no self-modifying anything. What
libc does after the translation is exactly what it would have done with
the original path.
