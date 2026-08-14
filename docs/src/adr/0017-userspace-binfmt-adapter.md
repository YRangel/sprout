# ADR-0017: userspace binfmt adapter (proot `-q` parity)

- **Status:** accepted
- **Date:** 2026-08-13
- **Deciders:** sprout

## Context

proot `-q qemu` runs foreign-architecture guest binaries (typically an
amd64 rootfs under a qemu-user) through the *kernel-level* tracer: every
exec, including the very first one issued by the launcher itself, is
intercepted and rewritten to `[qemu, original, args...]`. The equivalent
mechanism on Android — kernel `binfmt_misc` (`/proc/sys/fs/binfmt_misc`)
— is unusable rootless: the registration write needs CAP_SYS_ADMIN and
the procpfs path is SELinux-denied in Termux. Yet the use case exists:
the debian guest ships `/usr/local/bin/box64`, an amd64 bash
(`/usr/local/bin/box64-bash`) runs (and returns `x86_64` for uname).
The blocker is multi-GB boxed-x86 rootfs downloads, not emulator
availability (box64 is a 200 MB apt package).

Prior art: proot-distro registers box64/qemu-x86_64 via real binfmt_misc
(kernel-level, covers *every* exec including supervisor-only lanes).
The sprout preload interposer chain already rewrites execve argv for
shebang scripts and the loaders, so the interception spine for argv
manipulation exists in-process at zero extra syscall cost.

## Decision

Two gates, both optional and off unless a foreign-arch ELF is actually
sniffed (or `SPROUT_BINFMT_ALWAYS=1` requests proot-parity wrap-everything
semantics):

1. **Launcher gate (sprout-cli / main.rs)**: after the CLI resolves the
   guest program (post shebang recursion), `elf_meta` (sprout-core/elf.rs)
   reads the 20-byte ELF identity header. `(EI_CLASS, e_machine)` of
   `(2, 62)` (ELF64/x86-64) or `(1, 3)` (ELF32/i386) rewrites
   `program/args` to `[emulator, guest-spelled-target, ...args]` before
   the plan is built. `SPROUT_BINFMT_ALWAYS=1` additionally wraps native
   aarch64 ELFs (proot `-q` wrap-semantic parity for whole-rootfs
   emulation). The emulator path comes from (in precedence order):
   `SPROUT_BINFMT_X86_64` / `SPROUT_BINFMT_I386` env, the `-q`-CLI flag
   fed into `rootfs.qemu`, or the default `/usr/local/bin/box64`.
   Missing emulator fails with a named `BinfmtNoEmulator` error
   (`binfmt: no emulator for foreign-arch target '<prog>'...`), never a
   silent ld.so-style ("`error while loading shared libraries`") one.

2. **Chain gate (sprout-preload/csrc)**: `sp_execve_chain()`'s own child
   execve interceptions get the same sniff — an `x86_64-bash` launched by
   a guest script (e.g. through `bash -c /usr/local/bin/box64-bash`) is
   also routed through the emulator. Per-arch library paths are injected into the
   environment (`BOX64_LD_LIBRARY_PATH` /
   `BOX32_LD_LIBRARY_PATH` with sane box defaults), added only when the
   guest env doesn't already set them.

The `-q/--qemu PATH` CLI flag feeds `rootfs.qemu`, and `plan.rs`'s
single push_home_term environmental-default funnel injects
`SPROUT_BINFMT_X86_64` + `SPROUT_BINFMT_I386` into the guest env when the
user didn't explicitly set them (proot-parity: -q's semantics survive
into the guest so guest-process execs of x86 binaries route through the
preload gate the same way).

Scope exclusions (documented, not accidental): the *supervisor*
(static-stub) lane cannot consume a foreign-arch image — its
per-GuestClass decision matrix is aarch64-image-tied, and the tracing
loop has no rewritten-image injection point. The preload lane is the
only interception point, which matches proot-distro's box64 usage
(loader-chain + transparent exec's), so unloaded x86 statics under
`sprout --fallback ptrace` remain unwrapped. Emitting a dedicated
supervisor-side gate remains possible follow-up work, not implied by
this ADR.

## Consequences

- `sprout -r $B /usr/local/bin/box64-bash -c 'uname -m'` → `x86_64`,
  previously a lib-the-loader-style failure.
- Zero per-syscall overhead: the sniff happens once per execve chain
  hop (20-byte file read), never per syscall.
- Termination semantics preserved: the recursion depth limit of
  `sp_execve_chain` (depth > 4 = ELOOP) also bounds inner-emulator
  recursion.
- box32 (32-bit x86) rides the same box64 binary: `SPROUT_BINFMT_I386`
  defaults through `SPROUT_BINFMT_X86_64`, matching box64's integrated
  `-i386` branch.
- Cost-locked-in failure mode: scripts with an x86 interpreter shebang
  already get the same gate through the script→interp re-class (`#!
  /usr/local/bin/box64-bash`). An x86 ELF that is *itself* a script's
  shim used as argv0 remains the user's to point at the emulator.
- A later decision may add the supervisor-lane gate (documented above),
  or refine `SPROUT_BINFMT_ALWAYS` to skip known-host-lived executables
  (e.g. sprout-super). Neither is needed for the proot-distro box64
  usage pattern this ADR targets.
