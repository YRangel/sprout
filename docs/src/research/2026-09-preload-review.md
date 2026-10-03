# 2026-09 deep review: sprout_preload.c (interposer)

Scope: `crates/sprout-preload/csrc/sprout_preload.c` @ cdeb1aa, 5,680 lines,
461 functions (64 public interposed entry points). Method: structural map of
the API surface → profile of risky primitive usage → targeted reads of the
translation hot path, exec chain, fakeroot block, and stat family.

## Health profile (swept counts)

| primitive | count | verdict |
|---|---|---|
| `snprintf` | 249 | dominant; bounded everywhere inspected |
| raw `sprintf` | 0 | none (earlier grep hits were `snprintf` substring noise) |
| `strcpy` | 9 | all in fixed-format digest code with explicit gate |
| `strcat` | 11 | one in symlink splice, length pre-checked (`need > sizeof merged`) |
| `memcpy` | 91 | length arguments consistently pre-computed vs `SP_PATH_MAX` |
| `memmove` | 3 | array-shift in xcache evict only |
| `malloc`/`free` | 51 | mostly DNS + exec-chain; see finding F4 |

## Findings

### F1 — FIXED: `execle()` mis-parses `envp` past 127 arguments

Variadic loop stopped *consuming* at the storage cap:

```c
while ((s = va_arg(ap, char *)) && i < 127) a[i++] = s;   /* cap hit → scan stops */
char **envp = va_arg(ap, char **);                         /* reads argv[127] as envp */
```

With >127 arguments the va_list cursor stays mid-argv, so `envp` picks up a
leftover `char*` argument — the child gets a garbage environment (and a real
env pointer is never read). Fixed by consuming to the NULL sentinel always
(`if (i < 127) a[i++] = s;` inside an unconditional loop). `execl`/`execlp`
truncate silently at 127 args; that is acceptable and unchanged.

### F2 — FIXED: `..` is now clamped to the virtual root at every translate point

*(supersedes the original breadcrumb: "symlink splice does not canonicalize `..`")*

The original finding documented a semantics divergence from proot: after
splicing a relative symlink target (`$B/usr/x → ../../etc/passwd`), the
resulting `$B/usr/../..` string is handed verbatim to the kernel — which
resolves it against the HOST root, reading files above the rootfs even
though the guest has no privilege boundary to cross (same uid; cosmetic
divergence, not a protection regression — proot's protection is the ptrace
fences, not a same-uid same-host chroot).

Fixed with `sp_dotdot_canon(p, floor_len)`: lexical canonicalization
(collapse `//`, drop `.`, pop `..` against prior components) that never
pops below the `floor_len` prefix (the rootfs, or the bind host anchor).
Call sites:

1. `sp_translate_f` rootfs-prefix tail — guest-spelled `/../x` and
   `/a/../b` are clamped BEFORE the kernel ever sees them (this also
   closed the direct-spelling variant of the escape).
2. `sp_translate_f` bind branch — the guest suffix onto a bind host anchor
   is clamped at the anchor (cannot climb above the bind).
3. `sp_resolve_intermediate_links` relative splice.
4. `sp_chase_final` — all three arms (relative/within-rootfs/re-prefix)
   canon after rewriting `out`.

Covered by regression cases in `csrc/tests/test_translate.c`
(`/../..//..`, bind-anchor `..`, `.`/`//` normalization).

### F3 — FIXED (mitigated): hop-budget exhaustion clamps the kernel-visible tail

*(supersedes: "chain hop-limit silently accepts mid-resolution state")*

