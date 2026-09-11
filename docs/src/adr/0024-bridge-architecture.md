# ADR-0024: Per-syscall guest bridge — the four-layer execution ladder

## Status

Implemented in 0.6.0: layers L0-L2 and the session-owner/journal plumbing
landed and gate-tested; L3 (demotion) ships as a documented stub. 0.6.1:
the guest hostfs-mount replay path is fixed end-to-end and journal mounts
are durable (see CHANGELOG 0.6.1 for the six root causes).

## Context

sprout's mission: drop-in proot replacement — full Linux userspace on Android
without root, at native speed where possible, with real-kernel semantics where
required. ADR-0023 shipped the UML sidecar (rung 3: shared-physmem ring).
Research and on-device probing (HyperOS SDK36, kernel 6.12.23-android16,
untrusted_app_27, SELinux enforcing) established:

- seccomp filter install: **EPERM** (verified) → Systrap/SUD-class fast
  interception is impossible for us
- `PR_SET_SYSCALL_USER_DISPATCH`: **EINVAL** (not implemented in this arm64
  build; verified)
- `pidfd_open`/`pidfd_getfd`/`pidfd_send_signal`: **working** unprivileged
  (verified live)
- `brk #imm` → SIGTRAP → handler with full `pt_regs` via ucontext, pc-bump
  resume: **working** (verified live) → per-syscall in-process interception
  without ptrace is possible
- ptrace stop cost: ~22 µs/syscall (measured) — correct but too slow as a
  universal path
- ring RTT: 8–11 ms (measured) — fine for rare ops, wrong for hot path
- AF_UNIX across kernels through shared paths: **dead both directions**
  (verified: host connect → ECONNREFUSED with live guest listener; guest
  connect → hangs forever). Relays are mandatory.
- hostfs file I/O ≈ image I/O (both UML-trap-bound); hostfs mmap = copy, not
  shared pages (measured)
- guest RAM = host memfd (rung 3): the only truly shared memory region

## Axiom

**One process, one kernel. Semantics are composable; residence is not.**
A process is scheduled, signalled, and futex-waited by exactly one kernel.
What we can compose is *syscall semantics*: any syscall can be answered by
whichever kernel owns the truth for it. The architecture never shares kernel
state; it shares memory and routes syscalls.

## 1. The four-layer ladder

| Layer | Mechanism | Cost/op | Covers |
|---|---|---|---|
| 0 | LD_PRELOAD interposer + shadow memfd | ~1 ns (empty table: one acquire-load) | ~95% of all processes |
| 1 | brk-trap interposer (statics) | ~1 µs/trap | static binaries |
| 2 | whitelisted syscall → ring → guest agent | ~8 ms/op (rare by design) | mount-class guest-truth ops |
| 3 | demotion: process runs inside UML | ~1.5× syscall tax | kernel-native workloads |

Layer selection is **deterministic**: function of (syscall number, hostfs
classification of path args, guest state). No heuristics. Failure to service at
layer N falls to N+1 or fail-opens to host truth — never fails the process.

## 2. Shadow memfd — full specification

- Backing: `memfd_create("sprout-shadow")`, one per **rootfs** (not per
  process, not per session). Live at `~/.sprout/shadow/<roothash>.memfd`,
  referenced by fd passed as `SPROUT_SHADOW_FD` (CLOEXEC=0) plus the env name
  so exec chains re-resolve it.
- Layout:
  - `[0..8]` magic `SPRSHDW1`
  - `[8..16]` `gen` u64 seqlock (odd = write in progress)
  - `[16..24]` `crc` u32 CRC32 of entries region
  - `[24..28]` `count` u32
  - `[28..36]` `heartbeat_ns` u64 (CLOCK_MONOTONIC at last write/heartbeat)
  - `[36..64]` counters: shadow reads, ring redirects, demotions, fail-opens
  - `[64..]` entries × 64 B: `{type u8, state u8, crc u32, src_hash u64,
    dst_hash u64, guest_ctx u16, flags u16, reserved}`; types:
    `BIND_MOUNT, PROC_OVERRIDE, NS_FAKE, DEVNODE_FAKE`; states:
    `VALID, INFLIGHT, FAILED, REMOVED` (tombstone)
