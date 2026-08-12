# ADR-0012: apt/dpkg Compatibility Layer — Missing Interposer Symbols

- **Status:** Accepted (2026-08-11)
- **Context:** sprout v0.4.2 ran apt updates and shell loops fine, but a
  complete `apt-get install` chain (downloaded archives, reassembly, unpack,
  post-install scripts) failed in multiple symptomatic ways:
  "Sub-process /usr/bin/dpkg returned an error code", silent unpack aborts
  around package N=40, `dpkg-deb: unable to create temporary directory`, and
  rare `malloc assertion` deaths. proot-distro ran the same cycle purely.
  The bug surface spans archive-management (dpkg-deb), archive verification
  (sqv), and the dpkg main progra's post-install script execution.

## Root causes found (each distinct, per-symptom)

1. **Link-family translation holes.** dpkg-deb's consolidate stage uses
   `link()`/`linkat()` for backup files (`/var/lib/dpkg/status-old`) and
   `renameat()`/`renameat2()` for in-place commits, none of which were
   interposed in v0.4.2's preload set. The translation table also failed
   to consider `symlinkat`'s sibling-signature form (arg0 vs arg1 arg
   layout differences between musl/glibc callers).

2. **Android untrusted app domain hardlink restriction.** SELinux +
   filesystem rules on /data/data refuse hardlinks between different dirs
   for untrusted apps, so `link()` returned EACCES (wrapped as EPERM).
   `--link2symlink` mode existed but the fallback only covered `EPERM`,
   not the EACCES actually delivered. The interposer creates symlinks
   *placed under the guest's view*, so dpkg's binary-copy backup becomes
   a valid symlink. Matches proot's L2S extension behavior.

3. **Set-group ownership allowances.** apt's sqv verification stage and
   download commit stage run `chown`/`chmod` against downloaded artifacts.
   Under the proot-distro plugin (which always passes `--link2symlink`)
   proot always pretends `chown` works; sprout only faked chown under
   `-0`. Solution: fakeroom and `--link2symlink` both authorize
   skip-reconciliation of chown/chmod (return 0) while keeping inode-write
   rights untouched — same contract as proot's --link2symlink.

4. **PATH-search start point for variadic exec.** glibc's execlp calls
   `execvp` directly via PLT with `argv[0]` pointing at the program *name*,
   not an argv `execve()` call with a diced-up absolute candidate, so our
   execvp() was the right interception point but `SP_AVOID_SOME_SPACES`
   filtered "debian ".. resolv.conf.

5. **Signature-verification staging in /tmp.** apt' sqv relies on
   mkstemp(“/tmp/apt.sig.XXXXXX”) — glibc mkstemp's internal open is a
   raw syscall, not a wrapped entrypoint, so our mkstemp wrapper never
   fired and EACCES resulted (unwritable /tmp). We now wrap mkstemp /
   mkostemp / mkdtemp / tmpfile and mirror glibc's trailing-6
   substitution back into the caller's guest buffer.

6. **exec-chain argv construction.** execvp() grafts the fallback
   `sp_guest_path_search()` for PATH entries, but an older helper
   (uuid/cgroup disc) returned `EPERM` when opening /proc/self/uid_map
   resolved weirdly due to a shadow-supervisor musl ELE kernelcap failed
   opening... (mis-set flags). Actual root: /proc/self/uid_map file
   missing under the rootless model — glibc's reparse tries accessing
   it — misreported as "Permission denied" due to inheriting errno from
   the failed open: fixed to proper ENOENT handling and to *not* call
   chmod against rootfs-owned skeletons.

## Design rules deduced

- **Interposition regressive contract:** every *mutation-class* glibc
  entrypoint used by any "industry distribution toolchain" (dpkg, apk,
  rpm-free equivalents) must be wrapped *or have its musl/glibc DSL
  emulate-success* for the path-crossing space the rootfs Label contract
  lives in. Auditing via objdump imports a specific distribution binary
  is narrower than what this needs; error-platform tests are the safety
  net.

- **goexcept/ptr decoding for two-class emulation:** when the supervisor
  must emulate under shadow it does NOT examine the interposer's `LD_PRELOAD`
  chain; instead keep the mechanism right: sigsys swallow table + fake-chown
  by default environment + never re-exec nested sprout-ptrace (rootfs
  executables spawned by shadow children are re-classified in place by
  exec stop).

- **Verification:** apt-sl; apk nodejs; anonymous-BASH rc=42 parity vs
  proot-distro; install-cycle clean; regression suite & cargo tests.

## Consequences

The entire dpkg tool stack and apk/apk2 chains run correctly from
download→extract→configure→activate inside sprout rootless guests, with
identical behavior to proot-distro within proot's documented
compatibilty envelope. SQV signature-verified fetches, assorted build
tool chains using libc's exec-family variadic call forms, and
suid-adjacent accesses all behave deterministically.

Refs: ADR-0003 (no `.text` patching — all fixes at exported PLT level),
ADR-0007 (sanitized-chain), ADR-0009 (musl), ADR-0010 (perf+x11+shared-tmp),
ADR-0011 (Android-blocked syscalls at PLT).
