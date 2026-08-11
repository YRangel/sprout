# ADR-0001: Language split — Rust launcher + C11 LD_PRELOAD core

- **Status:** accepted
- **Date:** 2026-08-11
- **Deciders:** project owner + initial architecture

## Context

A rooted-in-userspace runtime has two very different kinds of code:

1. **Launcher/CLI** — parses flags, classifies ELFs, builds a launch plan,
   resolves loaders. Complexity is in correctness and UX. Rust is the obvious
   fit: sum types for the plan, exhaustive matches, rich error messages, no
   buffer overflows, easy cross-compile, great library ecosystem.

2. **LD_PRELOAD interposer** — every glibc path-taking libc call lands here.
   This code is constrained in ways that make Rust the *wrong* choice:

   - **Must interpose libc symbols.** `LD_PRELOAD` works by shadowing libc
     symbols like `open`. Any Rust runtime in that context (panic machinery,
     stack-probe support, thread-local init) increases the surface that can
     be invoked *before libc is fully initialized* or in async-signal contexts.
   - **Must not allocate in the hot path.** `open("/dev/pts/2")` on every
     frame of a Chromium render loop cannot afford a `Vec` growth; the
     interposer must do pure pointer math and bounded `memcpy`.
   - **Must be a tiny shared object.** Every byte of the `.so` is loaded into
     every guest process. Rust's monomorphised stdlib is overkill here.
   - **No panics, no unwinding.** A panicking interposer takes the guest
     process down with an opaque abort; C error returns compose with libc's
     conventions.

## Decision

The launcher, strategy logic, shell-out behavior, and all user-facing error
messages live in **Rust** (edition 2021, workspace members `sprout-cli` and
`sprout-core`).

The LD_PRELOAD path-translation core lives in **C11**
(`crates/sprout-preload/csrc/`), built as a `cdylib` via the `cc` crate. The
pure translation functions (`sp_translate`, `sp_reverse`) are isolated in a
header-consumable module and tested natively on any host; the interposition
wrappers are guarded by `SPROUT_INTERPOSE` at compile time.

The Rust side of `sprout-preload` is a thin artifact-discovery library; it
contains no runtime logic.

## Consequences

**Easier:** auditable hot path, no Rust-in-interposer pitfalls, ~15 KiB `.so`,
trivial LTO, standard glibc interposition semantics, easy benchmarking against
proot's own preload mode.

**Harder:** two toolchains in CI (stable Rust + a C11 compiler), and the C
side needs its own test harness (currently: a small native binary run from
`cargo test`). FFI surface kept at zero: the preload core communicates with
the launcher only via process environment (`SPROUT_*`), never by calling
back into Rust.
