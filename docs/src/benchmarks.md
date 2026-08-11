# Benchmarks

Method: median of 5 runs after 1 warmup, on-device (Android 16, aarch64),
guest **Debian 13 (trixie, glibc 2.41)** from proot-distro v5 containers.
Both tools ran the exact same guest command each row.

Recorded 2026-08-11 (build `debug`, aarch64, Android 16, kernel 6.12.23).

| workload | proot-distro | sprout | speedup |
|----------|--------------|--------|---------|
| `python3 -c pass` | 255 ms | 100 ms | **2.6×** |
| `bash -c true` | 202 ms | 92 ms | **2.2×** |
| `bash -c 'for i in $(seq 20); do /bin/true; done'` (exec-chain) | 306 ms | 82 ms | **3.7×** |
| `find /etc -maxdepth 2 -type f` | 258 ms | 82 ms | **3.1×** |

The chains/exec case is the biggest win: proot pays ptrace round-trips per
execve, sprout pays one loader launch only for the *first* exec and then
a PLT interposition per child (`posix_spawn` covered by the wrapper, plus
`execve/execvp/system`).

Reproduce: `bench/run.sh` (script writes `bench/results.json`).