- **Writer authority (gap closure — multi-session)**: exactly one writer per
  table. First session claims via `flock()` on
  `~/.sprout/shadow/<roothash>.lock`. Later sessions attach the memfd
  read-only and submit writes over the control unix socket
  `~/.sprout/shadow/<roothash>.ctl` (owner serializes them). Owner death:
  lock is released by the kernel; the next session steals the lock and
  **rebuilds** the table by replaying the journal (§10). No stale writer can
  exist: flock + rebuild.
- **Reader staleness rule**: if `heartbeat_ns` is older than 2 s (writer
  keeps it fresh at 250 ms intervals even when idle), readers treat the whole
  table as invalid and fail-open to host truth. A frozen table can never
  silently misdirect a path.
- **Writes**: single writer process; children map read-only (`mprotect(PROT_READ)`).
  Write cycle: build entries → memcpy → CRC → `__atomic_store_n(&gen, gen+1,
  __ATOMIC_RELEASE)`. Readers: load-acquire gen; odd gen → bounded spin →
  fail-open.
- **Fail-open invariant**: corrupt CRC, missing fd, stale heartbeat, bad magic
  → interposer passes every syscall to the host unshadowed and logs once.
  The shadow can degrade; it can never lie halfway.

## 3. Layer 0 — fast lane (default)

Every dynamic process gets `LD_PRELOAD=sprout-shadow.so` (already sprout's
mechanism). Interposer duties, in order:

1. Validate shadow fd (magic, CRC, heartbeat). Invalid → pass-through mode.
2. For path-taking syscalls with non-empty table: hash path prefix, look up:
   - `BIND_MOUNT` hit → rewrite path (host-side, pure memory op)
   - `PROC_OVERRIDE` hit on `/proc/...` → serve from shadow/guest per §8
   - mount-class whitelist hit → Layer 2 redirect
3. Everything else: pass through at native cost.

### Resolution order (gap closure — proc/identity truth)

`shadow → guest (only for shadowed entries) → host truth`, with one absolute
exception: **identity class files are never shadowed** — `/proc/cpuinfo`,
`/proc/meminfo`, `/proc/stat`, `sysconf(_SC_NPROCESSORS*)`, `uname(2)`,
`/proc/sys/kernel/*` always answer host truth. The process runs on the host;
it must see the host's compute identity. Guest-truth proc reads are limited
to `/proc/mounts`, `/proc/mountinfo`, `/proc/self/mounts*` (when shadowed),
and explicitly registered PROC_OVERRIDE entries.

### Capability illusion (gap closure)

`capget(2)` and `/proc/self/status` `Cap*` fields are overridden by the
interposer when the session table is non-empty: report the virtual set
`{CAP_SYS_ADMIN, CAP_MKNOD, CAP_CHOWN, CAP_DAC_OVERRIDE, CAP_SYS_RESOURCE}`.
Rationale: programs that pre-check capabilities before calling mount would
otherwise refuse ops the bridge can actually perform. This is a documented,
presence-gated lie (table empty → real host caps reported).

### Inheritance rules (gap closure)

- `fork()` (no exec): mapping and fd survive; child is a reader. Fine.
- `execve()` dynamic: `SPROUT_SHADOW_FD` (CLOEXEC=0) + env persist; interposer
  re-validates on first use.
- `execve()` setuid/setgid host binary: loader strips LD_PRELOAD → child is
  unshadowed by construction (host truth). Log once at debug. Accepted.
- Static exec inside a dynamic tree: interposer's exec hook (existing
  `execve` interposition) notifies the session owner, which performs the
  Layer-1 install on the fresh image before handing control back (see §4).

## 4. Layer 1 — brk-trap interposer for statics (verified mechanism)

For a static binary there is no loader and no LD_PRELOAD. The session owner
(supervisor) installs interception **per static exec**:

