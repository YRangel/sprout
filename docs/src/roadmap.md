# Roadmap

| Milestone | Scope | Status |
|---|---|---|
| **v0.1** | Core: workspace, ELF classify → preload strategy, loader-launch, CLI surface | **done** (`-b/-w/-0/--link2symlink/-0` accepted; `--fallback` reserved for ptrace path) |
| **v0.2** | execve chaining + shebang scripts + `system()`/`posix_spawn()` wrappers so guests spawn guests inside sprout | **done** |
| **v0.3** | ptrace fallback: static binaries (SIGSYS swallow, dirfd-family translation, execve rewrite incl. static→dynamic/script via loader chain) | **done** (Go untested) |
| **v0.4** | musl/Alpine guests (own loader name, no `--library-path`) | planned |
| **v0.5** | embeddable `.so` + Android Library (AAR) for other apps | planned |
| **v0.6** | reproducible benchmark suite published, regression alerts in CI | planned |
| **v1.0** | proot-distro endpoints all pass on sprout as the backend | goal |

The numbers are set by ADRs (0002, 0004, 0005); changes to goals get their
own ADR.

## v0.1 exit criteria

- `sprout -r /path/to/ubuntu /usr/bin/node --version` prints the node version.
- `cargo test --workspace` green on Termux and on ubuntu-24.04-arm.
- `--dry-run` documents the full exec plan for any guest ELF.
- No `.text` patching, no `ptrace`, no temp files created at run time.

## v0.3 design sketch (static binaries / exec-fallback supervisor; ptrace last resort, ADR-0002/)

The preload core's `execve` wrapper becomes real: it prepends the loader
to the target argv, keeps its own `SPROUT_*` env, and injects itself into
the child's `LD_PRELOAD`, preserving the loop across exec. Scripts
(`#!...`) unpack their interpreter in the wrapper instead of the kernel.
Single-file tests in the interposer gate this.
