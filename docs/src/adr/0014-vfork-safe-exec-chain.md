# ADR-0014: vfork-safe exec chain - no heap allocation inside children of posix_spawn

## Status

Accepted (v0.5.0)

## Context

`debian dash` spawns commands via `posix_spawn()`, which on glibc is
internally `clone(CLONE_VM | CLONE_VFORK)` - the child **shares** the
parent's entire address space until execve. Any library code that runs
in that window must obey "only modify pid + exec" discipline.

The interposer's exec-chain (`sp_execve_chain`) used `malloc` for the
translated argv array. In a vfork-child, that malloc goes straight into
the parent's live glibc arena. On the device, `dash` dies a few
milliseconds later with the canonical malloc topology assertion:

```
Fatal glibc error: malloc.c:2601 (sysmalloc): assertion failed:
(old_top == initial_top(av) && old_size == 0) || ...
```

The crash was reproduced deterministically on v0.4.2, v0.4.3 and v0.5
snapshots: `dash -c 'ls >/dev/null; true'`. bash and python are
completely immune - both spawn via plain COW `fork()`.

## Decision

All exec-chain structures are built in **caller-stack memory**, never
the heap:

- `sp_build_loader_argv(char **v, size_t vmax, ...)` receives a
  caller-supplied stack array (`vstack[SP_CHAIN_MAX_ARGS + 8]`).
- The shebang/script tail builds its chained argv in a stack-local
  `vchain` array likewise.
- Nothing in the chain path calls `malloc`, `free`, `realloc`,
  `strdup`, or any glibc allocator entry point.

## Consequences

- Crash eliminated: dash pipelines/braces/while loops, `ls; true`,
  `ln; ln -s` all rc=0. bash/python unchanged (already fine).
- Simpler than an arena + lock: stack memory is correctly scoped to the
  exec syscall that consumes it; the execve replaces the image and the
  memory's lifetime ends exactly at the call site.
- Chain depth remains depth≤4 (shebang recursion) with a 256-argument
  ceiling per level - both unchanged.

## Important build note discovered this round

The interposer must be compiled with the same flags the cargo build
uses: `-DSPROUT_INTERPOSE -fPIC -shared ... -ldl`. Plain `gcc -shared
-fPIC` yields an artifact whose wrapper exports are compiled out - and
guest exec then degrades to unprefixed host execution (on this box:
bionic linker errors like `CANNOT LINK EXECUTABLE "ls": library
"libcrypto.so" not found: needed by main executable`, from the toybox
ware that accidentally execs in `/system/bin`'s place).