1. `PTRACE_TRACEME` + execve via launcher; stop at exec.
2. `PTRACE_SETOPTIONS` with **`PTRACE_O_TRACEEXEC` only** — the supervisor
   stays attached for the process lifetime but pays **zero per-syscall
   stops**; it reacts solely to execve boundaries (chained statics stay
   covered).
3. At each exec-stop: read `/proc/pid/maps` + ELF header → locate executable
   file-backed segments → `mprotect` RW (via one ptrace-rigged `mprotect`
   call) → scan for `svc #0` (0xd4000001) → `PTRACE_POKETEXT` each into
   `brk #imm` (imm = opcode index) → write handler page (mmap'd via rigged
   call at a randomized address) containing: SIGTRAP handler (raw-syscall
   only code), opcode table, lazy shadow-memfd resolver from
   `SPROUT_SHADOW_FD` env → install `sigaltstack` + `rt_sigaction(SIGTRAP)`
   via rigged syscalls → restore text RO → `PTRACE_CONT`.
4. Runtime: `brk` → SIGTRAP → handler reads `uc_mcontext.regs` (x8 = nr,
   x0–x5 args): consult shadow (read-only mapping) → emulate (path rewrite +
   real host syscall executed from the handler) or Layer-2 redirect → write
   result into `regs[0]`, `pc += 4`, sigreturn. ~1 µs.
5. Nothing claims the trap (nr not in table): handler performs the original
   `svc` itself from the handler context and returns the real result — the
   patch is observationally neutral for unshadowed syscalls.

Cost: ~1 µs per trapped syscall vs 22 µs ptrace stop (~20×), zero cost for
untouched code. Verified primitives: brk survival, ucontext register write,
pc bump — all probed on-device this session.

## 5. Layer 2 — whitelisted syscalls over the ring

### Whitelist (closed in the supervisor; data-driven table, not open proxying)

| Class | Op | Condition |
|---|---|---|
| `mount`/`umount2`/`pivot_root` on hostfs-visible src+tgt | `OP_MOUNT` | both paths classify as hostfs-root-contained |
| mount of tmpfs/proc/sys/devpts/overlay types | **never bridged** — real host result | content would be guest-RAM-only; bridging would lie |
| `mknod` regular file / meta ops on hostfs paths | `OP_FILE_META` | path contained; device nodes rejected |
| proc reads per §8 shadow entries | `OP_PROC_READ` | path ∈ `/proc/mounts*` or registered override |
| signals across lanes | `OP_SIGNAL_FWD`/`OP_SIGNAL_REV` | §7 |
| AF_UNIX reach | `OP_RELAY_UNIX` | §9 |
| demotion | `OP_EXEC_MIGRATE` | §6 trigger table |
| page export (future, futex/mmap) | `OP_PIN_EXPORT` | §11 |

### Agent confinement (gap closure — security)

The agent executes as guest root. Every op passes, **unconditionally**:

1. `realpath()` every path argument (kills symlink traversal),
2. containment check: resolved path must be under the guest's hostfs root
   mapping or inside the guest image; anything else → `EPERM` back,
3. session-token check: the session owner generates a random u64 token at
   `uml up`, delivered to the agent via guest env and held host-side; every
   ring request carries it; mismatch → drop. Other host processes sharing
   the memfd cannot forge service.
4. no pointer-chasing into host memory: requests are fixed-size bounded
   structs; payload lengths validated against slot size.

The kernel fork exposes only byte-exact primitives (ring, physmem fd). All
policy lives in the userspace agent — auditable, fuzzable, replaceable.

### Ring (unchanged from rung 3) + new opcodes

Physical: last 2 MiB of guest physmem (memblock-reserved in the fork);
8 slots × 60 KiB; 16-byte headers; `host_seq`/`guest_seq` doorbells.
Consumer side: agent prefork pool already exists; ring slots get a
spin-then-park wake (bounded spin ~200 µs after activity, then park; eventfd/
pipe-IRQ wake from holder — pipe-fd-as-UML-IRQ is the fork-side follow-up
that drops RTT from ~8 ms toward ~100 µs; not required for v1 correctness).

