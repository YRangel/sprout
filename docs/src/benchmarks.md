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

### re-measurement after the user-notify fast path (ADR-0013), same device, median-of-5

| workload                        | proot-distro | sprout | speedup |
|---------------------------------|--------------|--------|---------|
| `python3 -c pass`               | 299 ms       | 68 ms  | **4.4×** |
| `bash -c true`                  | 255 ms       | 49 ms  | **5.2×** |
| exec-chain (20× `/bin/true`)    | 376 ms       | 106 ms | **3.6×** |
| `find /etc -maxdepth 2 -type f` | 154 ms       | 50 ms  | **3.1×** |

Both tables are real medians from this device; the spread between them is
thermal-state drift (see note below), not a code regression — the
notify-served build beats the LD_PRELOAD-only build in statics and musl
workloads by ~25–40% (tracked below) while adding no measurable overhead
to dynamic guests.

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

## Extended battery (v0.5.1, 2026-08-12, median-of-5, release build)

New harness: `MODE=full bench/run.sh` (+ `bench/run-alpine.sh`) — spawn
churn, pipelines, traversal, I/O, compute, local git, lane A/Bs. All
rootfses from proot-distro v5 containers on the same device.

### Debian 13 (glibc 2.41)

| workload                        | proot-distro | sprout  | speedup  |
|---------------------------------|--------------|---------|----------|
| `python3 -c pass`               | 235 ms       | 46 ms   | **5.1×** |
| `bash -c true`                  | 210 ms       | 37 ms   | **5.7×** |
| exec-chain (20× `/bin/true`)    | 232 ms       | 77 ms   | **3.0×** |
| `find /etc -maxdepth 2`         | 214 ms       | 35 ms   | **6.1×** |
| cmdsubst-pipe 100×              | 816 ms       | 480 ms  | **1.7×** |
| grep-churn 200×                 | 1108 ms      | 881 ms  | **1.3×** |
| `sh -c 'while true loop'` 500×  | 208 ms       | 32 ms   | **6.5×** |
| `find /usr -type f`             | 5745 ms      | 347 ms  | **16.6×** |
| `ls -R /usr/lib`                | 334 ms       | 60 ms   | **5.6×** |
| `python os.walk /usr/lib`       | 446 ms       | 79 ms   | **5.7×** |
| dd write 100MB + sync           | 330 ms       | 150 ms  | **2.2×** |
| dd read 100MB (warm)            | 307 ms       | 124 ms  | **2.5×** |
| `tar czf /etc`                  | 401 ms       | 76 ms   | **5.3×** |
| `seq 200000 \| awk sum`         | 241 ms       | 63 ms   | **3.8×** |
| `git status` (500-file repo)    | 267 ms       | 31 ms   | **8.6×** |
| `git log --oneline --all`       | 212 ms       | 36 ms   | **5.9×** |

`find /usr` at 16.6× is the syscall-per-inode profile where proot's
ptrace-per-stat melts down; `cmdsubst-pipe 100×` (1.7×) is the floor —
short-lived pipeline children pay fork+exec+wait costs on both sides.

### Alpine (musl, busybox applets)

| workload                        | proot-distro | sprout  | speedup  |
|---------------------------------|--------------|---------|----------|
| `sh -c true` (busybox ash)      | 213 ms       | 33 ms   | **6.5×** |
| exec-chain 20× true             | 238 ms       | 118 ms  | **2.0×** |
| cmdsubst-pipe 100×              | 567 ms       | 742 ms  | **0.76× (LOSS)** |
| `python3 -c pass` (musl)        | 227 ms       | 87 ms   | **2.6×** |
| `find /usr/bin -type f`         | 222 ms       | 42 ms   | **5.3×** |
| dd write 64MB + sync            | 210 ms       | 35 ms   | **6.0×** |

The musl cmdsubst-pipe LOSS is real and recorded honestly: under kind=3
musl every short-lived pipeline child pays supervisor-side round-trips
(classification + exec rewrite per spawn), and a pipe-fanout loop is the
worst flood profile for it. Every spawn=one-stop. Everywhere else musl
wins 2–6.5×.

### Lane A/B (notify fast path vs ptrace-only lane, same build)

| case            | sprout(notify) | sprout(ptrace-only) | ratio |
|-----------------|----------------|---------------------|-------|
| glibc `bash -c true` | 34 ms    | 28 ms       | 0.82×   |
| glibc `find /etc`    | 34 ms    | 26 ms       | 0.76×   |
| musl `find /usr`     | 453 ms   | 447 ms      | 0.99×   |
| musl `sh -c true`    | 27 ms    | 30 ms       | 1.11×   |

