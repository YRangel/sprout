# Interception strategy

The core promise and constraint of sprout. For the full comparison table
see ADR-0002; for the Android seccomp constraint ADR-0006; for the
sanitized-binaries fast path ADR-0007; for the supervisor layer ADR-0008.

Two interception layers cover the full binary spectrum:

| layer | covers | mechanism |
|-------|--------|-----------|
| `LD_PRELOAD` fast path | dynamic glibc images (the 99%) | symbol interposition, loader-chain launch, sanitized runtime libs |
| `sprout-ptrace` supervisor | static + script targets of static execers (the 1%) | ptrace argument rewriting at syscall-entry stops |

The launcher picks the layer per exec: dynamic binaries never see ptrace.

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

## Supervisor layer: `sprout-ptrace` (v0.3, statics)

Static and raw-`svc` (Go-class) binaries have no PLT for the preload to
net. They are the only images that run under the ptrace supervisor —
the last resort of ADR-0002. The supervisor:

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
- **Go-specific calling conventions** — verified only via the
  equivalent nolibc-static class (`-nostdlib -static` asm ELF). A real
  Go toolchain build is a CI milestone, not a code path.
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