## 6. Layer 3 — demotion

### Trigger table (deterministic)

| Trigger | Detection | Action |
|---|---|---|
| hard-kernel syscall set (`bpf`, `kexec`, `unshare(CLONE_NEWNS\|NET)`, `clone` with ns flags, `setns` to non-file-backed ns) | interposer sees the call, host result would be EPERM, op not bridgable | demote |
| Layer-2 op returns failure class "needs guest-internal fs" | agent verdict | demote |
| bridge-op rate exceeds threshold (≥ 32 ops / 5 s window) | interposer counter | demote |
| program behaviorally tests namespace enforcement | observed EPERM-then-retry pattern on the hard set (2 strikes) | demote |
| explicit user policy (`--uml-demote` / conf) | config | demote |

### Mechanics

`OP_EXEC_MIGRATE` carries argv, envp (sanitized), cwd, and the fd table
inventory. Holder proxies inheritable host fds into the guest by opening the
same hostfs backing files agent-side (same-file trick; hostfs root makes
paths coincide). stdio:

- **pipes/regular files**: relayed over ring frames (existing exec I/O
  machinery) — works v1.
- **TTY**: v1 gate — demotion of TTY-attached processes returns a truthful
  "not yet" and the process stays on the fast lane with real host results
  (no silent breakage). v2 adds host-pty ↔ guest-pty master relay in the
  holder (same byte-pump as RELAY_UNIX). Until then, interactive shells that
  need the guest stay on `sprout uml exec` (already shipped, works).

Exit status and signals of a demoted process propagate back to the fast-lane
parent via the supervisor (it is the reaping parent; agent reports exit via
ring event stream).

## 7. Signals across lanes (verified primitives)

- fast → guest: interposer catches `kill`/`tgkill`; supervisor maps
  sprout-side pid → guest pid (pid table maintained at spawn/migrate);
  `OP_SIGNAL_FWD` → agent `kill(2)` inside the guest. Real guest delivery.
- guest → fast: agent sends `OP_SIGNAL_REV {host_pid, sig}`; supervisor
  (which opened a **pidfd** for every fast-lane process at spawn — never raw
  pids) delivers via `pidfd_send_signal(2)`. No pid-recycling race in either
  direction.
- `wait()` semantics for demoted children: supervisor reaps; guest exit
  events arrive on the ring event stream; supervisor synthesizes wait status.

## 8. `/proc` merge policy (gap closure — one rule)

- `self/*`: host truth, always. The process is a host process.
- identity class (cpuinfo, meminfo, stat, uptime-adjacent, uname): host
  truth, never shadowed.
- mount tables (`/proc/mounts`, `/proc/mountinfo`, `/proc/self/mounts`):
  shadow-served when any BIND_MOUNT entry exists; content = host base lines +
  shadow entries rendered in kernel format. Consistent with `stat` by
  construction (same table).
- everything else: host truth unless a PROC_OVERRIDE entry exists (created
  only by explicit policy or Layer-2 outcomes).

## 9. AF_UNIX cross-lane (verified dead → relay is mandatory)

Guest `bind()` on a hostfs path yields a host-visible socket *inode* whose
queue lives in UML; host connect = ECONNREFUSED; reverse direction hangs.
Therefore: `OP_RELAY_UNIX {guest_path, host_path, dir}` — agent-side listener
proxy inside the guest + holder-side listener on the host path; connections
are pumped as ring/vsock streams. Semantics preserved: byte streams, connect/
accept; **not** preserved: `SO_PEERCRED` identities, `SCM_RIGHTS` fd passing
across the relay (documented deviation). Data-plane cost: one extra copy per
direction + transport.

## 10. Crash semantics — journal (jbd2 pattern)

- Every Layer-2 mutation writes an intent record to
  `~/.sprout/uml/<id>/journal` on hostfs **before** the ring issue; agent
  confirms completion; owner then flips the shadow entry INFLIGHT → VALID.
- Guest death: owner detects (holder child watch), marks table DEGRADED →
  readers fail-open to host truth (no fakery), `uml down` state cleaned.
