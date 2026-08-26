# Troubleshooting by symptom

Search this page (Ctrl-F) for the exact string from your terminal. Each
section has the same shape: **what it means** → **why it happens** →
**the fix**.

---

## Startup / CLI

### `sprout: guest loader not found`

```
sprout: guest loader not found: tried ["/lib/ld-linux-aarch64.so.1", ...] inside rootfs
```

**Means:** sprout looked inside the rootfs you picked and found no dynamic
loader, so no glibc binary can run.

**Why:** wrong rootfs, rootfs not unpacked yet, or the rootfs was built for
x86_64 while you're on aarch64.

**Fix:**
```sh
ls -la <your-rootfs>/lib/ld-linux-aarch64.so.1
# if missing: unpack correctly
sprout upkg rootfs.tar.xz -C <your-rootfs>
# or: point -r at the actual rootfs directory (the one containing bin/ etc/)
```

### `sprout: command not found`

You installed the source tree but never ran `install.sh`. Either:
- `cd ~/sprout && bash install.sh --verify`, or
- Use the absolute path: `~/sprout/target/release/sprout ...`

### `sprout: No such file or directory (os error 2)`

Most often: a `-b` or `--rootfs` path is missing on the host. Re-check
every path with `ls -la` before the guest call. `sprout --dry-run` shows
the resolved plan.

### `sprout --definitely-not-a-flag` doesn't error

It should. If unknown flags are accepted silently you're running an old
sprout (<0.5.1). Rebuild or reinstall; upgrade-path doc lives in
[installation.md](./installation.md).

---

## Network / DNS

### apt: `Unable to locate package ...` / `Temporary failure resolving ...`

**Means:** guest can't resolve DNS names.

**Why:** Android's DNS resolver is host-loopback only (127.0.0.1:53);
inside the guest the same address is the guest's own loopback with nothing
listening.

**Fix** (from Termux, replace `~/deltahalo` with your rootfs dir):
```sh
rm -f ~/deltahalo/etc/resolv.conf
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > ~/deltahalo/etc/resolv.conf
```

Check: `sprout -r ~/deltahalo --user=0:0 -- ping -c 1 deb.debian.org`
shows an IPv4/IPv6 address.

### `apt update` fails even after resolv.conf

Network is fine (can ping IPs) but apt transport fails: new Termux ships
an openssl that wants kernel entropy features not present. Workaround:

```sh
# inside guest
apt -o Acquire::https::No-Cache=True update
# or fall back to HTTP mirrors temporarily
```

---

## dpkg / apt package operations

### `dpkg: ... subprocess returned error exit status 2`

Host-path leaking into a postinst (e.g. `/data/data/com.termux/...`
appearing in guest error output).

**Why this happens:** a small number of postinsts ask for their own
realpath (`readlink -f $0` or `realpath()` on argv[0]) and sprout returns
the host-correct answer. The postinst then feeds that path to guest-side
`open()`, which can't read it (not a guest path at all).

