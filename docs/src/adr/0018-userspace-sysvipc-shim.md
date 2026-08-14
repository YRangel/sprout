# ADR-0018: userspace SysV IPC emulation for box64/box32 guests

Status: Accepted (shipped 2026-08-14)

## Context

Steam's Linux client uses SysV IPC objects (semaphores via `semget`/`semop`,
SHM via `shmget`/`shmat`) in tier0: `threadtools.cpp` asserts on
`semget()` when `Function not implemented` (= ENOSYS). On Android stock
GKI kernels `CONFIG_SYSVIPC=n` and the native SysV syscalls return
ENOSYS, so Steam died at startup before any UI could ever appear.

proot's answer is `--sysvipc`: the tracer intercepts the guest's sysv
calls and emulates the registry in userspace. sprout's structurally
different hot path (LD_PRELOAD interposer: `PTRACE_CONT` for dynamic
guests, no syscall stops on the fast path) can NOT reuse that design — a
box64-emulated i386 steam never makes the aarch64 syscalls onto which a
supervisor would attach its registry.

## Decision

**Preload a guest-ABI DSO (`libsprout-sysvipc.so`) into every wrapped
guest**, exporting the libc-visible wrappers of SysV IPC (`semget`,
`semop`, `semctl`, `shmget`, `shmctl`, `shmat`, `shmdt`). Objects live as
files under `/tmp/sprout-sysvipc/` so independent processes in separate
sprout sessions map the same backing state.

Two gates decide whether the shim is injected (both reuse ADR-0017's
userspace binfmt sniff):

1. **Launcher gate** (`sprout-cli/main.rs`): after the binfmt sniff
   (EM_386 or EM_X86_64), if the user hasn't overridden the emulator,
   set `BOX64_LD_PRELOAD=<rootfs>/usr/lib/sprout-sysvipc/{i386,x86_64}/
   libsprout-sysvipc.so`. box64 v0.4.3 reads `BOX64_LD_PRELOAD` even for
   box32 children (empirically confirmed — there is NO `BOX32_LD_PRELOAD`
   in this build's string table).
2. **Chain gate** (`sprout_preload.c::sp_binfmt_maybe_exec`): same sniff
   + same injection for any nested exec (steam.sh → steam client ELF),
   since the launcher gate only covers program 0.

`SPROUT_SYSVIPC_OFF=1` suppresses the injection entirely.

Backend: **fd + pread/pwrite + flock(2)** (NOT mmap-of-file). Absolute
file offsets are view-independent across guest ABIs; a page-mapped view
shift exists in box32 v0.4.3's wrapped mmap(2) on tiny maps and skewed
header fields between x86_64 and i386 views of the same backing file.
The critical-section ordering is flock(2) LOCK_EX on the object's fd,
and blocking `semop P` polls + nanosleep backoffs around that lock — no
process-shared futex needed.

Object ids are deterministic `(itype<<28) | (key & 0x0FFFFFFF)`, so any
fresh opener resolves `semctl(id, GETVAL)` without a central registry.
`IPC_PRIVATE` (key=0) allocates the next id cross-process from a shared
counter file (flock-protected increment); the private file path names
itself by the slot, so later openers resolve identically.

## Consequences

- Steam's tier0 semaphore-creation assertion is gone; steam now reaches
  X11 window creation, cache validation, and the steamwebhelper spawn.
  The remaining crash is a box32-internal breakpad/dladdr path (upstream
  issue #1780 — box32 cannot run steam's CEF-based webhelper stably on
  ANY host); out of scope for sprout.
- Full batteries: `bench/flags-matrix.sh` 25/25, `flags-matrix-extended.sh`
  73/73, clippy clean, `install.sh --verify` green.

## Constraints that shaped it

- **No `.text` patching of in-use binaries** (ADR-0003) — shim rides the
  PRELOAD chain, never edits.
- **Guest-visible artifacts live INSIDE the rootfs** (`/usr/lib/
  sprout-sysvipc/`) so every wrapping box64 process in any session maps
  the same backing dir.
- **box32's wrapped 32-bit libc is thin**: `pthread_mutexattr_setrobust`
  is absent (`PltResolver32: Symbol ... not found`), and `pwrite@GLIBC_2.1`
  is absent too. Helpers do lseek+read/write and the i386 shim skips
  robust-owner-death logic that x86_64 gets to keep.
- **ABI stability**: the on-disk header must read identically from BOTH
  guest PC domains; `pthread_mutex_t` is 40 bytes on x86_64 and 24 bytes
  on i386 — a direct process-shared pthread_mutex inside the file header
  was rejected after it mis-fired on a real steam pairing (glibc
  `___pthread_mutex_lock: mutex->__data.__owner == 0` abort). flock moves
  exclusion out of the file into the kernel's lock registry, which is
  perspective-invariant.
- **v1 gaps** (documented once-per-process): `SEM_UNDO` on semop is a
  no-op (`[sysvipc] SEM_UNDO=1 passing NOOP (v1)`); message queues are
  ENOSYS; shm attach bookkeeping after unclean exit is closest-effort
  (header carries creator pid but there's no steal-on-death logic yet).

## Validation (banked, 2026-08-14, Termux)

- Unit (semtest.c, both arches, fresh dir): 5/5 PASS on each
  (GETVAL=5 after SETVAL=5; after-P=3 after-P(-2); after-V=4 after-V(+1);
  RMID-ok; private-ok)
- Fork-shared P(-1)/V(+1) across two processes (semfork.c, x86_64):
  PASS (parent raises after 700ms; child acquires after stagger;
  `wait(child)=0`)
- Cross-ABI pairing (semxab.c, waiter=i386 × poster=x86_64, one key,
  `sleep 2` sequencing): PASS both orders (poster rc=0, waiter rc=0)
- Steam client (Debian trixie, steam-installer:amd64, downloaded i386
  runtime): no longer dies at tier0 `semget ENOSYS` — the log shows
  `[sysvipc] SEM_UNDO=1 passing NOOP (v1)` (= shim intercepted) and
  continues through box64's CEF lane where it then aborts inside its own
  dladdr code (upstream bug, not sysvipc).
