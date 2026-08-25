# 0021: FEX-Emu SysV IPC via in-source shim

Date: 2026-08-24
Status: accepted (experimental — local patch, not upstreamed)

## Context

Steam's `tier0` startup requires SysV semaphores on the **i386** ABI.
Under sprout + box64/box32 that is covered by the LD_PRELOAD shim in
ADR-0018 (guest loader pulls `libsprout-sysvipc.so` in via
`BOX64_LD_PRELOAD`, `semget`/`semctl`/`semop` interpose at the libc
wrappers, object state lives in `$SPROUT_SYSVIPC_DIR` fd-backed files).

When we tried to lift the same steam install onto **FEX-Emu** (to escape
box32's GLX / dladdr walls), the preload approach broke down:

- FEX implements the guest syscall surface in `Source/Tools/LinuxEmulation/LinuxSyscalls`.
  For SysV it routes through `::syscall(SYSCALL_DEF(semget))` — a raw
  passthrough to the host kernel. An LD_PRELOAD DSO preloaded into the
  FEX **loader** process does not reach FEX's own internal `::syscall`
  call sites, so the shim never fires and `semget` returns ENOSYS.
- FEX exposes no runtime toggle for SysV emulation and upstream's answer
  to emulated IPC is "use a real VM".

## Decision

Patch FEX's own `x32/Semaphore.cpp` in place:

1. Add a userspace `SysVSemSpace`: process-internal `std::unordered_map`s
   keyed by `key_t` AND by synthetic semid, with per-object
   `std::mutex + std::condition_variable` and a monotonically-increasing
   version counter so `semop` waiters wake reliably under contention.
2. Implement `sysv_semget`, `sysv_semop`, `sysv_semtimedop`,
   `sysv_semctl_32` (IPC_SET/IPC_STAT/GETALL/SETALL/SETVAL/GETVAL/IPC_INFO/
   IPC_RMID/GETPID/GETNCNT/GETZCNT) on top of that table, honoring
   IPC_PRIVATE + IPC_EXCL. SEM_UNDO is documented as a no-op.
3. Rewire `REGISTER_SYSCALL_IMPL_X32(semget/semctl/semop/semtimedop)` and the
   `semctl` i386-mux lambda to route into the emulation instead of
   `::syscall` passthrough. MSG_/SHM_ remain passthrough — steam does not
   touch them via SysV, and keeping them raw preserves any future host
   where CONFIG_SYSVIPC=y.

The emulation is **per-FEX-process**, not cross-process persistent — steam
runs a single supervisor + client pair under one FEX instance, which this
covers. Anything needing cross-process SysV identity stays on the box64
shim path (ADR-0018) which is fd-backed and survives process boundaries.

## Consequences

- steam's `tier0` semaphore phase clears under FEX; the client proceeds
  past `CProcessEnvironmentManager is ready` where every prior lane
  (box32, arm64-native) had died.
- The patch is a guest-side fork-local change, not upstreamable as-is
  (FEX upstream deliberately does not emulate IPC). Carried as
  `~/projeto/FEX-sprout-final.bundle`.
- FEX's `RootFS` config knob is out of scope for this shim and its
  FEXServer launch protocol is currently unsolved — host-path ↔ guest-path
  drift through FEX (e.g. steam's "Couldn't chdir into the install path")
  is a separate open problem documented in STEAM-INSTALL.md §8.
- sprout itself is untouched: this is a consumer-side patch on the
  emulator FEX, in the layered model (sprout = path translation, FEX =
  ISA translation, shim = syscall emulation) each concern stays in its own
  layer.
