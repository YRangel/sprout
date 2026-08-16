# sprout

**Rootless glibc/musl Linux userspace for Android. Fast, auditable, open.**

`sprout` runs full Linux userspaces (Node.js, Python, Git, Chromium, X
applications, desktop sessions) on Android without root. It is a drop-in
replacement for [proot](https://proot-me.github.io/) with an `LD_PRELOAD` fast
path that avoids ptrace syscall-stop overhead, plus automatic ptrace fallback
for static/Go binaries and a pure-notify stub lane for AArch64 statics.

## Status

**Working today on-device (Termux, aarch64):**

- Interactive glibc shells (bash/zsh) with X11 + audio (termux-x11 preset)
- XFCE4 desktop sessions + Firefox ESR under `--shared-tmp --termux-x11`
- Package managers: `apt`/`dpkg` and `apk` (musl lane)
- Toolchains: meson, autotools, gcc, clang — full native-source builds inside
  the guest (e.g. Mesa built natively for Turnip GPU bring-up)
- Static and Go binaries (ptrace supervisor, or pure USER_NOTIFY stub lane)
- Two rootfs flavors: glibc (Debian) and musl (Alpine)
- Vulkan visible-present workloads on Turnip (real Adreno GPU) AND llvmpipe
  (software/XShm) under termux-x11 — vkmark head-to-head benchmarks favor
  sprout on both (see [Benchmarks](./benchmarks.md)).
- x86_64/i386 guests via the userspace binfmt adapter (box64/box32, qemu,
  or any ARM64 emulator exposing `LD_PRELOAD`-composable startup) with
  userspace SysV IPC emulation (semaphores + shared memory).

## Layout of this book

- **User guide** — install, proot-compat table, environment policy, X11/GPU
- **Architecture** — interceptor model, translation rules, threat model
- **ADRs** — numbered decision records, template through shadow-state emulation
- **Operations** — benchmarks, roadmap, development notes
