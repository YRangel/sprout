# ADR-0005: Guest scope — aarch64 glibc only for v0.1

- **Status:** accepted
- **Date:** 2026-08-11
- **Deciders:** project owner + initial architecture

## Context

A "Linux userspace" is not one thing. Even on aarch64, the two common
choices are glibc (Ubuntu, Debian, Fedora) and musl (Alpine, postmarketOS),
and they differ in loader name, ldconfig behavior, NSS modules, and symbol
versioning. On top of that, many users want x86_64 binaries via Box64.

Trying to support everything means v0.1 ships nothing.

## Decision

The guest matrix is fixed for v0.1:

| Property | Choice |
|---|---|
| Architecture | aarch64 |
| libc | glibc ≥ 2.35 (Ubuntu jammy, Debian bookworm, Fedora 36+) |
| Loader | `ld-linux-aarch64.so.1` at canonical paths (see ADR-0003) |
| Distribution | any glibc-based distro in the `proot-distro` catalog |

Explicitly deferred:

- **musl / Alpine** — different loader name (`ld-musl-aarch64.so.1`), no
  `PT_INTERP`-compatible semantics for `--library-path`, needs separate
  deps handling. Tracked for v0.4.
- **x86_64 guests via Box64** — an emulator bridging problem, not a path
  problem. Tracked post-1.0.
- **32-bit guests** (armhf/i686) — immediately returned as `GuestClass::Elf32`
  with a clear error; not planned.

The boundary is enforced by `sprout_core::elf::classify` and tested in CI
so future regressions in detection are caught before release.

## Consequences

**Easier:** everything — CI matrix size, test fixtures, README honesty, and
the loader path search (three paths, not thirty).

**Harder:** Alpine users have to wait; they are a vocal minority.
Mitigation: the rootfs-binding core (`SPROUT_ROOTFS`/`SPROUT_BIND`) is
libc-agnostic, so a musl backend later is additive, not a redesign.
