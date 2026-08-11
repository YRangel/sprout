# ADR-0003: Loader-launch — execute the guest loader, never patch .text

- **Status:** accepted
- **Date:** 2026-08-11
- **Deciders:** project owner + initial architecture

## Context

To run a guest binary that expects its libraries under, say,
`/lib/aarch64-linux-gnu/`, we need (a) the guest's own glibc dynamic loader
running and (b) an `LD_PRELOAD` entry pointing at our interposer. There are
two ways to get there:

1. **Patch the binary's `.text`** — rewrite entry or relocation fixups so
   that startup immediately loads our shim. proroot does this.
2. **Execute the guest loader explicitly** — `execve` the guest's
   `ld-linux-aarch64.so.1` directly, passing `--argv0`, `--inhibit-cache`,
   and `--library-path`, so it maps the target binary *as a normal
   invocation*.

Option 1 is brittle: it silently depends on glibc's internal startup layout
and breaks when the binary is PIE in a different way than expected, when
hardening flags change, or when we later try to support execve chains.
Option 2 uses the loader's documented CLI surface (glibc 2.33+; every Ubuntu
jammy / Debian bookworm / Android-supported glibc satisfies this).

## Decision

A `Strategy::Preload` launch is always built as (see
`sprout_core::plan::LaunchPlan::preload`):

```
<rootfs>/lib/ld-linux-aarch64.so.1 \              # ← what the kernel execs
    --argv0 <original argv[0]> \
    --inhibit-cache \                              # ignore /etc/ld.so.cache
    --library-path <host abs path>:<more> \        # guest libs, resolved host-side
    <host absolute guest program> [args...]
```

`LD_PRELOAD=<host abs path>/libsprout-core.so` is in the environment; the
guest loader maps the interposer into every dynamically linked descendant,
including future `execve` chains (v0.2).

No bytes of any guest binary are ever modified. Patching `.text` is an
explicitly rejected pattern in this codebase and would fail review.

## Consequences

**Easier:** works for any glibc 2.33+ binary regardless of hardening
(relro, bind-now, PIE layout); inspectable with the existing
`--dry-run`/`--verbose` flags; survives guest upgrades because we only
rely on documented loader semantics; the exact command line a human would
run is the command sprout executes, so debugging is trivial
(`./sprout --dry-run --verbose /bin/bash -l`).

**Harder:** requires glibc ≥ 2.33 (v0.1 baseline is Ubuntu jammy-22.04 /
Debian bookworm, both 2.35); requires the rootfs to contain a working
loader at one of the three candidate paths (`/lib/ld-linux-aarch64.so.1`,
`/lib64/ld-linux-aarch64.so.1`, `/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`)
— `Rootfs::guest_loader()` fails with a clear error otherwise.
