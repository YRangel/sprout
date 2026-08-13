# Interception strategy

The core promise and constraint of sprout. For the full comparison table
see ADR-0002; for the Android seccomp constraint ADR-0006; for the
sanitized-binaries fast path ADR-0007; for the supervisor layer ADR-0008.

Two interception layers cover the full binary spectrum:

| layer | covers | mechanism |
|-------|--------|-----------|
| `LD_PRELOAD` fast path | dynamic glibc images (the 99%) | symbol interposition, loader-chain launch, sanitized runtime libs |
| `sprout-super` supervisor + `sprout-stub` in-guest companion (v0.6 notify lane; supervisor renamed from `sprout-ptrace` in v0.5.1) | static + Go binaries (the 1%) | seccomp user-notify serve from a stub-owned filter; ptrace only as the execve rewrite vehicle (ADR-0016) |

The launcher picks the layer per exec: dynamic binaries never see ptrace.

## One-page map

```
                    sprout (CLI, Rust)
                          |
                    elf.rs classify:  ET_DYN+PT_INTERP  /  Go-note  /  ET_EXEC  /  not-ELF
                          |
        +-----------------+------------------+-----------------+
        |                                    |                 |
   DYNAMIC glibc/musl                   GoDynamic          STATIC (glibc/musl/Go-static)
        |                               (Go bypasses         |
        |                                libc: L1 blind)     |
        v                                    |                 v
 ============================               v         stub lane (default, kind=1)
 L1  LD_PRELOAD interposer        supervisor-wrapped    sprout-stub (freestanding, no libc)
     libsprout-core{,-musl}.so    loader chain            |
     exec*/posix_spawn/system/    L1 interposer         manual ET_EXEC mapper
     socket/link/stat wrappers    + L3 notify serve      builds stack+auxv
        |                                    |           jumps to guest entry
 L2  sanitized guest libs (cache,             |              |
     svc->mov x0,xzr @ {99,293})             |     in-guest SIGSYS emu {99,293}
        |                                    |     (own sigaction, rt_sigreturn)
 ============================                |              |
        |                                    |              v
        v                                    v        ============================
 L3  seccomp user-notify filter (child installs pre-exec; listener fd stolen via pidfd)
        |                                    |              |
        |    +-------------------------------+              |
        |    v                                                     |
 ======= sprout-super (supervisor: poll + serve + waitpid loop, NO steady ptrace) =======
        |    serve set (aarch64 nrs):                                          |
        |      paths:  56 openat 437 openat2 48 faccessat 34 mkdirat 35 unlinkat
        |              33 mknodat 88 utimensat 36 link 37 unlink 38 rename 276 renameat2
        |      stat:   79 newfstatat 291 statx 78 readlinkat ( + nlink spoof, SP_HREG )
        |      exec:   221 execve (lazy-attach rewrite: loader chain / scratch argv /
        |                     script [interp,opt?,script,rest])                         |
        |      unix:   200 bind 203 connect 206 sendmsg 211 sendto (pidfd-dup serving)  |
        |      fake:   /proc/stat  /proc/loadavg  + normalize-ld-symlink pre-pass        |
        |    answer  = in-place string edit (host<=guest) + CONTINUE, or direct serve    |
        |    readmem = process_vm_readv ; writemem = sp_vm_write (same-uid = OK)          |
        v                                                                                v
                                     ANDROID KERNEL
```

**ptrace inventory** (the whole of it, nothing else exists):

| when | what | how often |
|------|------|-----------|
| stub-lane execve(221) notify | lazy `ATTACH -> GETREGSET -> rewrite argv/regs -> SETREGSET -> DETACH` while task is seccomp-parked | one attach per exec, never per syscall |
| `SPROUT_NOTIFY_STATICS=0` | legacy TRACEME lane: per-syscall stop/translate | every syscall (escape hatch, A/B) |
| `SPROUT_USER_NOTIFY=0` | same TRACEME lane for dynamic guests | every syscall (debug mode) |
| musl-static / kind=3 corners | ptrace+notify hybrid where stub emu tables don't cover | per-syscall for uncovered ops |

That is the entire ptrace surface: no tracing in the dynamic lane, no
steady-state tracing in the static lane, `PTRACE_TRACEME` exists only
behind the two `=0` escape hatches, and the one remaining productive use
is a *sub-millisecond attach* that detaches before the trapped execve
retries — the task never pointer-stops outside seccomp's own park.

