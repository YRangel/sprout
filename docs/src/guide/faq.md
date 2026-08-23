# FAQ

## Launcher / CLI

**`--rootfs` "not provided" even though I typed it?**
You were bitten by the trailing-vararg swallow (fixed). If your sprout is
older than this change, *any* unrecognized `-flag` before the command
captured everything after it — including `--rootfs=...` — into cmd, and
clap then correctly reported rootfs missing while your `--rootfs=...` sat
invisibly inside it. Upgrade. The current build hard-fails unknown flags
on contact with a `--` tip, and proot muscle-memory flags (`-i`,
`--change-id`, `-L`, `--mixed-syscall`) are accepted with notes.

**Options BEFORE or AFTER the command?**
Before. `sprout -r R --user=0:0 -- cmd args...`; separate with `--` when
the command itself begins with a dash. `--help` prints the rule + a
compat map at its tail.

**"guest program '-L' not found inside rootfs"?**
Same older bug class (unknown tokens after `--rootfs` became the guest
program name, not an error). Current build: `-L` parses as a compat
no-op printing a one-line note, then `cmd` resolves normally. Git the
current build (`./install.sh --verify`).

## Emulation / x86

**Can I run x86_64 apps?**
Yes — box64 inside the guest: install/build box64 anywhere under the default
lookup path (`/usr/local/bin/box64` in the rootfs) and plain
`sprout -r R -- ./x86-app` auto-wraps, no flags. `-q PATH` (or
`SPROUT_BINFMT_X86_64`) overrides the emulator path explicitly. Host (Termux) box64/qemu-user
works too, sprout detects the bionic emulator and direct-spawns it —
but Android blocks `set_robust_list` for host-bionic children and the
guest `ld-linux` path is invisible to it, so static x86 + fresh-dynamic
x86 guests should use the in-guest box64 lane.

**Sober / VinegarHQ configuration?**
Upstream builds of Sober target x86_64 only. On our aarch64 lane the
flathub entry doesn't exist. Use the native Android client or FEX/Box64.

**i386?**
Via box64's integrated box32 persona (`SPROUT_BINFMT_I386` if you have a
separate box86). Debian-box64 binaries are x86_64-only today.

## Syscalls / compatibility

**`Bad system call` when I do `syscall(291)` (statx) by hand?**
That's Android policy killing RAW `svc(291)` for the untrusted UID —
reminder: the host filter’s KILL decision outranks any user-notify answer, so the
process dies identically under proot. glibc's `statx()` wrapper DOES work:
the preload lane interposes it via PLT and answers through emulation when
necessary. Write apps through libc.

**LibreOffice/service says "ERROR: /proc not mounted"?**
Fixed: sprout fakes `/proc/version`, `/proc/stat`, `/proc/loadavg`,
`/proc/uptime`, `/proc/sys/kernel/overflow{u,g}id` at every level — memfd
answers, materialized file reroute, notify ADDFD, classic scratch-file —
AND the new statx emulation completes the story. LibreOffice 26.2 maps
a working window under this build ('Untitled 1 — LibreOffice Writer').

**Can I debug inside the guest (gdb/strace)?**
No — ptrace is owned by the supervisor. Debug from the host side or use
SPROUT_DEBUG -v traces. This is the same class as proot.

**mount/loop/namespaces?**
Kernel-block for every rootless runner. Out of this model.

## Desktop / X11

**Desktop opens, windows map, but everything is slow?**
Typomorph classes: (1) compositor/on for xfwm4: `xfconf-query -c xfwm4 -p /general/use_compositing -s false` fixes it on this device family; (2) you skipped `--shared-tmp` so the X socket is unreadable by root children; (3) HyperOS put the x11 display into zombie state — run `~/projeto/x11-rescue.sh`.

**Audio crackles or stalls after a termux pkg upgrade?**
pkg resets `$PREFIX/etc/pulse/default.pa` conffile. Re-add the
`module-native-protocol-tcp` line (`auth-ip-acl=127.0.0.1 auth-anonymous=1`).
`~/pulse-guard.sh` heals it automatically now.

## Correctness rules of thumb

**Debug an app crash: sprout's fault?**
Control-lane doctrine: run the same binary under `proot -r $D ...`
(`~/proot-control.sh`). Identical crash = the app is broken in
containerized environments; sprout can't fix that tax. Crash ONLY under
sprout = bug against us, file it with the repro.

**Fake root is privilege?**
`id -u == 0` inside a guest describes ITS address space only — the host
kernel's view never changed. If code trusts it for a security decision,
that code was already wrong under proot too.

## Installation / upgrades

**How to verify a dev `.so` build matches the deployed one?**
Rebuild in-guest with the canonical recipe and compare md5s. ⚠ the source
file must carry the SAME filename (`/tmp/sprout_preload.c`): gcc bakes
the name into `.note.gnu.build-id` + `.symtab` — `hc-preload.c` vs
`sprout_preload.c` yields a different binary for identical code. The
healthcheck (`~/projeto/healthcheck.sh`) does this L1 leg for you.

**What batteries run before a release?**
`cargo test --workspace`, `test_translate`, `cargo fmt --check`, `cargo clippy -D warnings`, `bench/run.sh`, `bench/run-statics.sh`, `flagmatrix.sh` 11/11, `healthcheck.sh` 0 fails on BOTH lanes (6.12 notify + 4.14 ptrace), desktop spawn smoke.
