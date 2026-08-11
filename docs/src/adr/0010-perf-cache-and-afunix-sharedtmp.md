# 0010. Perf cache + AF_UNIX pathname translation + shared-tmp scaffold

**Status:** accepted, implemented (v0.4.1)

## Context

Three user-visible gaps appeared after v0.4:

1. **exec-chain benchmark regressed** 5.8× → 4.5× after musl's absolute
   -symlink chase (`sprout-preload/csrc/sprout_preload.c`, `sp_translate_x`)
   added an `lstat`+`readlink` round-trip on every translated path.
2. **X11/Wayland/VirGL/ssh-agent did not work**: all speak AF_UNIX
   *pathname* sockets (`/tmp/.X11-unix/X0`), whose pathnames the kernel
   follows on the host without translation — guest spelling never reached
   the host server.
3. **proot-distro parity was incomplete**: `--shared-tmp` is the one flag
   that makes all of the above usable in practice.

## Decision

**A. Per-process translate memoization.** `sp_translate`+chase is a pure
function of the guest path (rootfs/binds frozen per-process). A 128-entry
open-addressing hash caches positive translated results for the process
lifetime. Worst case on collision: extra probe. This restores v0.3 hot
path costs for chain-heavy guests *and* improves shells/pythons where the
same prefixes re-translate thousands of times. No cross-process sharing
(subsequent exec chains each start cold); the dominant repeating pattern
is intra-process (e.g. `/bin/true` 20×).

**B. AF_UNIX pathname translation in both layers.**
- LD_PRELOAD interposer (glibc + musl): `bind`, `connect`, `sendto`,
  `sendmsg`, `recvfrom`, `recvmsg`, `getsockname`, `getpeername`.
  Forward translates the pathname (`sp_translate` order: binds then
  rootfs, same as file paths); reverse direction (`getsockname` etc.)
  uses `sp_reverse` (strip bind/rootfs prefix). **NUL-first bytes
  (abstract namespace) are untouched** — kernel-only namespace, no
  filesystem semantics to redirect. **No symlink chasing on sockets**
  (bind *creates* the pathname).
- ptrace supervisor (musl/static/Go): forward-only v1 — `bind(200)`,
  `connect(203)`, `sendto(206)` rewritten via the stack-scratch arena;
  `sendmsg`(211)/reverse noted in code as a documented gap (X11 stream
  workloads only ever need the connect direction).
- 108-byte `sun_path` limit: if the translated pathname would exceed,
  pass through unchanged (kernel's own EFAULT/ECONNREFUSED is always
  more honest than a silently truncated path).

**C. `--shared-tmp` CLI flag (proot-distro parity).**
- `bindings.push(Binding::parse("${PREFIX}/tmp:/tmp"))`
- Same semantic as `proot-distro login --shared-tmp`: host `$PREFIX/tmp`
  is guest `/tmp`, carrying X11/Wayland/VirGL/ssh-agent sockets over
  normal file bind semantics.
- Documented in `docs/src/guide/x11-gpu.md` with the **physics-based**
  GPU explanation: `/dev/kgsl` and `/dev/mali0` are SELinux-blocked for
  every unprivileged app on Android; virgl/llvmpipe work socket-side and
  are the only realistic GPU acceleration on unrooted Android.

## Consequences

- exec-chain: 71 ms → back to ~49 ms (returns to v0.3 levels; N=100/200
  sweeps confirm the marginal per-exec cost is again ld.so-load dominated
  at ≈2.0–2.4 ms/exec, which is the floor on any launcher that must honor
  host-enforced PT_INTERP).
- On-device verified: glibc Python client ↔ host X-socket server rc=42;
  musl, musl-static, Go-dynamic, Go-static all rc=42; abstract socket
  round-trip verified; `getsockname` returns the **guest spelling** after
  translation.
- `--dry-run` lists the injected `SPROUT_BIND=...` line; unprivileged
  verification of proot-distro `--shared-tmp` parity completed.

## Refactor invariants (for any future rewrite)

Keep these true or the new runtime breaks:

1. `sp_translate` order: binds guest→host **first**, then rootfs prefix,
   then passthrough; reverse order on the way back. No partial binds.
2. AF_UNIX: abstract names (sun_path[0]=='\0') are NEVER touched; only
   pathname sockets translate.
3. Cache: only *positive* results (a lookup miss costs the pure-function
   price, that's it); never thread-safe problems — read-modify-write on
   the same slot is idempotent by definition (g_cfg frozen at init).
4. `--shared-tmp` is sugar for `Binding`. No other behavior (no side
   files, no env injection).
5. The ADREADME: the rootfs prefix lives in exactly one place per layer
   (`sp_translate`); any new wrapper calls it, never re-implements it.