## Fast path: `LD_PRELOAD` through the sanitized loader chain

`sprout` execs the guest program through a **sanitized copy of the
guest's own glibc loader** (`ld-linux-aarch64.so.1`) with
`LD_PRELOAD=libsprout-core.so:<sanitized-libc>`:

```
target/<cache>/ldso-sanitized-<hash>  --argv0 /bin/sh --inhibit-cache \
    --library-path /lib:/usr/lib:/lib/aarch64-linux-gnu  /bin/sh
```

Both cached derivatives (`ld.so` and `libc.so.6`) have the instruction
`svc #0` replaced by `mov x0, xzr` at every site that would issue a
syscall Android ≥15 SIGSYS-blocks during glibc init
(`set_robust_list`=99, `rseq`=293). Emulating "success" is what glibc
already does when the call is unavailable in the first place
(ADR-0007), so behavior is indistinguishable from a lenient kernel.
Cache keys are FNV-1a64 content hashes; the guest rootfs is never
modified.

From then on, every glibc path-taking entry point — `open`, `stat`,
`execve`, `readlinkat`, `opendir`, … — is handled by the interposer
first:

1. translate the guest path to its host equivalent (pure table lookup,
   no allocation, in-place when short enough)
2. tail-call the *real* libc symbol via `dlsym(RTLD_NEXT, …)`
3. return exactly what the real call returned

No signal stops, no `PTRACE_SYSCALL` round-trips, no seccomp-bpf
dispatcher. Guest processes run at native syscall throughput.

### Exec chaining (v0.2): guests that spawn guests

Dynamic guests re-exec themselves constantly (shells, make, python
subprocess). `LD_PRELOAD` interposes the `execve` family the kernel
never re-enters it for, so sprout — not the kernel — resolves the
target path:

1. **classify** the target with `sp_classify_host()`
   (`SP_ELF_DYNAMIC` / `SP_ELF_STATIC` / `SP_SCRIPT`, opening the host
   file once)
2. **dynamic** → rebuild argv into a *new* loader chain launch
   (`loader --argv0 --inhibit-cache --library-path target`), keep
   envp (including LD_PRELOAD)
3. **script** → parse the shebang; if the interpreter is dynamic,
   re-exec `[interp, opt?, script, args…]` through the loader chain
4. **static** → exec it straight (no preload possible; the supervisor
   isn't engaged here — a dynamic image that execs static keeps its
   supervisor ancestor attached, which handles the static child)

Depth guard: chains deeper than 4 return `ELOOP` (guards a recursive
link or a self-exec loop that would otherwise hang).

### The chain-tail trap: `dlsym(RTLD_NEXT, "execve")`

The interposer's own `execve` symbol is exported and preloaded, so
calling "the real execve" naively **re-interposes it** (RTLD_NEXT
resolves to the interposer itself). Chain tails must therefore call a
tailwind obtained once via `dlsym(RTLD_NEXT, "execve")` (skipping the
interposer by contract of preloading ordering).

### posix_spawn / posix_spawnp / system (v0.2b)

glibc freezes the POSIX spawn ABI as an internal pair of ops + a
`__spawn_action` array describing file actions
(close/dup2/open/chdir/fchdir). sprout's wrapper:

1. `fork()` (POSIX permits fork+setup+exec as conforming spawn)
2. in the child: apply the frozen action array op-by-op, honoring
   spawnattr flags (`RESETIDS`, `SETPGROUP`, `SETSIGMASK`, `SETSIGDEF`)