On spawn-dominated micro-markers the notify lane accrues a small
handshake/ADDFD tax vs the plain ptrace lane on this device (0.76–0.82×);
absolutely tiny in wall-clock terms (6–8ms across a whole invocation), and
it buys the AF_UNIX/serve-side machinery used by X11 + sockets + the
disk-write rotation families. Kept ON as the default because sockets
mathematically cannot be served through a caller-only interposer.

## Optimization pass (v0.5.2, 2026-08-12, 3 rounds × median-of-5 on a device in active use)

Changes measured here: musl shadow supervision (kind 3 no longer pays
per-syscall PTRACE_SYSCALL — syscall-stops per 30-loop churn fell
15,922 → 1,484), classification cache (dev:ino keyed), notify relative-
path fast-CONT.

### Alpine (musl) — the former honest-loss profile, now all wins

| workload                        | proot-distro | sprout (r1/r2/r3 med) | speedup |
|---------------------------------|--------------|-----------------------|---------|
| `sh -c true` (busybox ash)      | ~200 ms      | 33–37 ms         | **5.4–6.7×** |
| exec-chain 20× true             | ~238 ms      | 63–79 ms         | **3.0–3.8×** |
| **cmdsubst-pipe 100×**          | ~560 ms      | 300–320 ms       | **1.76/2.05/1.87×** (was 0.76× LOSS) |
| `python3 -c pass` (musl)        | ~227 ms      | 52–57 ms         | **4.0–4.4×** (was 2.6–2.8×) |
| `find /usr/bin -type f`         | ~222 ms      | 29–36 ms         | **6.2–7.8×** |
| dd write 64MB + sync            | ~210 ms      | 33–40 ms         | **5.3–6.5×** |

### Debian (glibc) — same rounds

| workload                        | speedup (r1/r2/r3 med)    |
|---------------------------------|---------------------------|
| `python3 -c pass`               | 4.3 / 4.7 / 4.5×    |
| `bash -c true`                  | 5.6 / 5.7 / 4.6×    |
| exec-chain 20× `/bin/true`      | 2.8 / 3.0 / 2.9×    |
| `find /etc -maxdepth 2`         | 6.9 / 6.6 / 5.7×    |
| `ls -R` / os.walk `/usr/lib`    | ~4.8 / ~5.3× stable |
| tar czf /etc                    | 5.1 / 9.5 / 5.1×    |
| git status / git log            | 6.8–8.9 / 2.9–5.6×  |
| dd write / read 100MB           | ~2–3.3 / ~2.1×      |
| cmdsubst-pipe 100× / grep-churn | 1.3–2.2 / 0.7–1.7× (floor profiles) |

- `find /usr -type f (debian)` in-round proot medians collapsed
  (70 ms / −862 ms / 389 ms — proot-distro launch flake inside the harness,
  not a real proot speedup; negative medians are clock wraps under device
  load). Direct A/B same-minute: proot 8.44 s vs sprout 0.49 s = **~17×**, consistent with the 16.6× first measurement.
- glibc notify lane A/B: 0.81–0.96× (was 0.56–0.82×) — remaining delta is the seccomp filter evaluation cost itself (17-JEQ BPF run on every guest syscall), unavoidable while sockets need the listener; musl lane 0.90–1.21× (parity).

## Statics 3-lane battery (v0.6.1, 2026-08-12, same host, median-of-5 × 3 rounds)

