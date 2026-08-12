# ADR-0013: seccomp user-notify fast path for hot syscall translation

## Status

Accepted (v0.5.0)

## Context

The ptrace supervisor stops at **every** syscall of static/musl/Go
tracees (~20-40µs per entry+exit pair). Path-heavy static workloads pay
seconds. The original constraint stands: ptrace is the last resort —
hot work must move out of the per-syscall stop loop.

Linux offers `SECCOMP_RET_USER_NOTIF` (kernel 5.0+,
`SECCOMP_FILTER_FLAG_NEW_LISTENER`): a filter can route chosen syscalls
to a supervising listener over an fd instead of executing them. v0.5's
probe on the target device (Android 16, kernel 6.12) confirmed:
`seccomp()` callable as untrusted app, `FLAG_NEW_LISTENER` fd, ADDFD
(present since 5.14) live-probed, `SECCOMP_USER_NOTIF_FLAG_CONTINUE`
fallback (5.5+).

## Decision

Install the translation filter **in the supervisor's forked child before
execve**, then pass the listener fd to the supervisor over a pre-fork
`socketpair(SOCK_SEQPACKET)` via SCM_RIGHTS (no pidfd dependency).
Inheritance covers the whole child tree.

The supervisor event loop multiplexes:

- a `poll()` on the listener + a **SIGCHLD self-pipe** (ptrace stops
  deliver SIGCHLD, so both sources wake one poller), then
- a `waitpid(-1, WNOHANG)` drain loop over every queued ptrace event.

Servicing (per trapped syscall):

- **fd-returning ops** (`openat`, `openat2`) — supervisor performs the
  translated open, injects the host fd into the tracee's table via
  `SECCOMP_IOCTL_NOTIF_ADDFD`, returns the child descriptor as val.
- **value ops** (`faccessat`) — supervisor answers with `error`/0.
- **mutation family** (`mkdirat`, `unlinkat`, `mknodat`, `fchmodat`,
  `fchownat`, `utimensat`, `symlinkat`, `linkat`, `renameat`,
  `renameat2`) — supervisor performs the translated syscall itself.
- **anything outside the filter's served set** — falls through to the
  ptrace-exit-stop path via `SECCOMP_USER_NOTIF_FLAG_CONTINUE`.
- **stat-family buffer writes** (`newfstatat`, `statx`, `readlinkat`)
  are intentionally NOT served: writing raw structs into the tracee's
  stack across glibc internals showed heap damage during mixed debug
  runs; those stay ptrace-served (cheap enough with the woken loop).

Relative guests paths resolve through `/proc/<pid>/cwd` (the tracee's
real cwd is already translated) instead of rootfs-prefixing.

`execve/execveat/chdir` or anything else without a daemon-safe mapping
simply isn't trapped and rides the normal ptrace tables (statics) or
the interposer (glibc-dynamic).

## How the first-stop deadlock was avoided

A user-notify wait blocks **inside** the syscall; ptrace stops only
materialize on the return-to-userland path. Sequence sensitivity is
therefore real: the supervisor MUST pump notifications while awaiting
the initial exec stop — the waitpid path uses the pump loop from day
one (`sp_notify_pump()` + `WNOHANG`), never a bare blocking `waitpid`.

Also load-bearing: the seccomp filter is per-task and inherited. The
supervisor **must not** let its own filter install: installation
happens strictly in the CHILD before exec.

## Consequences

- ~5-15µs per advertised syscall vs ~60-80µs ptrace round trip on the
  hot set; expected dominant win for static-heavy and musl workloads.
- glibc-dynamic shadow tracees stay `PTRACE_CONT`-only — no notify tax.
- If `GET_NOTIF_SIZES`/listener/ADDFD probed unavailable, the older
  ptrace path is used unchanged (`SPROUT_USER_NOTIFY=0` forces it).
- Android's global seccomp filter still kills `io_uring*`, `rseq`,
  `faccessat2` itself: those arrive as SIGSYS and keep the existing
  ptrace-emulation table, so the two mechanisms coexist.
