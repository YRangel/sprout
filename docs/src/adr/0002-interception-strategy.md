# ADR-0002: Interception strategy — LD_PRELOAD fast path, automatic ptrace fallback

- **Status:** accepted
- **Date:** 2026-08-11
- **Deciders:** project owner + initial architecture

## Context

Running a full glibc userspace relocated under a host rootfs requires
translating every filesystem path the guest sees. Three mechanisms exist
on Linux/Android:

| Mechanism | Overhead | Coverage | Constraint |
|---|---|---|---|
| `ptrace` (proot) | 2 ctx switches per syscall stop | ~100% of syscalls, all children | signal handling edge cases; slow |
| `LD_PRELOAD` interposition (proroot) | ~0, in-process | glibc dynamic binaries only | blind to static/Go/raw-syscall guests |
| `seccomp` user-notify | ~0 for fast path | everything below the injecting process | SELinux blocks unprivileged `SCMP_ACT_USER_NOTIF` on Android |

Android does not allow unprivileged user namespaces to create mount
namespaces usable for a plain `chroot`, so any viable approach reduces to
path translation, and the two viable mechanisms are `LD_PRELOAD` and
`ptrace`.

Choosing *only* `LD_PRELOAD` silently breaks static/Go binaries: the `open`
call in a Go program is a raw `svc` instruction, so no preloaded library
interposes anything. This is proroot's real coverage constraint, and it is
the correctness gap sprout must not ship.

## Decision

Every guest program is classified by ELF inspection before launch:

```
ELF64 with PT_INTERP  →  Strategy::Preload (fast path, ADR-0003)
ELF64 without PT_INTERP →  Strategy::Ptrace (fallback, v0.3)
anything else          →  clean error (scripts v0.2, Elf32 unsupported)
```

The `Strategy` enum and the `classify` function live in `sprout-core`. The
CLI flag `--fallback=preload|ptrace|auto` exists to force one path for
debugging but defaults to `auto`.

The ptrace fallback is **not yet implemented**; on a static binary v0.1
returns `Error::PtraceUnimplemented`, which the user surfaces as a clear
message. This is intentional: a known hole in v0.1 is preferable to a
silently broken fast path.

## Consequences

**Easier:** honest coverage, clear scope per release, testable routing,
a clean road for static-binary support (which proot already implements and
which we can port carefully).

**Harder:** sprout keeps two path-translation implementations in the long
run (in-process for preload, in-tracer for ptrace), and they must agree
semantically — the C translation layer is written as a pure header-library
so the eventual ptrace helper can reuse the same `sp_translate` function
rather than grow a divergent table.
