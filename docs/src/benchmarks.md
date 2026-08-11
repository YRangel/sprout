# Benchmarks

Method: wall-clock medians, on-device (Android 16, aarch64, kernel
6.12.23). Same guest rootfs and same command lines on both sides;
`proot-distro v5 ... login` is the incumbent baseline. Reproduce with
`bench/run.sh [rootfs] [iterations]` (honors `SPROUT_BIN=...`).

## glibc guest — LD_PRELOAD fast path (release build, median-of-5)

Guest: **Debian 13 (trixie, glibc 2.41)** (`proot-distro/containers/debian`).

| workload                        | proot-distro | sprout | speedup |
|---------------------------------|--------------|--------|---------|
| `python3 -c pass`               | 270 ms       | 43 ms  | **6.3×** |
| `bash -c true`                  | 266 ms       | 28 ms  | **9.5×** |
| exec-chain (20× `/bin/true`)    | 325 ms       | 77 ms  | **4.2×** |
| `find /etc -maxdepth 2 -type f` | 300 ms       | 30 ms  | **10.0×** |

Debug build for reference: 2.6–3.0× on the same workloads.

Why sprout wins: proot pays ptrace round-trips for **every** path-bearing
syscall in the guest; sprout's dynamic path pays one loader launch for the
first exec and then a PLT interposition per child (`posix_spawn`/`execve`/
`system` wrappers) with native syscalls in between.

v0.4.1 note (ADR-0010): the symlink-chase cost that regressed this cell
to 4.5× is now amortized by the per-process translate memoization table.
Marginal per-exec cost sits near the ld.so load floor (~2 ms), which any
PT_INTERP-respecting launcher pays.

**Device variance**: cells drift ±30% across thermal states (same session
measured 51 ms and 77 ms for exec-chain medians across different device
states). Ranges are honest; speedup within one session is stable.

## musl guest + supervisor-routed workloads (release build, median-of-7)

Guests: **Alpine (musl 1.2.6, busybox)** (`proot-distro/containers/alpine`)
and supervisor-mode binaries in the Debian rootfs. Musl/static/Go classes
always pay ptrace stops (ADR-0008/0009) — yet still beat the incumbent's
ptrace-everything model:

| workload                        | proot-distro | sprout | speedup |
|---------------------------------|--------------|--------|---------|
| busybox `sh -c true` (musl)     | 245 ms       | 40 ms  | **6.1×** |
| busybox `ls /etc/apk` (musl)    | 261 ms       | 40 ms  | **6.5×** |
| musl `python3 -c pass`          | 290 ms       | 107 ms | **2.7×** |
| musl-static binary (exit 42)    | 239 ms       | 36 ms  | **6.6×** |
| nolibc-static (openat+exit 42)  | 247 ms       | 39 ms  | **6.3×** |
| Go static (CGO_ENABLED=0)       | 223 ms       | 40 ms  | **5.6×** |

Honest notes:

- `busybox ls /etc` exits 1 under sprout because `/etc/mtab`
  (relative → `../proc/mounts`) is unresolvable: sprout deliberately does
  not bind-fake `/proc` (proot-distro does). The `/etc/apk` cell is the
  clean comparison.
- musl python3 (2.3×) is the busiest traced-process profile measured so
  far — dense futex/signal/syscall traffic under kind=3 tracing.
- Go-static at 4.9×: even the worst-case profile (per-syscall tracing of a
  runtime that never lets LD_PRELOAD near its paths) is materially faster
  than proot.

## Historical (v0.3 toolchain sweep, 2026-08-11, same host)

First published release pass: python3 239→41 (5.8×), exec-chain 285→49
(5.8×), find 214→24 (8.9×). Kept for comparison with the table above
(post-v0.4 numbers trade exec-chain speed for musl correctness).