3. re-exec through the *chain* builder above — **not** through raw
   `__execve`, which would bypass interposition entirely (this is how
   python3.13's `subprocess.run` was losing its loader chain)

`system()` is a thin wrapper over `sh -c` via the same chain.

## Supervisor layer: `sprout-super` (v0.3 statics; binary renamed from `sprout-ptrace` in v0.5.1)

Static and raw-`svc` (Go-class) binaries have no PLT for the preload to
net. Since v0.6 (ADR-0016) their default lane is **ptrace-free**:
`sprout-super` execs the freestanding **`sprout-stub`** companion, which
hand-maps the guest image, installs an in-guest SIGSYS emulation handler
(Android TRAP set: set_robust_list/rseq/set*id/setgroups), then installs
a seccomp user-notify filter *itself* and hands its listener fd to the
supervisor over a socketpair (`pidfd_getfd` steal, same M3 wire
protocol). The filter survives the guest's own execve, so every image
the guest replaces itself with keeps reporting into the same listener.
The supervisor's steady state is a `poll + serve + waitpid` loop — no
TRACEME, no ptrace attachments, no per-syscall stops.

ptrace survives as a vehicle of last resort at exactly one point:
**execve notifications** (register-level `x0..x2` rewrites+
static→dynamic loader-chain surgery) via a lazy
`PTRACE_ATTACH/INTERRUPT → GETREGSET → rewrite → SETREGSET → DETACH`
cycle that runs while the task is already seccomp-parked. `SPROUT_
NOTIFY_STATICS=0` selects the legacy all-ptrace lane below for A/B and
fallback; both lanes share every supervisor-side translation and serve
helper.

### Legacy: the TRACEME supervisor (pre-v0.6 default, ADR-0002 last resort)

The supervisor:

1. **`SECCOMP_RET_TRAP` swallow** — Android ≥15 delivers SIGSYS (not
   KILL) for the blocked init syscalls; on `SIGSYS` stops with
   `x8 ∈ {99, 293}` the supervisor forces `x0=0` and continues without
   re-injecting, exactly the same emulation used by the sanitized libs.

2. **lazy image classification** — the first exec stop is consumed by
   the bare `waitpid` before `PTRACE_SETOPTIONS`, so
   `PTRACE_EVENT_EXEC` never fires for the main program. Images are
   classified at each thread's **first syscall-stop** instead
   (`/proc/pid/exe` headers; sanitized-loader children are detected by
   exe readlink match and treated as dynamic).

3. **dirfd-family translation at syscall-entry** — for
   `openat/statx/newfstatat/faccessat2/…/execve/execveat` with
   `dirfd == AT_FDCWD` (absolute paths, plus relative paths absolutized
   against `/proc/pid/cwd` reverse-mapped to guest spelling): peek the
   guest path, translate with the same `sp_translate` core, poke the
   result into a **stack-scratch** region (`sp−16 KiB`, map-probed
   first — translated paths are always longer, so never in-place),
   rewrite the argument register via `PTRACE_SETREGSET`.

4. **static→dynamic execve rewrite** — a static tracee exec'ing a
   dynamic guest target (or script with a dynamic interpreter) gets its
   whole exec rebuilt in tracee memory (`sp−64 KiB` arena): argv/envp
   arrays + strings, loader chain argv
   (`loader --argv0 … --inhibit-cache --library-path … target`), plus
   injected envp (`LD_PRELOAD=interposer:sanitized-libc`,
   `SPROUT_LOADER`/`SPROUT_LIBRARY_PATH`/`SPROUT_ROOTFS`/`PATH`). The
   child then runs the standard fast path under supervisor watch.

Everything else passes through untouched.

## What this intentionally does not cover

Known exclusions, all tracked on the roadmap:

- **musl guests** (Alpine) — v0.4. Different `ld-musl` path, different
  sanitize profile; the *model* carries over (the nolibc-static class
  already passes the supervisor).
- **Go binaries** — covered by the supervisor (static Go via
  classification, dynamic Go detected by its PT_NOTE `.note.go.buildid`
  and launched through the loader chain wrapped in the supervisor;
  verified with real Go 1.24.4 builds: static CGO_ENABLED=0 and
  dynamic -linkmode=external, goroutines + file I/O + exit codes).
- **64-bit guests on other ABIs** (x86_64 via Box64) — separate
  sandboxing question entirely.

## Why this shape, not what proot does

| | sprout | proot |
|---|---|---|
| dynamic binaries | **no ptrace**: PLT interposition | always ptrace (every syscall of every thread) |
| static binaries | ptrace (last resort) | ptrace (identical cost) |
| startup | one loader exec | ptrace attach + boot stop |
| guest→guest exec | in-process, only argv rebuilt | full ptrace context re-arm per exec |

The benchmark numbers in `docs/src/benchmarks.md` (5.8–8.9× median) are
the direct cost of "interpose at the symbol layer instead of the kernel
trap layer".