Original risk: when `sp_chase_final`'s 8-hop budget or the intermediate
walk's 16-hop budget ran out mid-chain, `out` could still name a symlink
whose GUEST-spelled absolute target the KERNEL then resolves HOST-side
(e.g. chain tail pointing at host `/system` — readable by the same uid,
so no privilege boundary broken, but a semantic escape and a divergence
from proot's ELOOP).

Fix: chase budget raised 8→16; on budget exhaustion with the final
component still an absolute symlink, the tail is re-prefixed once more
(`rootfs + target` + clamp) so the kernel's own 40-hop follow starts
inside the rootfs. Genuine loops then hit the kernel's ELOOP as in proot.
Residual: a chain longer than 16 hops of MIXED targets can still leave a
relative link whose closure escapes host-side — same-uid, non-boundary,
note only.

### F4 — FIXED (decision landed): `vfork` is now interposed as `fork()`

*(supersedes: "ADR-0014 no-heap-in-vfork contract vs current chain")*

ADR-0014 demands malloc-free chain building ("chains run under vfork-shared
frames"), but `sp_execve_chain` mallocs its 20 KiB frame slab and
`sp_classify_host` fopens. Safe even before this fix for all *interposed*
spawn paths (posix_spawn/system/popen all use fork()), with one residual:
a guest calling `vfork()` directly and then `execve()` once landed in the
heapy chain while the parent was CLONE_VFORK-frozen — UB-but-works.

Fix: interpose `vfork() → fork()` (plain wrapper at the spawn-adjacent
block). fork() is a strict semantic superset for well-behaved callers
(the child may touch memory) and removes the hazard class entirely; the
ADR-0014 strict-contract comments ("no heap in vfork frames") are true
again because no vfork frame can reach the chain anymore.

### F5 — FIXED: fstat-family nlink spoof via `/proc/self/fd` reverse lookup

*(supersedes: "hardlink-registry nlink spoof is path-only")*

`fstat`/`fstat64` (and the fd-side of the statx emulation path) now run
the link2symlink nlink parity just like the path wrappers do: read the
fd's host spelling via `syscall(SYS_readlinkat, "/proc/self/fd/%d")`
(RAW — the interposed readlink would recurse), reverse-map through
`sp_reverse`, and bump `st_nlink` 1→2 iff the guest path is in the `l2s`
registry. useradd/stat-parity probes over fds now see the same answer as
path stats.

### F6 — NEW (found while testing F1): >127-arg execle chain SIGSEGV even post-F1

Reproducer: `crates/sprout-preload/csrc/tests/test_execle_env.c`, built
with `-DEXECLE_FILL_F130` (compile in any glibc guest, run via
`sprout -r ~/roots/debian --user=0:0 -- /root/sptest/te_F130`).

- F30 (30 filler args): PASS — child prints `marker_alive`, rc 0.
- F130 / F300 (>127 args, i.e. the wrapper-truncated argv): rc 139,
  SIGSEGV in the SECOND-level exec — the interposed `execle` builds the
  capped argv correctly (F1 fix verified: scan-to-NULL reaches real envp),
  then `sp_execve_chain` execs `ldso-sanitized` with ~131 argv entries and
  the new image's init crashes after the auxit/auxfix probe opens
  `/proc/self/auxv`, `/proc/self/maps`, `sp-auxfix.log` (ptrace log: child
  stopped sig=11 right after those reads).
- Direct launcher execs with 300 real args (`sprout -- /bin/sh -c ...f299`)
  work fine — so plain big-argv ldso startup is OK; the crash is specific
  to the chain-into-loader path when argv crosses ~127+.

Suspects: AT_EXECFN stack-string rewrite in the auxfix block (l.~600-660),
which memcpy's SPROUT_EXE into the auxv slot assuming loader-path length;
or a stack-scan heuristic that walks argv on the new image's stack. NOT yet
root-caused; no plausible fix landed (in-flight edit abandoned for being
sloppy — see session notes). Run F130 against any fix attempt.

## Not audited in depth (next pass candidates)

- `sp_dns_*` hand-rolled wire parser (l.1472–1639) — bounds look consistent
  on skim but deserves a fuzz or property test.
- `sp_statx_emulate` struct field mapping (l.2522) — verify alignment vs
  kernel statx ABI on arm64.
- Socket sockaddr rewriting (`bind`/`connect`/`get{sock,peer}name`,
  l.3134–3371) and the ashmem-tracking ring (`sv_ashmem_*`).
- `sprout_shadow.c` / `sprout_bridge.c` companions — separate review.

## Verification

- Edit compiles clean inside glibc guest: `gcc -fsyntax-only -DSPROUT_INTERPOSE`
  → EXIT=0, only pre-existing const-qualifier warnings (3833/4816).
- Smoke 8/8 and proot-compat 11/11 were green at review start (task 22
  baseline, commit cdeb1aa).
