# Building

## Native, on the Android device (Termux)

```sh
pkg install rust clang binutils
git clone https://github.com/sprout-os/sprout
cd sprout
cargo build --workspace
cargo test  --workspace
```

This builds all Rust crates and compiles + runs the C translation unit
tests. It also **skips** building `libsprout-core.so`, because a `.so`
linked by Termux' bionic toolchain would not work inside a glibc guest
(ADT: different symbol versioning, different `dlopen` semantics). The
loader-side `.so` is produced in CI on a glibc aarch64 toolchain and
fetched during local development:

```sh
# once, or after any .so change
curl -LO https://github.com/sprout-os/sprout/releases/latest/download/libsprout-core.so
# or point --rootfs tests at the .so you just built in CI
```

If you maintain a glibc cross-toolchain on the device and want the build
to produce the artifact anyway:

```sh
SPROUT_FORCE_PRELOAD_BUILD=1 CC=aarch64-linux-gnu-gcc cargo build -p sprout-preload
```

## On a glibc Linux aarch64 host (e.g. CI, RPi, Ampere VM)

```sh
cargo build --workspace
cargo test  --workspace     # produces libsprout-core.so under target/
```

The shared object lands in
`target/<profile>/build/sprout-preload-*/out/libsprout-core.so` and
`sprout_preload::core_library_path()` returns that path at runtime.

## Layout

```
crates/
  sprout-cli      # the sprout binary — parse flags, orchestrate
  sprout-core     # Rootfs / classify / Strategy / LaunchPlan (pure Rust)
  sprout-preload  # csrc/initer + Rust discovery lib
docs/             # mdBook: architecture, ADRs, guides
bench/            # repro benchmarks vs proot (see benchmark.md)
```

## Conventions

- No `unsafe` in Rust without a comment citing the exact preconditions.
- No allocation in the C hot path; everything goes through `SP_PATH_MAX`
  arenas.
- Every behavior change lands with (a) a test and (b) an ADR if it
  crosses strategy decisions.
- `cargo fmt --check` and `cargo clippy -- -D warnings` are CI-enforced.

## First-push CI checklist (todo #16)

Local dry-run (all green 2026-08-11, Android 16 / Termux):
`cargo fmt --check` ✓ · `cargo clippy -D warnings` ✓ · `cargo test` 23/23 ✓ ·
`actionlint` (rhysd/actionlint v1.7.8) on all 3 workflows ✓ ·
`mdbook build` (v0.4.52, static musl binary) ✓ · `cargo audit -n` rc=0 over 45 deps ✓.

Found-and-fixed before first push:
1. `docs/book.toml`: `[output.linkcheck]` must be `optional = true` — CI's
   actions-mdbook installs mdbook only; without the flag `mdbook build` fails
   rc=101 (verified locally).
2. `release.yml` docs job: `publish_dir: docs/book/html` — with two backends
   (html + optional linkcheck) mdbook ≥0.4.36 nests each backend's output;
   publishing `docs/book` would 404.
3. `test-termux` job moved to `termux-selfhosted.yml` (workflow_dispatch only):
   a `[self-hosted]` job with no registered runner queues 24h → CI red.
4. `.github/actionlint.yaml` declares the custom `termux` runner label.

Handoff (done 2026-08-13):
- repo is public at `github.com/YRangel/sprout`; push CI = lint (rustfmt +
  clippy) + mdBook on `ubuntu-latest` only. The ubuntu/ARM test/build lanes
  were dropped: testing glibc cargo harnesses on ubuntu proved nothing about
  the Android target — runtime gates live in `termux-selfhosted.yml`
  (manual dispatch until a Termux runner is registered).
- `ubuntu-24.04-arm` survives in exactly one place: `release.yml`, to produce
  the glibc-linked `libsprout-core.so` release artifacts (needs a glibc
  toolchain Termux's rust+clang can't provide).
- repo MUST stay public for free `ubuntu-24.04-arm` *release* runners.
- `git tag v0.4.0 && git push --tags` exercises release.yml end-to-end
  (collects sprout + libsprout-core.so + sprout-super + optional musl .so,
  strips, SHA256SUMS, GitHub Release, gh-pages docs deploy)

## Stable invariants for future refactors (ADR-0010 §"Refactor invariants")

If you ever restructure this codebase, these are load-bearing; violating
them silently regresses musl/X11/exec-chain behavior:

1. **Translation order** everywhere (interposer C, supervisor C, Rust
   helpers): bind grafts first → rootfs prefix → passthrough. Lookups
   use longest-prefix match (the bind table is sorted descending by
   guest length at load).
2. **AF_UNIX pathname discipline**: pathname sockets translate; abstract
   (NUL-first) sockets pass through; NO lstat/chase for sockets; 108-byte
   sun_path cap = passthrough.
3. **Memoization contract** (`sp_xcache`): positive results only, keyed
   on the GHUEST path string, config frozen at init, collisions tolerated
   as extra probe, NOT shared between processes (process-local only).
4. **Sanitize cache key** = `content_hash(input_bytes || table_bytes)`;
   the table version bumps automatically invalidate.
5. **Plan env keys** (Rust side both layers honor):
   `SPROOT_ROOTFS`, `SPROOT_LIBRARY_PATH`, `SPROUT_GUEST_PRELOAD`,
   `SPROUT_LOADER`, `SPROUT_BIND`, `SPROUT_LIBC`, `SPROT_SUP_TIMEOUT`.
   Do not renumber or respell without updating plan.rs tests.
6. **Supervisor tracee kinds** (int): -1 unclassified, 0 dynamic
   (preload-governed), 1 static, 2 dynamic-Go, 3 musl-dynamic.
7. `--shared-tmp` is sugar for a single `Binding`; do not add side
   effects to it.
