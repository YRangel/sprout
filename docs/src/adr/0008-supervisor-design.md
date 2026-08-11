# 0008. ptrace supervisor design (static / preload-incapable guests)

**Status:** accepted, implemented

## Context

Static binaries (glibc-static, Go) cannot be intercepted by `LD_PRELOAD`:
no PLT, raw `svc` for everything. On Android ≥15 they additionally hit
`set_robust_list`/`rseq` seccomp blocks at startup. proot's approach is
unconditional ptrace for everything; we deliberately reserve ptrace as the
**last resort** (ADR-0002), used only for these images.

Hard problem discovered: static→dynamic `execve` (or static→script with a
dynamic interpreter) requires the full loader chain (sanitized `ld.so`,
`--argv0/--inhibit-cache/--library-path`, preloaded interposer), exactly
like the preload interposer builds in-process. The kernel cannot do it
(`PT_INTERP` resolves on the host, Android `/lib64` doesn't exist, $PREFIX
unable to provide it).

## Decision

`sprout-ptrace` (container binary, `crates/sprout-ptrace/csrc/sprout_ptrace.c`):

1. **Process model** — `PTRACE_TRACEME` child, `PTRACE_O_TRACEFORK |
   TRACEVFORK | TRACECLONE | TRACEEXEC | TRACESYSGOOD | EXITKILL`. Syscall
   numbers/details via `PTRACE_GET_SYSCALL_INFO` (authoritative; arm64),
   fallback x8+`in_sys` toggle. Per-pid state table (512 entries).

2. **Seccomp survival (SIGSYS whitelist)** — Android ≥15 delivers
   `SECCOMP_RET_TRAP` (SIGSYS), not KILL. On SIGSYS signal-stop with
   `regs[8] ∈ {99 set_robust_list, 293 rseq}`: set `regs[0] = 0` and
   continue without reinjecting the signal. Safe: both syscalls are
   advisory; glibc falls back gracefully. (Always-on for static AND
   dynamic tracees — the fast-path sanitization can't help statics.)

3. **Lazy image classification** — the first exec stop is consumed by the
   bare `waitpid` before `PTRACE_SETOPTIONS`, so `PTRACE_EVENT_EXEC` never
   fires for the main image. Each tracee is classified at its **first
   syscall-stop** instead (post-exec by construction): read
   `/proc/<pid>/exe` phdrs; `PT_INTERP` ⇒ dynamic; else static — *except*
   when `/proc/<pid>/exe` readlink equals `SPROUT_LOADER` (the sanitized
   ldso itself is ET_DYN-without-PT_INTERP): those are loader-chain
   images, treated as dynamic (their host .so opens must NOT be
   guest-translated).

4. **Path translation (static tracees only)** — on syscall-entry for the
   dirfd-family table (`openat/openat2/statx/newfstatat/faccessat[2]/
   readlinkat/mkdirat/unlinkat/mknodat/fchmodat/fchownat/utimensat/
   symlinkat(linkpath)/linkat/renameat[2]/chdir/execve/execveat`), when
   dirfd == AT_FDCWD: peek the path from tracee memory; if relative,
   absolutize against `/proc/<pid>/cwd` reverse-translated to guest
   spelling (cwd outside rootfs ⇒ pass through); translate with the same
   `sp_translate` core as preload; poke the result into the **stack
   scratch** at `SP−16 KiB` (mapped-probed with PEEK before POKE; never
   in-place — translations are always longer); rewrite the argument
   register via `PTRACE_SETREGSET`.

5. **Static→dynamic / static→script execve rewrite** — when the exec
   target classifies dynamic (has `PT_INTERP`) or is a script whose
   interpreter is dynamic, the whole exec is rebuilt in tracee stack
   scratch (`SP−64 KiB` arena: argv array | envp array | strings):
   - argv = `loader --argv0 a0 --inhibit-cache --library-path <lp>
     <host-prog> [orig args...]`
   - envp = copied, with `LD_PRELOAD=<interposer>:<sanitized-libc>`,
     `SPROUT_LOADER`, `SPROUT_LIBRARY_PATH`, `SPROUT_ROOTFS`, `PATH`
     injected/replaced (the whole plan context: without it the interposer
     degenerates to host paths and resolves host toybox!)
   - `regs[0..2]` repointed. Chained children then run the standard
     preload fast path under the supervisor's watch (SIGSYS swallow
     still applies; path duty is back with the interposer).

6. **Honest leftovers** — static→static execve is simple path translation;
   script-from-static with a *dynamic* interpreter works (interpreter
   resolved on guest PATH, shebang opt-arg preserved); script→static
   interpreter, musl guests, and syscall-arg rewriting for output buffers
   (readlinkat result, getcwd) remain documented gaps.

## Consequences

- Verified on Android 16: static glibc binary survives startup, opens
  `/etc/...` markers, execs dynamic guests, runs scripts — with zero
  preload availability. Exit codes propagate exactly (42).
- The supervisor handles `env -i`-style sparse envs correctly by injecting
  the full plan context.
- Cost: every static-tracee syscall costs two ptrace context switches;
  that's why it's confined to statics. Dynamic fast path untouched.

## Threat-model notes

- Scratch writes are confined to `SP−N` of the *stopped* thread; other
  threads never address it; no global writes.
- `SP_*` buffers are bounded (`SP_PATH_MAX` strings, ≤256 argv entries,
  ≤64 KiB arena); overflow ⇒ honest no-op (kernel sees original args).
- The supervisor never follows into the guest rootfs writes; it only
  rewrites syscall arguments in tracee memory.
