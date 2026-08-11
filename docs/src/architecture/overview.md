# Architecture overview

sprout is four small layers, each independently testable:

```
┌───────────────────────────────────────────────────────────┐
│ sprout-cli  (Rust)                                        │
│   --rootfs/-r, --bind/-b, --cwd/-w, -0, --link2symlink,   │
│   --fallback, --dry-run, --verbose, <COMMAND [ARGS...]>   │
├───────────────────────────────────────────────────────────┤
│ sprout-core (Rust)                                        │
│   Rootfs └── Binding   ELF classify   Strategy   LaunchPlan│
├───────────────────────────────────────────────────────────┤
│ sprout-preload (C11, LD_PRELOAD cdylib)                   │
│   sp_translate / sp_reverse  ← pure, unit-tested          │
│   open/stat/execve wrappers ← thin, compile-gated          │
├───────────────────────────────────────────────────────────┤
│ glibc guest loader (from the guest rootfs, not from us)   │
│   ld-linux-aarch64.so.1 --argv0 --inhibit-cache           │
│       --library-path <host abs dirs> <guest program>      │
└───────────────────────────────────────────────────────────┘
```

## The request lifecycle

```
$ sprout -r ~/ubuntu -0 -w /root -b /sdcard:/mnt/sd -- /usr/bin/node server.js

1. CLI parses flags → Rootfs { root:/home/u/ubuntu, bindings:[...], ... }
2. classify ELF(/home/u/ubuntu/usr/bin/node) → Dynamic { interp:"/lib64/..." }
3. Strategy::for_elf → Preload
4. LaunchPlan::preload builds:
     loader    = /home/u/ubuntu/lib/ld-linux-aarch64.so.1
     argv      = [--argv0 node, --inhibit-cache,
                  --library-path /home/u/ubuntu/lib/aarch64-linux-gnu:...,
                  /home/u/ubuntu/usr/bin/node, server.js]
     env       = SPROUT_ROOTFS=/home/u/ubuntu
                 SPROUT_BIND=/sdcard=/mnt/sd
                 SPROUT_FAKEROOT=1
                 LD_PRELOAD=<host>/libsprout-core.so
                 LD_LIBRARY_PATH=<same as --library-path>
5. exec → kernel runs the guest loader → loader maps node + libsprout-core.so
6. node calls open("/etc/passwd") → libsprout-core.so's open() wrapper
   → sp_translate → open("/home/u/ubuntu/etc/passwd") → real libc open()
7. zero context switches; node runs at native speed until it exits
```

## Invariants

- A translation is always idempotent: translating a translated path is a no-op
  (this is what makes `execve` chaining safe).
- Every binding is applied *before* the rootfs prefix, so `-b /sdcard:/data`
  wins even though `/data` would otherwise become `<rootfs>/data`.
- The preload core never allocates and never calls back into Rust; its only
  inputs are the process environment and the path string.
- Nothing on disk is ever written to, patched, or renamed.
