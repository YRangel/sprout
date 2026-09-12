# Architecture limitations — research & remediation (2026-09-11)

Deep-research pass over the four load-bearing risks identified after 0.6.1,
plus the fixable 0.6.x gaps. Each item: finding, evidence, fix, effort, verdict.
Sources: local kernel tree (`~/uml-kernel/linux-um-arm64-shallow`), live on-device
probes (phone1, HyperOS SDK36, host kernel 6.12.23-android16), virtio-fs docs,
gVisor platform posts, LWN, kernel mailing lists.

## R1. hostfs coherence & mmap — PARTIALLY DISSOLVED (rung-3 unblocked)

**Finding 1: the rung-3 mmap SEGV is gone on the rebuilt kernel.**
Live probe (2026-09-11, LLVM=1 defconfig build): `mmap()` of a hostfs file
succeeds for read AND write; `msync`/flush propagates guest writes to the
host file. The SEGV was an artifact of the Sep-8 prebuilt kernel, not of
hostfs. Driver side this is consistent: `hostfs_file_fops` has
`.mmap_prepare = generic_file_mmap_prepare` and full writeback a_ops
(`writepages/write_begin/write_end/dirty_folio`).

**Finding 2: host→guest coherence is still absent by design.**
Guest page cache never revalidates host-side changes: a host-modified file
reads STALE in the guest indefinitely (until umount or drop_caches).
Guest→host is fine (writeback). hostfs has no invalidation protocol and
upstream shows no interest in adding one.

**Fix A (recommended, medium effort): virtio-fs over `virtio_uml`.**
The kernel tree already has everything guest-side:
- `arch/um/drivers/virtio_uml.c` — generic vhost-user **client**; cmdline:
  `virtio_uml.device=<socket>:<virtio_id>`; `VIRTIO_ID_FS = 26`
  (`include/uapi/linux/virtio_ids.h`).
- `CONFIG_VIRTIO_FS` (fs/fuse/Kconfig) — FUSE-protocol fs with real
  coherence: invalidation notifications, `cache=auto|always|never` modes,
  xattrs, POSIX locks, `announce-submounts`, inotify-driven host→guest
  invalidation in the daemon.
- Host daemon: `virtiofsd` (rust-vmm, Rust, statically linkable; CI
  publishes static builds). Runs unprivileged as a vhost-user server on a
  unix socket; sandboxing (seccomp/namespace) must be disabled on Android —
  verify `--seccomp`/`--sandbox` flags during bring-up.
- No guest networking required (unlike 9p, see Fix B). mmap works via the
  FUSE writeback-cache modes without DAX (DAX only adds *shared-page* mmap,
  which needs KVM — irrelevant here; page-cache mmap is what we verified
  with hostfs and what virtio-fs provides coherently).

**Fix B (rejected): 9p.** `net/9p` + `p9_unix_trans`/`p9_tcp_trans` exist,
but: unix transport needs a guest-side unix socket (cross-lane unix is
proven dead on this host), tcp transport needs guest networking (no tun,
no slirp transport in this tree). The only remaining path is a custom 9p
transport over our SHM ring — real kernel work for worse semantics than
virtio-fs. Also: gVisor's gofer moved to LISAFS (not 9P2000.L), so the
best-known userspace server is off the table; `rs9p` (Rust 9P2000.L
server crate) exists but Fix A dominates it.

**Fix C (cheap mitigation, ship now): explicit guest cache flush.**
On host-side writes to share dirs (holder knows them), doorbell the agent
→ agent `echo 1 > /proc/sys/vm/drop_caches` (or `sync` + targeted
`posix_fadvise(DONTNEED)` equivalents) before re-reading. Coarse but closes
the stale-read window for the common "host drops a file, guest runs it"
flow.

**Verdict:** keep hostfs for the agent share dir (it works, mmap included);
plan virtio-fs as the user-facing bind/mount fs for 0.7. Effort: days.

## R2. ptrace cliff for statics — HALVED: PTRACE_SYSEMU works on this host

