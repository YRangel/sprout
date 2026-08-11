# sprout

**Rootless glibc Linux userspace for Android. Fast, auditable, open.**

`sprout` runs full Linux userspaces (Node.js, Python, Git, Chromium) on Android
without root. It is a drop-in replacement for [proot](https://proot-me.github.io/)
with an `LD_PRELOAD` fast path that avoids ptrace syscall-stop overhead, plus an
automatic ptrace fallback for static/Go binaries (v0.3).

## Status

**v0.1 in active development.** What works today:

- ELF classification → strategy routing (preload vs ptrace)
- Launch through the guest's own glibc loader (no patching, no root)
- Proot-compatible CLI surface: `-r -b -w -0 --link2symlink --dry-run`
- C11 path-translation core with a pure, unit-tested translation layer
- Full workspace test coverage green on Android (Termux) and glibc CI

What lands next:

| Version | Deliverable |
|---------|-------------|
| v0.1    | Direct execve through guest loader, one command at a time |
| v0.2    | `execve` chaining: shebang scripts, child processes propagating the preload |
| v0.3    | Automatic ptrace fallback for static/Go binaries |

See [Roadmap](./roadmap.md) and the [ADRs](./adr/template.md).

## Why not proroot?

[proroot](https://github.com/coderredlab/proroot) pioneered this shape, but:

- **closed-source binaries** — nothing to audit, nothing to patch for your app
- **no static/Go support** — those binaries take raw syscalls, `LD_PRELOAD` is blind
- **`.text` binary patching** — modifies guest code pages, fragile across glibc versions

`sprout` is the auditable re-design: MIT/Apache-2.0, every decision in an ADR,
ci-built binaries, reproducible benchmarks vs proot.
