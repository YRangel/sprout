# Roadmap

| Milestone | Scope | Status |
|---|---|---|
| **v0.1** | Core: workspace, ELF classify → preload strategy, loader-launch, CLI surface | **in flight** |
| **v0.2** | execve chaining: shebang scripts, `sh -c`, subprocesses inherit `LD_PRELOAD` | planned |
| **v0.3** | ptrace fallback: static/Go binaries work out of the box | planned |
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

## v0.2 design sketch (execve chains)

The preload core's `execve` wrapper becomes real: it prepends the loader
to the target argv, keeps its own `SPROUT_*` env, and injects itself into
the child's `LD_PRELOAD`, preserving the loop across exec. Scripts
(`#!...`) unpack their interpreter in the wrapper instead of the kernel.
Single-file tests in the interposer gate this.