- Next `uml up`: journal replay; ops are idempotent (mount → EBUSY-treated-
  as-success detection); unresolved intents marked FAILED and surfaced.
- At-least-once, never silently-divergent: the shadow cannot claim a state
  that the journal cannot reconstruct.

## 11. Futex across lanes (kernel-fork patch, owned by us)

Guest futex path in the sprout kernel fork: if the futex address lies in a
registered shared range (published by `sprout_shm`), the guest kernel
delegates the wait/wake to the **host** futex syscall on the physmem-mapped
address → both lanes' waiters land in one host-kernel queue. Native wake
latency, no relay. Guest-private futexes are unaffected (correct: no
cross-lane waiters on private memory). Cross-lane sync becomes an explicit
opt-in (shared arena), µs-class. Future companion: `OP_PIN_EXPORT` (pin
guest tmpfs pages via fork-internal GUP, publish PFN list, host maps
physmem offsets) → true shared-memory objects for mmap-class sharing.

## 12. Guest networking

Userspace NAT in the holder (passt/slirp pattern): UML virtio-net frames ↔
holder via vsock (µs-class, shipped transport); holder terminates TCP/UDP
with real host sockets per connection (SYN→socket, masquerade). Inbound via
port-forward table. No TAP, no /dev, no root. AF_UNIX is separate (§9).

## 13. Explicitly rejected (with evidence)

- **LKL / kernel-as-library**: no process model (single address space, no
  fork/exec/mm isolation) → cannot host the workloads that need the guest at
  all; ~2× per intercepted syscall (Salesforce production data) on exactly
  the hot paths we must not tax; second kernel fork to maintain; and the
  decisive point — its VFS/mount/socket objects are *equally invisible to the
  host kernel*, so it fixes none of the physics walls. Correct tool for
  userspace TCP stacks; wrong tool for a proot replacement. Revisit only as
  the holder's NAT engine if passt-style ever bottlenecks.
- **gVisor-style always-intercept**: ~100× measured class tax on everything;
  our mission is compatibility at native speed, not a security boundary.
- **seccomp-notify / filters / SUD**: EPERM / EINVAL on this platform
  (probed). Dead ends, documented for future devices.
- **Popcorn-style replicated kernel**: requires cross-kernel memory
  subsystems (userfaultfd cross-process, remote page faults) that Android
  SELinux forbids. The compose-semantics-not-residence axiom is the same
  insight at userspace cost.
- **Bridging guest-internal filesystems (tmpfs/overlay/devpts)**: content
  has no host object; a "successful" bridge would be a lie worse than EPERM.
  Hard-excluded; such workloads demote.

## 14. Final limits (the honest residue)

1. Guest tax ~1.5× for processes that genuinely live in the guest.
2. Bridge hop ~8 ms (→ ~100 µs with the fork IRQ follow-up) for guest-truth
   ops; rare by design; hot usage = demotion signal.
3. The shadow is a model, not the kernel: behaviorally-testing programs get
   host truth and demote. We compose semantics; we do not fake enforcement.

## 15. Acceptance matrix (each = a test with a receipt)

| # | Test | Layer | Pass criterion |
|---|---|---|---|
| T1 | `sp_statloop` with empty shadow | 0 | ≤ 1.02× native (one load overhead) |
| T2 | `sp_statloop` with 64 BIND_MOUNT entries | 0 | ≤ 1.10× native |
| T3 | static binary mount+open through brk lane | 1 | correct file visible, exit 0, < 5 ms total |
| T4 | `mount --bind` of hostfs dir from fast lane, UML up | 2 | open through shadowed path returns guest-written content; UML down → EPERM |
| T5 | kill across lanes both directions | 2 | delivery < 50 ms, no pid-recycle hits (pidfd) |
| T6 | `/proc/mounts` consistency vs `stat` | 0/2 | same st_dev/st_ino story for shadowed paths |
| T7 | demote a canned chown-heavy postinst workload | 3 | completes; exit status propagates; stdio relays |
| T8 | owner kill mid-session | — | readers fail-open within 2 s; no wrong-path writes |
| T9 | guest SIGKILL with INFLIGHT mount | — | journal replay on next up; no phantom entries |
| T10 | corruption fuzz of memfd (random bytes) | — | CRC catches; fail-open; no crash |
| T11 | two sessions, one rootfs | — | second session's mount visible to first (owner RPC) |
| T12 | full gates (`tests/uml.sh` + smoke + proot-compat) | — | 7/7 unchanged, plus new gates |

