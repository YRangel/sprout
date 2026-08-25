# ADR-0022: sprout upkg — in-Rust tar extractor for the no-rootfs bootstrap case

**Status**: Accepted (2026-08-25)

## Context

proot's rootfs bootstrap pattern is:

```
proot --link2symlink tar -xJf rootfs.tar.xz
```

This runs **host-side tar** with link() rewriting — no guest rootfs required.
It's how users who never had a guest build one.

sprout's `-r ROOTFS` contract requires the rootfs to already exist. The
interposer shim (libsprout-core.so with link() downgrade) only runs on
*guest* code under a selected preload or ptrace lane. Applying it to host
binaries hits the bionic-vs-glibc ABI wall: shim NEEDs libc.so.6, bionic's
namespace provides libc.so; LD_PRELOAD of the glibc shim under
/system/bin/linker64 fails with "library libc.so.6 not found".

## Decision

Add a leaf subcommand `sprout upkg TARBALL [-C DIR]` that extracts
in-process, in Rust (tar crate, plus flate2 / xz2 / bzip2 auto-detected by
magic bytes), with the SELinux-aware policy applied at write time:

  - **HardLink entries are materialized as full content copies** from
    their already-extracted target (never libc link() — SELinux denies
    cross-mount hardlinks).
  - setuid/setgid bits dropped from stored permissions (fake-root
    parity: sprout never runs as root-kernel-side, so these are
    unenforceable at runtime; masking them at write time avoids
    surprises for later callers that honor them).
  - CharDev/BlockDev/Fifo entries skipped with a count warning —
    unfixable rootless.
  - Absolute paths and `..` rejected outright (path traversal guard).
  - umask 077 (Termux app default) reapplied after unpack so archive
    modes are preserved (cg: tar-open-time umask sets 0600 even for
    0644-declared files otherwise).

## Why this shape

- The proot recipe (`--link2symlink tar`) is a *host-mode escape hatch*.
  Sprout replicated the "escape hatch" intent as a zero-rootfs-required
  subcommand that doesn't have to be prefixed by -r.
- Keeping it subcommand-shaped (peeked in argv[1] before clap) keeps the
  proot-flag surface unchanged for users who never touch it.
- Re-implementing tar-walk in Rust keeps the hardlink→copy policy under
  our control. Host-tar on Termux already silently degrades hardlinks
  (inode comparison shows different inodes post-extract), but it does so
  *after* $TMP-write side effects we can't police precisely; doing the
  walk natively also avoids shipping a separate ABI-split host-shim
  artifact (see §Background above).

## Consequences

  - New workspace deps: tar, flate2, xz2, bzip2 ~ +200 KiB release-mode
    binary.
  - New artifact lane in release.yml not needed (no new DSO); the
    subcommand compiles into the regular sprout binary.
  - Test: tests/smoke.sh gate 7 builds a tar with a hardlink pair, runs
    `sprout upkg`, asserts both files exist and contents match.
  - Drop-in check: fresh debian rootfs (90 MiB xz) extracts in ~7.6 s
    on this device and boots directly under `sprout -r <dir>`.

## Future work

If bionic-shim ABI work ever lands, `sprout --link2symlink tar -xJf`
could reuse this core; today the subcommand is the sure path.
