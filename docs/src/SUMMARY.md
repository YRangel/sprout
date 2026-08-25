# Summary

[Introduction](./introduction.md)

# User guide

- [Installation](./guide/installation.md)
- [proot compatibility](./guide/proot-compat.md)
- [Environment policy](./guide/environment.md)
- [X11 / GPU](./guide/x11-gpu.md)
- [FAQ](./guide/faq.md)

# Architecture

- [Overview](./architecture/overview.md)
- [Interception model](./architecture/interception.md)
- [Path translation](./architecture/translation.md)
- [Threat model](./architecture/threat-model.md)

# Decisions (ADRs)

- [Template](./adr/template.md)
- [0001: language split](./adr/0001-language-split.md)
- [0002: interception strategy](./adr/0002-interception-strategy.md)
- [0003: loader launch](./adr/0003-loader-launch.md)
- [0004: host scope](./adr/0004-host-scope.md)
- [0005: guest scope](./adr/0005-guest-scope.md)
- [0006: Android seccomp blocked syscalls](./adr/0006-android-seccomp-blocked-syscalls.md)
- [0007: guest libc/ld.so sanitization](./adr/0007-guest-libc-sanitization.md)
- [0008: supervisor design](./adr/0008-supervisor-design.md)
- [0009: musl guests](./adr/0009-musl-guests.md)
- [0010: perf cache + AF_UNIX + shared-tmp](./adr/0010-perf-cache-and-afunix-sharedtmp.md)
- [0011: emulate blocked syscalls at PLT](./adr/0011-emulate-blocked-syscalls-at-plt.md)
- [0012: apt/dpkg compatibility layer](./adr/0012-apt-dpkg-compat-layer.md)
- [0013: seccomp user-notify fast path](./adr/0013-user-notify-fast-path.md)
- [0014: vfork-safe exec chain](./adr/0014-vfork-safe-exec-chain.md)
- [0015: relative path semantics (cwd + dirfd)](./adr/0015-relative-path-semantics.md)
- [0016: pure-notify statics lane (sprout-stub)](./adr/0016-pure-notify-statics-lane.md)
- [0017: userspace binfmt adapter (proot -q parity)](./adr/0017-userspace-binfmt-adapter.md)
- [0018: userspace SysV IPC emulation for box64/box32 guests](./adr/0018-userspace-sysvipc-shim.md)
- [0019: proot runtime-flag parity (-k/-p/-v/-V/-h/--kill-on-exit/--sysvipc/--ashmem-memfd)](./adr/0019-proot-runtime-flag-parity.md)
- [0020: sysv-shm emulation via the libandroid-shmem protocol (MIT-SHM/llvmpipe fix)](./adr/0020-sysv-shm-libandroid-shmem-protocol.md)
- [0021: FEX-Emu SysV IPC via in-source shim](./adr/0021-fex-sysvipc-in-source-shim.md)

# Operations

- [Benchmarks](./benchmarks.md)
- [Roadmap](./roadmap.md)
- [Development](./development.md)
