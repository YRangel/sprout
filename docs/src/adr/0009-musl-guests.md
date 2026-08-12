# 0009. musl guests (Alpine): supervisor-first architecture

**Status:** accepted, implemented (v0.4)

## Context

musl rootfses (Alpine) differ from glibc rootfses in ways that void most
of the glibc fast path:

1. **ld.so IS the libc** — one file serves both roles. Sanitizing only the
   "loader" left busybox's DT_NEEDED `libc.musl-aarch64.so.1` resolving
   (by NAME, musl dedups on filename not SONAME) to the *unpatched* rootfs
   copy.
2. **Android seccomp blocks the whole set*id family** (143–152, 159) for
   untrusted apps; busybox's suid-drop (`setgid(getgid()); setgroups(1,...);
   setuid(getuid())`) must fake success — a rootless sandbox IS "already at
   minimal privilege". Same story for `faccessat("", R_OK, AT_EMPTY_PATH)`
   (48): musl 1.2.6's `__libc_start_main` polls it for thread-group identity,
   glibc tolerates nothing there (glibc artifacts get ONLY {99,293}).
3. **Every Alpine applet is an absolute symlink** (`/bin/ls → /bin/busybox`)
   — the host kernel resolves the target on the HOST (missing). All
   translation layers (interposer path wrappers, supervisor exec rewrite,
   Rust `find_program`, exec classification) required absolute-symlink
   dereferencing back into the rootfs.
4. Flattening further: glibc guests on Android 15+ die if we apply the
   musl table statically (faccessat/access wrappers need real semantics).

## Decision

- **Split emulation tables by flavor**: glibc artifacts + supervisor base
  table = `{99 set_robust_list, 293 rseq}`. musl ldso + supervisor extra
  table (kind==3 or flavor==musl) = `{48, 143..152, 159}` on top.
- **musl runs entirely under the supervisor** (`sprout-ptrace G-function,
  kind=3 "MUSL-DYNAMIC"`): the initial image is the musl-loader chain
  wrapped in `LaunchPlan::supervise`; busybox's suid-drop is swallowed at
  signal-stop level, and all path args get ptrace-side translation with
  the existence filter active. The musl interposer artifact remains
  injected via supervisor-chain env so busybox execs are also PLT-chained.
- **Sanitization is content-hash + table-version keyed**: when
  `EMULATED_SYSNOS_*` changes, the cache names change (prevents reusing
  unpatched artifacts). musl ldso is sanitized AND materialized as
  `~/.cache/sprout/musl-shadow-lib/libc.musl-aarch64.so.1` (real SONAME)
  and placed first in `--library-path` to satisfy musl's filename dedup.
- **Absolute-symlink chasing exists in three places with one algorithm**
  (Rust `Rootfs::resolve_absolute_symlink`, interposer `sp_translate_x`,
  supervisor `sp_resolve_absolute_symlink`): chase at most 8 hops,
  absolute targets re-translate, relatives pass through. Callers use
  dlsym(RTLD_NEXT) syscalls (no self-interposition recursion).
- **Interposer explicit-kill switch**: `SPROUT_DISABLE_L2S=1` reverts
  wrappers to plain translation (shell debugging aid).

## Consequences

Verified on Android 16 with proot-distro's alpine (musl 1.2.6):

- dynamic busybox + applet chains (`sh -c 'ls /etc | head'`) ✓
- suid-drop swallowed; `id`-family semantics stay truthful for glibc guests
- musl-static binaries via supervisor: rc=42 pass-through ✓
- python3 on musl with `subprocess` through the musl `__fdopfa` linked-list
  introspection (`__def/__mask` attr fields) ✓
- glibc battery unaffected ({99,293} only): bash/dash/python/Go static+dynamic ✓

Honest gap: stat() on applet symlink paths returns the *real* (busybox)
entry metadata, which is what the kernel would do on the guest — correct
semantics, but `/bin/ls` vs `/bin/busybox` identity is lost.

## Update (v0.5.2, 2026-08-12): the shadow knob has been flipped ON

The sequencing laid out above completed: syscall-order sequencing shipped
first, and the musl shadow gate (`SPROUT_SHADOW=1` now pushed by
`LaunchPlan::supervise`/`supervisor` for musl plans;
`SPROUT_MUSL_NOSHADOW=1` reverts) is validated with a 10.7× collapse in
per-spawn ptrace stops (15,922 → 1,484 debug events per 30-loop churn)
and a 2.45× med-of-20 speedup on exactly the pipe-flood profile that used
to lose to proot (0.76× → 1.8–2.1×). musl stat-family is now
interposer/notify-served like glibc; SIGSYS emulation of the set*id
family still arrives as signal-stops (works identically under CONT).
