# ADR-0015: path semantics — cwd defaults, relative paths, dirfd authorships

## Status

Accepted (v0.5.0)

## Context

Three compounding path-semantic bugs surfaced during the dogfood of the
drop-in parity defaults (fakeroot + link2symlink on by default):

1. **cwd inheritance**: sprout dropped children into the host cwd
   (e.g. `/home/projeto`). Proot's rule is: if cwd is inside the guest
   rootfs keep it, else `/`. With the host cwd outside the rootfs, any
   root-relative journal (apk's `installed.tmp.<pid>` rotation in the
   gdb-style execute, dpkg's status backups) landed as junk files in the
   user's repository checkout, and packages reported "failed to write
   database: No such file or directory".

2. **blind relative translation at the interposer**: relative path args
   (e.g. `open("installed.tmp.1744")`, `open("lib/apk/db/lock")`) were
   passed to the kernel untouched, landing relative to the REAL cwd —
   correct only when the real cwd happened to be inside the guest.

3. **dirfd-relative paths translated against cwd**: apk's db rotation
   goes through `apk_ostream_to_file(db->root_fd, "installed.tmp.PID")`
   — kernel-level directory-relative references (`openat(dirfd,
   "installed.tmp.PID", ...)`). Translating that name against cwd
   falsifies the target directory; observed as `No such file or
   directory` at rotation time.

## Decision

- **Default cwd**: guests start at `/` (rootfs root) unless `-w` is
  given; the host cwd is used only when it lies inside the rootfs.
  Proot parity, junk-proof.
- **Interposer relative paths**: turned absolute first by joining
  against the process's own `/proc/self/cwd` (host view, rootfs-prefix
  stipulation), then run through the standard rootfs translation.
- **dirfd-relative paths (`dirfd != AT_FDCWD && path[0] != '/'`)**:
  pass through RAW at the interposer (the dirfd is an fd in the host
  table; the kernel resolves it exactly). Supervisor notify server was
  already CONTINUE-ing those; the ptrace-path rule gate was already
  AT_FDCWD-only. Applied to: openat/openat64/fchmodat/fchownat/
  utimensat/fstatat/newfstatat/statx/mkdirat/unlinkat/readlinkat/
  renameat/renameat2/linkat/symlinkat (+fstatat64/fstat64 added for
  glibc-2.33-style imports).

## Consequence

apk add/del cycle completes with a consistent database and no junk
files (`/home/projeto/installed.tmp.*`, `$A/installed.tmp.*` stopped
being created); apt/dpkg rotation, git's `.git` journal and all the
prior normal-usage suite continue green (15/15). One cosmetic residual
remains: apk's busybox-1.37.0-r31.trigger reports a 127 inside apk's
own sandbox (del-only, no state inconsistency; tracked as a known
limitation, does not affect the package database).
