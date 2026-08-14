# ADR-0020: SysV shared-memory emulation via the libandroid-shmem protocol

## Status

Accepted, implemented.

## Context

Android has no SysV IPC (`shmget/shmat/shmdt/shmctl` hit the kernel
seccomp surface: `ENOSYS`/kill). mesa's software-present paths — most
visibly llvmpipe's Vulkan xcb WSI — allocate pixels with `shmget(2)` and
hand the integer shmid to the X server through a MIT-SHM `Attach` request.

Observed failure on Termux:X11 (vkmark llvmpipe diagnosed 2026-08-14):

- client `shmget` fails → mesa passes `shmid=0xFFFFFFFF` on the wire;
- termux-x11 replies `XShmAttach` → `Error 10=Access`,
  `CreatePixmap` → `Error 128=BadShmSeg`;
- the present-completion handshake (PresentIdleNotify + DRI3
  `FenceFromFD` triggers) never resolves;
- frame N≥2 `vkAcquireNextImageKHR` blocks forever. Under vkmark this
  means: `[clear] <default>:` then a hard-freeze with every thread
  parked in `futex_wait_queue`; under vkcube: zero-CPU hang at init.
  GPU (Turnip) + headless-swapchain lanes never used XShm, so they were
  unaffected (deliberately misleading early evidence).

proot survived this because termux's proot fork emulates the whole shm
family (`--sysvipc`) over the **libandroid-shmem protocol**: each process
owns an abstract unix socket at `/dev/shm/<sockid-hex>`, shmid = `(sockid
<< 16) | counter`, consumers connect + `send(shmid)` + receive
`key_t`+ashmem fd via SCM_RIGHTS, then `ASHMEM_GET_SIZE`+`mmap`.
termux-x11's server links libandroid-shmem and runs that hydration when
MIT-SHM Attach arrives — so a proot guest's XShm pixmaps become
server-mappable, zero-copy.

## Decision

Port the libandroid-shmem wire contract into `libsprout-core.so`
(`crates/sprout-preload/csrc/sprout_shm.inc`, included from
`sprout_preload.c`, all guests, both glibc and musl .so flavors):
interpose `shmget/shmat/shmdt/shmctl`. Segments are real ashmem regions
(`/dev/ashmem` + `ASHMEM_SET_NAME/SET_SIZE` ioctls) so the server's
`ASHMEM_GET_SIZE` check passes. Owner side binds the abstract socket +
one acceptor thread per process; remote attach (`shmid>>16 !=`) hydrates
the fd from the owner. Named keys bridge through
`/tmp/ashv_key_<key>` symlinks (decimals, readlink→atoi), matching
libandroid-shmem. Kill-switch: `SPROUT_SYSVIPC_OFF=1` (shm family then
returns ENOSYS, restoring pre-fix behavior).

All I/O inside the emulation goes through raw `syscall(SYS_*)` — never
back through the interposer's own wrappers (recursive-translation hazard;
same family as the accept→accept4 syscall note in ADR-0011).

## Consequences

- vkmark llvmpipe (xcb, 1280x720, default present mode) completes the
  full 13-scene suite end-to-end: **score 469**; proot lane: 259
  (**1.8×**). vkcube llvmpipe renders (was a silent all-futex freeze).
- glxgears already worked (GLX composited server-side via dri3), so
  sprout now covers every major software-render present path.
- `--sysvipc` stays a semantic no-op like proot (emulation is on by
  default, like proot's default enable in termux builds).
- v1 limits: max 64 concurrent segments per process, forked children
  reset the registry (matching `ashv_check_pid`), musl interposer build
  refresh deferred (host glibc-headers gap for a legacy `struct stat64`
  use outside this change; the runtime Alpine lane keeps the previous
  musl .so until the musl build flow lands).
- This is v2 of sprout SysV IPC overall: ADR-0018 covers semaphores for
  the binfmt (box64/steam) lane; ADR-0020 covers shm for MIT-SHM.
