# Roadmap

| Milestone | Scope | Status |
|---|---|---|
| **v0.1** | Core: workspace, ELF classify → preload strategy, loader-launch, CLI surface | **done** |
| **v0.2** | execve chaining + shebang scripts + `system()`/`posix_spawn()` wrappers so guests spawn guests inside sprout | **done** |
| **v0.3** | ptrace fallback: static binaries (SIGSYS swallow, dirfd + cwd-relative translation, execve rewrite incl. static→dynamic/script) | **done** |
| **v0.4** | musl/Alpine guests: supervisor-first route, musl sanitizer table, SONAME-shadow libc, absolute-symlink chasing, suid-drop swallow | **done** |
| **v0.5** | proot-distro flag parity + X11/desktop lanes: `--termux-x11`/`--shared-tmp`, `-k/--kernel-release`, `-p/--port-mapping`, `--kill-on-exit`, `--sysvipc`/`--ashmem-memfd` acceptance, `MOZ_DISABLE_WASM_SIGHANDLERS` doc | **done** |
| **v0.5.1** | hardening + perf pass: env-wipe exec-chain survival, library-stamp normalize, exec-name memo, `-P/--redirect-ports` aliases, fake-proc full table (version/uptime), statx emulation, host-bionic `-q` lane, CLI unknown-flag discipline + `-i/--change-id`/`-L` aliases, LibreOffice class | **done (tip)** |
| **v0.6** | publish workflow + mdBook online, reproducible benchmark cards, hour-batteries in CI (healthcheck.sh lanes on both phone classes) | in-flight |
| **v0.7** | alpine/musl lane hardening ‖ arm32 guest lane ‖ wine-class canvas on box32 | scoped |
| **v1.0** | sprout-distro: proot-distro-style UX shipped on sprout as the backend (install/uninstall/login/backup with byte-identical flags) | goal |

The numbers are set by ADRs; goal changes get their own ADR.

## Current focus queue (post-0.5.1)

1. Management UX polish: login shortcuts, helper scripts (`start-desktop.sh`,
   `x11-rescue.sh`, `stack-health.sh`, `healthcheck.sh`, `flagmatrix.sh`)
   → candidate to fold into a `sprout-distro` thin frontend.
2. Fake-proc table extension on demand (`/proc/net/dev` from `/sys/class/net`
   counters is first in line, only once an actual tool dies on it).
3. mdBook publish pipeline (drop-in gh-pages build from this branch).
4. perf-resident: notify-only statics benches in the matrix; HEAP footprint
   audit of the preload on long-running desktop sessions.

## v0.1 exit criteria (kept from the era, all satisfied years-go)

- `sprout -r /path/to/ubuntu /usr/bin/node --version` prints the node version.
- `cargo test --workspace` green on Termux (self-hosted runner, see
  development workflow notes).
- `--dry-run` documents the full exec plan for any guest ELF.
- No `.text` patching, no `ptrace`, no temp files created at run time
  (exceptions folders: `~/.cache/sprout` derivatives, norm-stamps).
