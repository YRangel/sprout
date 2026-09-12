# ADR-0025: Performance & second-universe arc — SYSEMU, ring capability, virtio devices

## Status

Accepted 2026-09-11. Implementation tracking: tasks sprout#13-#21. Research
basis: `docs/src/research/2026-09-architecture-limitations.md` (committed
with live-probe receipts).

## Context

0.6.1 closed the correctness defects in the bridge (journal replay, durable
mounts). What remained were the four load-bearing risks: (1) the ptrace
cliff (2 stops/syscall ≈ 130× native), (2) split-brain fs (shadow table +
journal vs guest mount table, hostfs with zero host→guest invalidation),
(3) the un-upstreamed kernel fork, (4) bridge auth permanently fail-open.
Plus the "second universe" trade-off: no network, no GPU, no devices.

Deep research (web + local kernel tree + live probes) changed the map:

- **PTRACE_SYSEMU works on this host** (arm64 ≥5.3; phone1 = 6.12.23;
  live probe rc=0 with clean entry-stop). One stop per syscall instead of
  two; the syscall is cancelled, not executed. This is exactly the
  mechanism UML/arm64 itself uses (`arch/um/os-Linux/skas/process.c`).
- **hostfs mmap is fine on the rebuilt kernel** (read+write verified,
  writeback reaches the host file). The rung-3 SEGV was the stale Sep-8
  prebuilt kernel only. Remaining defect is coherence: hostfs page cache
  never revalidates host-side changes.
- **`arch/um/drivers/virtio_uml.c` is a generic vhost-user client** that
  already shares the whole guest physmem memfd with the daemon
  (`VHOST_USER_SET_MEM_TABLE`, fd passing). Any vhost-user device works
  with `virtio_uml.device=<socket>:<virtio_id>` — fs (26), net (1), etc.
  No guest networking, no KVM, no tun required.
- **Rootless host daemons exist for every device class**: virtiofsd
  (rust-vmm, static Rust), passt (C, rootless NAT with `--vhost-user`),
  virgl_test_server_android (Termux; vtest over TCP once net exists).
- 9p is a dead end on this host (cross-lane unix dead; no slirp in tree;
  gofer abandoned 9P for LISAFS). virtio-fs dominates it on semantics
  (FUSE invalidation, xattrs, POSIX locks, cache modes) AND effort.
- memfd RAM is lazy — the 2 GB guest reservation is virtual, not real.

## Decision

### D1. Supervisor: PTRACE_SYSCALL → PTRACE_SYSEMU (perf flagship)

The ptrace supervisor intercepts at syscall ENTRY only, with the syscall
cancelled — one stop per syscall instead of two. Syscalls whose RESULT we
must observe (openat fd returns, getcwd, readlink) are executed under
PTRACE_SYSCALL selectively and the tracee is returned to SYSEMU
immediately after. Fallback: if SYSEMU is rejected at runtime (older host
kernels), keep today's PTRACE_SYSCALL path (runtime probe, no config).

Expected: ~2× fewer stops on the fallback lane. Hard wall that remains:
no syscall filtering without seccomp (proven dead on this host), so every
syscall still costs one stop. This is the platform ceiling.

Correction recorded: demotion-to-guest is a SEMANTICS fix, not a speed
fix — UML pays the same SYSEMU stop per guest syscall.

### D2. Bridge auth: token as ring-header capability

The session token moves into the ring header (holder writes at init;
agent reads the shm it already maps). Only processes that can map the
memfd can read it — no file provisioning, no bootstrap window, no
16-vs-17-byte bug. Companion: SO_PEERCRED uid checks on `ring.sock` and
`shadow.ctl` accept paths. The `provision_token` files path is deleted.

### D3. Guest lifecycle: dead-man switch + sync timer

The holder stamps a heartbeat counter into the ring header. The agent's
ring_loop watches it: stale >30 s → `sync` + `reboot(POWER_OFF)`.
Orphaned guests self-terminate cleanly instead of being SIGKILLed with a
dirty page cache. Plus a 10 s guest-side sync timer in the agent.

### D4. Shadow table hygiene

Compaction on commit when REMOVED entries exceed 50% (rebuild in place;
readers are seqlock-guarded so a gen bump is safe). `SPROUT_SHADOW_STRICT=1`
selects fail-closed behavior for debugging; the `ping` ctl reply gains a
corruption/retry counter.

### D5. hostfs coherence: flush-on-notify now, virtio-fs as the answer

Short-term: holder doorbells the agent on known host-side share writes;
agent drops the affected guest page-cache (`sync` + `drop_caches` scoped
to the share). Closes the stale-read window for "host drops a file,
guest execs it".

The real fs: **virtio-fs over virtio_uml**. `virtiofsd` built on-device
(Termux cargo; rust-vmm stack has Android support via the AVF/crosvm
lineage) with sandboxing disabled (seccomp is host-blocked); kernel
`CONFIG_VIRTIO_FS=y`; cmdline `virtio_uml.device=<sock>:26`. Delivers
FUSE invalidation (true host→guest coherence), xattrs, POSIX locks,
cache modes, and page-cache mmap (verified pattern from hostfs).
hostfs stays for the agent share dir (bootstrap, agent binary, logs).

### D6. Networking: passt over virtio_uml

passt (`--vhost-user`) as a vhost-user-net daemon (VIRTIO_ID_NET=1) gives
the guest rootless NAT. Unblocks docker-in-guest, package managers with
real netns semantics, X11-over-TCP to termux-x11, and virgl vtest over
TCP (guest Mesa virpipe → virgl_test_server_android) — GPU acceleration
with NO new kernel device.

### D7. Split-brain: journal → desired-state SET sync

Replay becomes controller-style reconciliation: one ring op carries the
full desired bind set; the agent mounts what's missing, unmounts what's
extra, and replies with a mount-table digest. Idempotent, self-healing,
single round-trip. When virtio-fs lands, guest-initiated mounts become
natural and the shadow table degrades to a cache of the guest mount table
with generation-based invalidation — one source of truth.

### D8. DAX endgame (documented, not scheduled)

True shared-page mmap coherence needs virtio-fs DAX: a second vhost-user
memory region mapped as guest device memory. virtio_uml currently
advertises one region (physmem); extending it means mapping the DAX
window into the UML physical address space. Incremental kernel work on
top of D5, only if page-cache mmap proves insufficient in practice.

### D9. Kernel fork: LTS discipline

Track linux-stable LTS tags only; our delta stays minimal
(`sprout_shm.c` + fragment, kept as a format-patch series on top of the
zalexdev tree). Upstreaming the arm64 port is the right long-term fix,
deferred to post-1.0.

## Consequences

- Fallback lane roughly doubles in speed; the 130× figure in docs must be
  re-measured and updated (task #21 bench matrix).
- The guest becomes a real computer: coherent fs, networking, GPU path —
  all rootless, all through the one virtio_uml mechanism.
- Bridge ops gain a real capability check; threat model doc updates.
- ADR-0024's journal section is superseded by D7 (intent rows → desired
  state); ADR-0023's rung-3 note is amended (mmap works).
- New host-side runtime deps (virtiofsd, passt) are optional: `up`
  degrades to hostfs/no-net when the daemons are absent.
