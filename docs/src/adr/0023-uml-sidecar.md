# ADR-0023: UML sidecar (real guest kernel next to the fast lane)

- **Status:** accepted (Phase 0+1 implemented)
- **Date:** 2026-09-07
- **Deciders:** sprout
- **Supersedes:** nothing. Complements ADR-0007 (sanitized ld.so), ADR-0017
  (userspace binfmt), ADR-0019 (portmap).

## Context

sprout's fast lane (LD_PRELOAD + ptrace fallback) runs leaf compute at
~1.0x native, but some workloads need a *real guest kernel*: systemd as
PID 1, cgroups v2, kernel modules, docker-inside, `binfmt_misc`, raw
sockets. No userspace trick provides these — they are kernel objects.
Prior art: coLinux (guest-as-host-driver, dead on maintenance), LKL
(library kernel, no systemd), gVisor (reimplemented kernel, syscall gaps),
UML (mainline ARCH=um, zero patches, runs as one host process).

UML on Android constraints: no KVM, no TUN/TAP, no userns (confirmed
denied on 6.12.23-android16 by probe-uml.sh gate 6 — irrelevant for UML),
no kernel binfmt_misc registration on the host (dead rootless, ADR-0017).
What the host *does* permit (probe gates 1–3b): ptrace attach, `mmap
MAP_FIXED` replace (SKAS0 requirement), AF_UNIX sockets.

## Decision

**Partition per-process, not per-syscall.** Never merge the kernels
(§Alternatives). One long-lived `linux.uml` owns init (~1.5x); sprout's
fast lane is untouched — the `uml` CLI arm peeks argv[1] before clap, no
shared state, bench gate enforces ±2%.

- Transport: AF_UNIX socket on hostfs-shared dir (~30µs per exec — the
  sidecar's whole speed thesis; no slirp/TUN/TAP/TCP, works rootless on
  Android. UML has no virtio-vsock for ARCH=um).
- Guest `/` = UBD image (block, ~10x faster than hostfs for bulk);
  `/home`,`/tmp` = hostfs exchange mounts; bulk-data dirs mountable via
  hostfs per-dir.
- Agent: ~250-line C daemon, prefork pool, vfork+exec, protocol v1
  (`uml/sprout-uml-agent.c`). Rust std-only was considered; C matches the
  preload csrc toolchain already in-tree.
- `sprout uml up/exec/down/status` in `crates/sprout-cli/src/uml.rs`.
  No auto-routing in v1 (explicit `uml exec` — no misroute bug class).

## Consequences

- New surface: `uml/` dir (agent C source), `scripts/probe-uml.sh`,
  `tests/uml.sh` (7 gates green on Termux/aarch64/6.12).
- Kernel artifact ships via CI (ubuntu-24.04-arm), hash-verified in
  install.sh — no on-phone compile.
- Inside the guest we are root: real `binfmt_misc` registration there
  replaces ADR-0017 sniffing for guest-side foreign-arch execs.

## Field notes (2026-09-09, first E2E cycles)

- **`stub_exe=` is load-bearing on Android.** kbuild emits the SKAS
  stub at `<build-dir>/arch/um/kernel/skas/stub_exe`; if the cmdline
  lacks `stub_exe=`, UML memfd-execs the stub, SELinux denies it, and
  every guest execve returns `-ENOMEM` — which the boot log reports as
  `Starting init: … (error -12)`. See troubleshooting.md.
- **UML panics reboot in-process** (~17s/cycle). `uml.log` also appends
  across boots: a log showing N "No working init" panics is one boot
  panicking N times, not N boots. Truncate before fresh runs.
- **Backend lifecycle**: vhost-device-vsock is spawned only for the
  vsock transport, before the guest, with a socket-existence wait
  (no blind sleep). Files transport boots with no vhost-user
  attachment at all. Transport is chosen by `--transport files|vsock`
  or `SPROUT_UML_VSOCK=1`; the readiness probe execs `/bin/true`
  through the *chosen* carrier, so "up" means that carrier works.
- cmd_down tears down the backend unconditionally (vhost-user master
  never reconnects, so a stale backend poisons the next boot).

## Alternatives rejected

1. **Per-syscall lane hop** (route systemd-ish syscalls to UML, rest to
   host): one kernel must own page tables/IRQs/timers; arbitrating them
   per-op re-invents a hypervisor, slower than either lane. Multikernels
   (Barrelfish) are theses, not features.
2. **hostfs-as-root**: measured ~10x bulk-IO tax (libguestfs); UBD root +
   hostfs exchange is strictly better.
3. **LKL third lane**: no systemd/PID1 story; re-evaluate only if UML is
   SELinux-killed.
4. **Patched UML kernel**: zero-patch policy caps rebase cost at the
   config fragment.
