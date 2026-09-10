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

## Field notes (2026-09-09, second E2E wave: profiles, durability)

- **`--profile mini`** (`SPROUT_UML_MINI=1`): boots with
  `init=/root/mini-init` — the agent as PID 1 over a bare devtmpfs.
  Boot drops 13.2s → **0.5s**. Stateless by design (no persistent
  guest state should be written in mini); systemd profile remains the
  default for stateful work. Mini verified over both vsock and files.
- **Graceful down is mandatory for dirty rootfs.** A SIGKILL'd guest
  loses its page cache: writes vanish, partial flushes can corrupt the
  ext4 image. cmd_down now powers off files-transport guests through
  the files carrier (`/sbin/poweroff` request + guest-exit poll, 25s)
  before any SIGTERM/KILL. Never kill -9 a guest whose writes matter.
- **Stale poweroff request = boot landmine.** If the poweroff request
  survives guest death, the next boot's agent drains it and powers off
  instantly — every subsequent `up` fails. cmd_up purges `req.*`/
  `resp.*` from the share before boot.
- **`rw` kernel arg is required**: fstab in the trimmed image is
  unconfigured, so the classic remount-fs never runs and the kernel
  default `ro` sticks — every rootfs write returns EROFS.
  build_cmdline passes `rw` explicitly.
- **Auto transport default**: vsock when the vhost-user backend is
  resolvable (`SPROUT_UML_VHOST` or `PATH`), files otherwise. Explicit
  `--transport` always wins.
- **Rung 3 (shared-memory ring) SHIPPED 2026-09-09** — `--shm`/
  `SPROUT_UML_SHM=1`: guest RAM backed by a host memfd
  (`physmem_fd=` fd-passing), exposed to guest userspace as
  `/dev/sprout-shm` (kernel device in the uml-kernel port), with the
  exec ring carved out of the LAST 2 MiB of physmem. Three pieces
  were needed beyond the wave-2 insight:
  1. **memblock-reserve the ring tail** (kernel `physmem.c`): without
     it the buddy allocator freely hands ring pages to guest
     userspace/page-cache and the host ring is silently corrupted by
     unrelated guest memory.
  2. **`remap_pfn_range` via `virt_to_page(uml_physmem + phys)`**
     (kernel `sprout_shm.c`): the driver must remap the pfn of the
     page that the kernel's own MAP_SHARED linear mapping uses —
     that is what makes agent mmap genuinely share memfd pages with
     the host holder (earlier attempt remapped the raw file offset
     and the guest saw zeros/hangs).
  3. **Publish the exact physmem size**: UML strips every
     kernel-consumed arg (`mem=`, `rw`, `ncpus=`…) from
     `/proc/cmdline`, so the agent cannot learn RAM size that way,
     and `/proc/meminfo` is short by the kernel reserve. The device
     publishes it via `read()` (8-byte LE u64); the agent maps the
     ring at `size - 2MiB`.
  Measured: ring ping (holder↔agent) 8–11ms — dominated by the
  agent's 4ms idle poll cap and guest fork; exec ≈ files-transport
  (guest fork/exec floor) but with a deterministic, zero-IO
  carrier. Holder teardown on `down` kills the memfd holder
  (earlier leak) and clears `ring.sock`.
  Perf work for later rungs: pre-forked ring workers / vfork to
  cut the two forks per exec.

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
