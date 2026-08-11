# Threat model

sprout runs **untrusted guest binaries** inside the host process's
security context. This page states the trust boundaries plainly.

## Assets

- Host filesystem outside the rootfs (Termux private dir, `/sdcard`, app
  private data)
- Host processes and sockets
- The host session's ambient authority (Termux API, accessibility, ADB)
- sprout's own code (the `.so` we inject)

## Trust boundaries

1. **The guest binary.** Fully untrusted. sprout makes no attempt to
   restrict what syscalls it issues; it only rewrites paths.
2. **The preload core.** Runs inside the guest's address space, uses the
   guest's libc. It must not trust the config blindly — every translate
   call bounds-checks against `SP_PATH_MAX`.
3. **The launcher.** Trusted to build a correct plan. Inputs are flags and
   on-disk ELF files; both are untrusted-content and parsed defensively
   (bounded reads, no `unwrap` on user-controlled byte ranges).

## Threats and mitigations

| Threat | Mitigation |
|---|---|
| Guest escapes the rootfs via `..` | Bind-translate before prefixing; no way to spell `..` past rootfs because translation is a prefix operation on absolute paths |
| Guest `readlink`/`/proc/self/exe` reveals host layout | `sp_reverse` rewrites results before returning; `/proc` bind is off by default |
| Malicious ELF header crashes the classifier | Bounded reads, exact-slice parsing, and explicit `NotElf`/`Elf32` results in `elf.rs`; fuzzed in CI (planned) |
| Malicious rootfs with hostile loader | Loader path searched only within rootfs; a hostile loader is a hostile guest — that is the documented use case, not a bug |
| `LD_PRELOAD` raised privileges | sprout never runs setuid and refuses to if invoked via sudo/doas (tracked) |

## Non-goals

sprout is **not a sandbox**. It provides no seccomp filtering, no network
isolation, no syscall policy. If you need confinement from a hostile
guest, nest sprout inside a kernel-level isolation boundary (Android app's
own UID sandbox is the natural one for Termux).

## Reporting

See [SECURITY.md](../../SECURITY.md) for disclosure.
