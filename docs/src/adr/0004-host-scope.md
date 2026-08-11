# ADR-0004: Host scope — Termux-first, aarch64, Android 8+

- **Status:** accepted
- **Date:** 2026-08-11
- **Deciders:** project owner + initial architecture

## Context

The motivating use case is running Linux dev tools on Android phones,
which means Termux (or an app embedding an .so) on arm64 Android. But
"Android" spans API levels from 21 upward and four ABIs; a v0.1 that claims
to support everything will support nothing reliably.

SELinux is enforced on every supported device. Unprivileged user-namespace
creation is not available on stock kernels (`unprivileged_userns_clone` is
absent), so the entire class of "use mount namespaces to fake a chroot"
designs is off the table on devices we care about.

## Decision

v0.1 targets exactly:

- **Host OS:** Android (Termux environment for CLI; `.so` embeddable later)
- **API level:** 26+ (Android 8.0) — the earliest level with the modern
  linker namespace APIs we depend on and Termux's documented floor
- **Architecture:** `aarch64` only (`arm64-v8a`); x86_64 Android hosts
  (ChromeOS, emulators) run their guests via Box64 or similar, which is a
  separate project

On-device builds produce the Rust binaries natively (Termux clang/rustc
work fine); the glibc-targeted `libsprout-core.so` is built in CI on
`ubuntu-24.04-arm` and shipped as a release artifact. Building the `.so`
directly inside Termux is intentionally rejected because the resulting
object would be linked against bionic symbols and unusable in the guest
(see `sprout-preload/build.rs`).

## Consequences

**Easier:** a tight, testable matrix; CI runners are cheap; the Termux
dev-loop is `./sprout` against the device you already hold.

**Harder:** API 26 excludes old but still-deployed phones (acceptable for
v0.1; revisit in v1.0 if user demand appears); x86_64 Android hosts are
explicitly deferred, which means ChromeOS is out of scope until guest
emulation is tackled.