## 16. Implementation order

1. **shadow memfd + interposer reads** (T1, T2) — biggest payback, no UML dep
2. **ring ops MOUNT/FILE_META/PROC_READ + agent confinement + token** (T4, T6, T10)
3. **brk interposer for statics** (T3) — verified primitives only
4. **session owner: flock claim, ctl socket, heartbeat, journal** (T8, T9, T11)
5. **signals via pidfd table** (T5)
6. **demotion triggers + EXEC_MIGRATE + stdio relay** (T7)
7. **RELAY_UNIX** (AF_UNIX bridge, T4-ext)
8. **fork: futex rendezvous, pipe-IRQ ring wake, PIN_EXPORT** (perf pass)

Steps 1–4 are the v1 core; 5–7 complete the ladder; 8 is the performance
pass. Each step is independently shippable and gates-tested.

## ADR-0024 — implementation status (0.6.0)

| Layer | Landed in | Notes |
|---|---|---|
| L0 shadow memfd | `crates/sprout-cli/src/session_owner.rs` (writer, flock + 250 ms heartbeat + `shadow.ctl`), `crates/sprout-preload/csrc/sprout_shadow.c` (reader) | layout: 56-byte header + 28-byte entries (measured `sizeof`, not the stale comments); readers fail open on magic/gen/heartbeat/strtab mismatch (T8/T11 covered by `test_shadow.c`) |
| L1 brk lane | pre-existing supervisor lane | unchanged in 0.6.0 |
| L2 bridge ops | `uml/sprout-uml-agent.c` opcodes 0x03-0x09 (MOUNT, PROC_READ, FILE_META, SIGNAL_FWD/REV, RELAY_UNIX, EXEC_MIGRATE) | 16-byte auth header (u64 token + reserved); confinement via realpath+prefix; token bootstrap is fail-open until `/run/sprout/session.token` exists |
| L3 demotion | stub | detector only (`SPROUT_DEMOTE_ENABLED`), executor scheduled for 0.7 |
| session owner | holder binary | flock claim, heartbeat thread, ctl socket, journal append on bind/unbind |
| journal + replay | `crates/sprout-cli/src/journal.rs` + `journal_replay()` in `uml.rs` | intent rows appended on ctl bind/unbind; replayed into the guest via PROTO_MOUNT on `sprout uml up` |

Discovery paths, in order: `SPROUT_SHADOW_FILE` (mmap-able file, works under
Android SELinux) then `SPROUT_SHADOW_FD` (pidfd_getfd-fetched fd). The
kernel-side wake doorbell (write/poll/ioctl on `/dev/sprout-shm`) is in the
sprout-arm64 kernel fork, pending a kernel rebuild.

**Resolved in 0.6.1:** the hostfs-mount replay defect is fixed end-to-end
(journal bind -> down -> up -> guest mount visible with host content).
Six stacked root causes: (1) mount data must be share-relative (fsconfig-era
hostfs appends monolithic data to the `hostfs=` boot root); (2) a ring
socketpair echo bug returned the request as the response; (3) mount(2) could
wedge the single-threaded ring server (now fork+waitpid); (4) the response
capture loop never saw EOF (sv[1] now closed post-handler); (5) destination
confinement rejected guest mountpoints (kernel `hostfs=` already confines
the host path); (6) the CLI misparsed the holder's `[u32 total_len]` frame
wrapper as the status byte (phantom EIO=5). Journal semantics also changed:
mount rows are durable (re-applied every boot, consumed only by unbind),
the holder seeds the shadow table from journal rows at start, and re-bind
is replace semantics. See CHANGELOG 0.6.1.
