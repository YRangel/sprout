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
