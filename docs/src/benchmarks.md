# Benchmarks

Method: median of 5 runs after 1 warmup, on-device (Android 16, aarch64,
kernel 6.12.23), guest **Debian 13 (trixie, glibc 2.41)** from proot-distro
v5 containers. Both tools ran the exact same guest binaries each row.

Recorded 2026-08-11 — `target/release/sprout` (Rust `-O3`/opt) +
interposer C built with gcc `-O3 -flto` in-guest.

| workload | proot-distro | sprout | speedup |
|----------|--------------|--------|---------|
| `python3 -c pass` | 239 ms | 41 ms | **5.8×** |
| `bash -c 'for i in $(seq 20); do /bin/true; done'` (exec-chain) | 285 ms | 49 ms | **5.8×** |
| `find /etc -maxdepth 2 -type f` | 214 ms | 24 ms | **8.9×** |

Debug build (`cargo build`, `-O2` interposer) for reference: ~2.2–3.7× —
release mode roughly doubles the margin again.

Why the chains/exec case dominates: proot pays ptrace round-trips per
execve, sprout pays one loader launch for the *first* exec and then a PLT
interposition per child (`posix_spawn`/`execve`/`system` wrappers). `find`
leans hard on the `-flto` interposer's inline path translation.

Reproduce: `bench/run.sh` (median-of-N comparative harness).


## musl guests (v0.4, supervisor-routed)

| workload | proot-distro (alpine) | sprout | speedup |
|----------|-----------------------|--------|---------|
| `busybox sh -c true` | 243 ms | 55 ms | **4.4×** |
| `busybox ls /etc` | 242 ms | 72 ms | **3.4×** |

Musl guests run under the supervisor (ADR-0009) because busybox's
suid-drop and musl's `ld.so == libc` shape defeat glibc-style
sanitization — yet still faster than proot, which pays ptrace per
syscall everywhere.