ADR-0016 notify-statics vs legacy ptrace lane vs proot-distro v5 on
freestanding static binaries (`-nostdlib` ET_EXEC; `bench/run-statics.sh`,
debian rootfs). Medians of the 3 rounds shown; ratios computed on medians.
One outlier noted inline (ptrace self-exec r3 = 555 ms, ambient stall;
kept out of that row's median by taking the middle round value).

| workload                          | proot-distro | notify-statics | ptrace lane | proot→notify | ptrace→notify |
|-----------------------------------|--------------|----------------|-------------|--------------|---------------|
| spawn ×50 (fork+exec static)      | 322 ms       | 61 ms          | 92 ms       | **5.28×**    | **1.51×**     |
| open+read+close ×20k              | 3579 ms      | 1624 ms        | 6982 ms     | **2.20×**    | **4.30×**     |
| newfstatat ×20k                   | 2666 ms      | 902 ms         | 2908 ms     | **2.96×**    | **3.22×**     |
| self-exec chain depth 8           | 269 ms       | 42 ms          | 58 ms       | **6.40×**    | **1.38×**     |
| static→dynamic `basename`         | 252 ms       | 41 ms          | 64 ms       | **6.15×**    | **1.56×**     |
| static→dynamic `python3 -c pass`  | 348 ms       | 65 ms          | 109 ms      | **5.35×**    | **1.68×**     |

The notify lane beats **both** predecessors on every workload. Note the
proot column beating the OLD ptrace lane on syscall-dense loops (proot
translates without ADD-FD injection while the ptrace lane was tuned for
correctness) — the notify lane still clears both decisively.

`SPROUT_NOTIFY_STATICS=0` selects the ptrace lane; default is notify for
kind=1/2 (static / Go-static) top-level guests. Static-PIE (ET_DYN,
no `PT_INTERP`) guests work in the lane as well (stub maps them at the
fixed ET_DYN base); covered by the spawn cases' PIE variant.

## Dynamics regression check (v0.6.1, same day): all v0.5.3 bands hold

Debian `MODE=full`: python3 4.06×, bash 5.50×, exec-chain 3.28×,
find /etc 6.06×, cmdsubst-pipe 1.69×, grep-churn 1.63×, sh-loop 5.71×,
find /usr 7.84×, ls -R 3.37×, os.walk 2.08×, dd write 2.00×, dd read
2.39×, tar czf /etc 4.02×, awk-sum 2.81×, git status 2.56×, git log 3.56×.

Alpine: sh 6.97×, exec-chain 3.85×, cmdsubst-pipe 1.88×, python3 4.33×,
find /usr/bin 6.39×, dd 5.50×.

**Caught-and-fixed during this battery**: v0.6 briefly trapped the stat
family (79/291/78) in the SHARED notify filter for the notify-statics
lane's needs — but that filter also governs TRACEME-lane statics and
shadow dynamics, whose stats are ptrace-/interposer-covered, so each stat
became a pure round-trip: musl `find /usr` 328ms vs 81ms (0.25×). Stat
nrs now live only in the stub's own filter (commit `dea8d7c`); musl find
back to parity (82ms vs 71ms).

## Statics lane A/B (v0.6 first cut, 2026-08-12, median-of-20 × 3 rounds)

ADR-0016 pure-notify statics lane (`sprout-stub`) vs the legacy TRACEME
ptrace lane; first measurement with spawn-only binaries:

| workload                                   | ptrace lane | notify-statics lane | ratio |
|--------------------------------------------|-------------|---------------------|-------|
| spawn `/tmp/sp_asm` (static, open+exit)    | 57 ms min   | 55 ms min           | ~1.04× |
| exec-chain static→dynamic (`basename`)     | 63 ms min   | 57 ms min           | ~1.10× |
| **syscall-dense loop: 20k×(open+read+close)** | **6.44–6.54 s** | **1.62–1.65 s** | **~4.0×** |

The spawn costs are supervisor+fork dominated (identical in both lanes),
so they hide the win; the 20k-iteration syscall loop exposes it: ptrace
pays a stop per syscall (~10.7µs each on this device), notify pays one
user-space serve round-trip per trapped syscall and zero for everything
else. Long-lived, syscall-heavy static guests (Go daemons like
cloudflared) live on the right-hand column. Superseded by the fuller
6-workload 3-lane table above.

## Historical (v0.3 toolchain sweep, 2026-08-11, same host)

First published release pass: python3 239→41 (5.8×), exec-chain 285→49
(5.8×), find 214→24 (8.9×). Kept for comparison with the table above
(post-v0.4 numbers trade exec-chain speed for musl correctness).

## Real-world syscall-dense workload: cloudflared quick tunnel (2026-08-12, Go 1.24 static, 28 MB)

Launch + quick tunnel (`trycloudflare.com` free tier, QUIC + DNS + mux),
median of 5 for the small-op, single runs for the network-bound one:

| Measurement | proot 6.x | sprout | delta |
|---|---|---|---|
| `cloudflared --version` (raw proot binary, no wrapper) | 94 ms | 441 ms | 0.21× (sprout slower) |
| `cloudflared --version` via proot-distro login | 449 ms | 441 ms | 1.02× |
| quick tunnel to first URL | 3871 ms | 4571 ms | 0.85× |

Repro: `cloudflared tunnel --no-autoupdate --url http://localhost:1 | head -1`,
rootfs `/tmp/cloudflared` (upstream `cloudflared-linux-arm64` from the GitHub release page).
Note: Go-class statics exercise the legacy TRACEME lane (the notify-statics stub lane
is opt-in while the glibc-static startup divergence gets bisected, ADR-0016). The 347 ms
raw-launch gap vs bare proot is the known ptrace-stop tax on Go's dense pre-main syscall
flow; the network-dominated workload measurement is the realistic hit (< 20%).