**Fix** (the one your friend's case used):
```sh
# inside the guest
dpkg --configure -a
apt --fix-broken install
# if a specific package refuses, stub that one postinst:
rm -f /var/lib/dpkg/info/PKG.postinst
printf '#!/bin/sh\nexit 0\n' > /var/lib/dpkg/info/PKG.postinst
chmod 755 /var/lib/dpkg/info/PKG.postinst
dpkg --configure -a && apt-get -f install
# optional once everything is green:
apt install --reinstall PKG
```

Most postinsts don't do realpath games. The few that do include
dictionaries-common, the gdk-pixbuf loaders, some gstreamer metadata
registrations.

### dpkg: `.../usr/bin/dpkg returned an error code (1)`

Follows the previous block; one failing package blocks every dependent
package from configuring. Same fix.

---

## X11 / desktop

### `FAIL: no socket after 5s — nohup: failed to run command '.../termux-x11'`

You don't have the `termux-x11-nightly` package installed.

**Fix:**
```sh
pkg install x11-repo -y
pkg install termux-x11-nightly -y
```

(and the companion Android app from F-Droid or GitHub releases —
that's the actual X server. The pkg side alone is only a launcher.)

### `FAIL handshake` after the pkg is installed

The termux-x11 **APK** isn't running:
- Termux from F-Droid + APK from Play Store (or vice versa) — signatures
  don't match, silent block. Install both from the same store family.
- Or APK simply hasn't been opened once since install. Open it once,
  then rerun your launcher.

### Firefox-style banner: "X server already running on display :0" then `xrdb: Can't open display`

Stale socket file.

`/tmp/.X11-unix/X0` is a file that gets left behind when you kill-9'd a
previous run. startxfce4 sees the file and mistakenly concludes the server
is alive.

**Fix:**
```sh
rm -f $PREFIX/tmp/.X11-unix/X0
~/start-desktop.sh start
```

(This is exactly what `examples/desktop/x11-rescue.sh` does on every run —
calling raw `sprout -r ... startxfce4` skips the sweep.)

### `xfce4-session: Cannot open display:` / `dbus-launch ... failed to connect to X11`

The X server never came up — you either launched without `termux-x11`
running, or your display env doesn't match your launcher's expectation.

Sprout's rule: sprout never invents DISPLAY. You opt in with
`--termux-x11` (sets `DISPLAY=:0`) or by passing it
explicitly: `env DISPLAY=:0 sprout ...`.

### `XDG_RUNTIME_DIR "/tmp/runtime-root" not available`

The dir doesn't exist inside the guest. xfce-session dbus fails without
it.

**Fix once**, from host-side Termux:
```sh
sprout -r ~/deltahalo --user=0:0 -- mkdir -p /tmp/runtime-root
chmod 700 ~/deltahalo/tmp/runtime-root
```

### `pulseaudio --start failed` at desktop launch

Non-fatal; audio just won't work. Likely stale pid file. Workaround
once:
```sh
rm -f $PREFIX/var/run/pulse/pid
pulseaudio --start --exit-idle-time=-1
```

---

## Permissions / file behavior

### Guest files created at mode `0600` everywhere when apt says they should be `0644`

Termux apps run with umask 077. sprout's CLI applies a default umask of
022 (unless `SPROUT_KEEP_UMASK=1`) before exec'ing the guest, so this
shouldn't happen in ≥0.5 versions. If it does, you're either setting a
custom umask your shell, or running an old sprout.

### `chown` from guest side reports EPERM

Ownership mapping is fake-id. `chown` of a file OUTSIDE the sprout-managed
fake-root still touches kernel-truth ownership — denied. If you must
"change" ownership to another guest-side user, use `--user` to impersonate
instead. Never use real root (sprout is rootless).

### `grep: /etc/passwd.lock: File exists` during `useradd`-class commands

The link2symlink fallback produced `nlink=1` behavior somewhere — the
fake-lock protocol demands nlink=2. This regression was fixed in v0.5.
If it returns on your build, paste your `sprout -v 3` log around the
`linkat()` call chain into a GitHub bug.

---

## Emulation / x86

### `wrong ELF class: ELFCLASS64` when launching an i386 binary

box64 handles x86_64; box32 handles i386. In sprout ≥0.5 i386 is auto-
wrapped via the two-env-row convention (`SPROUT_BINFMT_X86_64` +
`SPROUT_BINFMT_I386`). Check both are exported into the guest session.

### x86_64 binary dies at `set_robust_list` / `rseq` log lines

Android's seccomp filter blocks `set_robust_list` (99) and `rseq` (293) for
these app uids. sprout's preload masks them; the ptrace lane also
translates the attempts (see ADR-0006 for the kernel-block table). If
you see the raw error:
- preload lane → check `LD_PRELOAD=$PREFIX/bin/libsprout-core.so` is in
  the child's env (use `sprout --dry-run ...` to inspect)
- ptrace lane → needs a current sprout-super (rebuild + reinstall)

### box64 launches but segfaults inside a specific emulator

That's box64-the-emulator, not sprout-the-loader. Check
`BOX64_LOG=1 sprout -r ... -- box64 ./yourprog` for the JIT trace. Guest
state goes in GitHub's `ptitSeb/box64` issue tracker, not sprout's.

---

## Slow / performance

### sprout feels slow on tiny commands like `true` or `echo hello`

Startup-dominated path. sprout parses CLI, reads ELF headers, plans,
then spawns. proot's launcher is lighter in C terms, so sub-100ms
commands show ~5ms cost. This is inherent and amortized away on any
meaningful workload.

Benchmarks: `bench/run.sh` and `bench/run-statics.sh`. The controlled
variant of both sprout-notify-vs-proot vs sprout-ptrace-vs-proot is
explained in `docs/src/benchmarks.md`.

### `dpkg-postinst: glibc-locale-archive-builder takes forever`

Long-classic on Termux. The `.l2s` hash + loader dirs are the right thing
to check: ensure `SPROUT_CACHE_DIR` is not on FAT32. sprout does its own
guest-glibc sanitization in `~/.cache/sprout` and should fall there
automatically — if it lands on SD card (exfat) you will feel a 50×
slowdown.

---

## Still stuck?

1. `sprout --dry-run -v 3 ... ` shows the whole launch composition
2. `bash tests/smoke.sh <rootfs>` and `bash tests/proot-compat.sh <rootfs>`
   (in the repo) exercise the gates expected to pass
3. Check the FAQ and the ADR index for things that don't match this page
4. Open an issue: https://github.com/YRangel/sprout/issues — include the
   command, the exact error text, and your sprout version
   (`sprout --version`)
