# sprout

**Rootless glibc/musl Linux userspace for Android.** A drop-in replacement
for proot/proot-distro that keeps proot's observable semantics on the
hot paths where it matters (file-system translation, exec chains, env
survival, proot-distro flag compatibility) while replacing its engine:

- **LD_PRELOAD interposition** for glibc-dynamic guests — no ptrace traps.
- **seccomp-user-notify supervisor** for statics, musl guests and the
  syscalls the filter shouldn't trust to libc.
- **Pure-notify `sprout-stub`** on kernels new enough (≥ 5.14 ADDFD) for
  the statics lane without ptrace at all.

All three lanes share one translation engine (`sprout_core`), one
config/env contract, and one cache root (`~/.cache/sprout`).

## Why it exists

proot generality has a price paid by *every* glibc app: ptrace-freeze on
every syscall. On a past-generation phone that is ~2× shell cost; on the
current Android-16/HyperOS kernel it multiplies into whole-desktop stalls
(the proot-distro [#567](https://github.com/termux/proot-distro/issues/567)
slowdown class). sprout pays ctx-switches only for:
*syscall numbers glibc can't interpose (raw svc classes)*, *binaries that
can't carry the preload* (static, Go), *binaries whose arch differs*
(x86/x86-64 through the binfmt adapter to box64/box32/qemu), and *glibc
internals that ignore `LD_PRELOAD`* (see ADR-0011).

The other side of that ledger: sprout embraces **interposer-side emulation**
wherever Android policy kills syscalls that glibc apps expect (set_robust_list,
rseq, sysvipc, `/proc/*` EACCES table, statx) — documented per class in
[Environment policy](guide/environment.md) + ADR-0006/0007/0011/0018/0020.

## Status

- v0.5.x, 2026-08 era. Daily-use proven: Debian trixie guest with XFCE4,
  Firefox ESR, LibreOffice, flatpak-flathub, mesa/turnip hardware GL,
  box64 x86 lane, termux-x11 desktops, pulse-AAudio audio, proot-distro
  compatibility invocations of all four shapes.
- Tested deploy channels: HyperOS SDK-36 phone (6.12 kernel, user-notify)
  AND a POCO 4.14 phone (pure classic ptrace lane). Both run the same
  artifact set with 11/11 flag-matrix + healthcheck.
- Every feature/bug ADR-written; [CHANGELOG.md](../CHANGELOG.md) records
  the exact-bus payloads.

## Layout of this book

- **User guide** — installation, proot compat tables, env policy, X11/GPU, FAQ.
- **Architecture** — overview, interception model, translation rules, threat model.
- **ADRs** — 20+ decisions with rationale (the "how the sausage was made" archive).
- **Operations** — benchmarks and development discipline (how we keep it all honest).