**Finding:** arm64 gained `PTRACE_SYSEMU` in 5.3; the zalexdev port relies
on it (`arch/um/os-Linux/skas/process.c`: "arm64 hosts older than 5.3 flip
it to 0"). Live probe on phone1 (host 6.12.23): `PTRACE_SYSEMU` rc=0, clean
syscall-entry stop (SIGTRAP) and continue. SYSEMU = ONE stop per syscall
(entry only), syscall cancelled — vs TWO stops with PTRACE_SYSCALL.

**Fix (cheap, high value): switch sprout-ptrace supervisor to SYSEMU.**
Entry-stop-only interception with the syscall cancelled; for the handful of
syscalls whose RESULT we need (openat fd, getcwd), run that one syscall
under PTRACE_SYSCALL and resume SYSEMU after. Expected ≈2× fewer stops on
the fallback lane (130× native → ~65×).

**Remaining wall:** no way to filter WHICH syscalls stop (seccomp is
dead on this host, proven) — every syscall pays one stop, period.
This is a hard platform boundary.

**Correction to the demotion story:** UML intercepts guest syscalls with
the same SYSEMU mechanism — a static binary demoted to the guest pays the
SAME per-syscall stop cost as under our supervisor. Demotion's value is
kernel SEMANTICS (real pid1, /proc, cgroups), not speed. The speed fix for
statics is SYSEMU in our own supervisor, not migration.

## R3. un-upstreamed kernel fork — manage, don't solve

**Finding:** upstream UML is still x86-only (uml-devel, ongoing). The
arm64 port is zalexdev's tree and nothing else public. Our own delta is
tiny: `arch/um/drivers/sprout_shm.c` + `sprout-uml.fragment` — the
maintenance risk lives almost entirely in the port itself.

**Options:**
- (a) *Rebase discipline (adopt now):* track linux-stable **LTS** tags only
  (6.12, then 6.18), not mainline; document the rebase checklist
  (defconfig recipe + fragment + module build + boot gate). Guest kernel
  does not need to chase releases — security surface of a sidecar guest is
  modest.
- (b) *Upstream the port (long-term, the right fix):* a multi-month effort
  needing a champion and a test story. Out of 0.x scope; revisit at 1.0
  if the port proves stable in the field.
- (c) *Submodule/vendor zalexdev's tree* with our delta as patches on top
  (`git format-patch` series), so any future maintainer change is a
  mechanical 3-way rebase.

**Verdict:** (a)+(c) now, (b) later if adoption justifies.

## R4. kill-dirty corruption — design fix available

**Finding:** corruption window = guest page cache not yet written back when
linux.uml dies. With mmap/writeback verified working (R1), the window is
only *un-flushed* dirty pages.

**Fixes (stack them):**
- (a) *Dead-man switch (recommended):* holder stamps a heartbeat counter
  into the ring header (it already owns the shm); agent's ring_loop checks
  staleness each poll; stale >30 s → agent `sync` + `reboot(POWER_OFF)`.
  Orphaned guests self-terminate cleanly instead of being SIGKILLed dirty.
  Small change, kills the whole bug class.
- (b) Guest-side periodic `sync` timer in the agent (10 s).
- (c) Optional `ubd0s` (O_SYNC backing) mode for paranoid workloads —
  slower, crash-safe.
- (d) ext4 `commit=1` on the backing mount.

## R5. bridge auth is off — clean fix available

**Finding:** token provisioning via guest file (`/run/sprout/session.token`)
has a bootstrap window AND a length-check bug (16 vs 17 bytes) that leaves
`agent_tok=0` → permanent fail-open.

**Fix (recommended):** pass the token through the **ring header**. The
holder already writes the ring header at init; the agent already maps the
same shm. A 16-byte token field there is a pure capability: only processes
that can map the memfd (holder, kernel, agent) can read it. No file
provisioning, no bootstrap window, no token on disk. Companion hardening:
SO_PEERCRED uid check on `ring.sock` and `shadow.ctl` accept paths.
Effort: ~1 day.

## R6. split-brain reconciliation — reframe as desired-state sync

**Finding:** post-0.6.1 the journal holds *durable desired state* (mount
rows) but replay is still per-row intent processing with no convergence
guarantee.

**Fix (medium): controller-style full reconciliation at `up`.**
Holder computes the full desired bind set → ONE ring op carries the set →
agent mounts everything missing AND umounts everything present-but-undesired
→ replies with the resulting mount table digest. Idempotent, self-healing
after partial failures, removes intent-vs-state ambiguity. The current
replay loop is 80% of this; the delta is a SET op + the umount-diff side.
Long-term, if virtio-fs (R1) lands, guest-initiated mounts become natural
and the shadow table degrades to a pure cache of the guest mount table
with generation-based invalidation — one source of truth.

## R7. smaller confirmed items

- **Tombstone leak (shadow table):** compact on commit when REMOVED > 50%
  (rebuild entries in place; readers are seqlock-guarded, brief gen bump is
  safe). Trivial.
- **2 GB memfd is NOT 2 GB RAM:** memfd/shmem allocates lazily; only
  touched pages consume. Virtual reservation only. Non-issue — but document
  it, because it LOOKS alarming in `/proc/<pid>/status`.
- **Fail-open observability:** add `SPROUT_SHADOW_STRICT=1` (fail-closed for
  debugging) and a corruption/seqlock-retry counter in the `ping` reply.
- **Whitelist:** PROC_READ/FILE_META handlers exist agent-side with no
  callers — wire fast-lane callers behind env flags.

## Recommended execution order

| # | Item | Effort | Payoff |
|---|------|--------|--------|
| 1 | SYSEMU supervisor (R2) | days | ~2× fallback lane |
| 2 | ring-header token (R5) | ~1 day | closes auth hole |
| 3 | dead-man switch + sync timer (R4) | days | kills corruption class |
| 4 | tombstone compaction (R7) | hours | correctness hygiene |
| 5 | guest cache-flush mitigation (R1C) | hours | stale-read window |
| 6 | virtio-fs via virtio_uml (R1A) | 1-2 wks | the real fs answer |
| 7 | desired-state SET sync (R6) | days | kills split-brain class |
| 8 | LTS rebase checklist + patch series (R3) | process | maintenance |
