# From zero to a desktop: step by step

Every step below says what it does, why it exists, and the one thing to
check before moving on. Copy one block at a time. If the check fails, stop
— the fix is right there under **If this went wrong**.

Goal at the end of this page: a full XFCE desktop running inside sprout,
in the Termux:X11 Android app, with sound.

---

## Step 0 — install the Termux:X11 half (no guests yet)

A desktop needs an X server. On Android that means **two** components
that must come from the *same store family* (both F-Droid, or both
GitHub releases — signatures must match):

```sh
pkg update && pkg upgrade -y
pkg install x11-repo -y
pkg install termux-x11-nightly -y
```

Then install the **Termux:X11 Android app** (the `.apk` from
https://github.com/termux/termux-x11/releases — file ends in
`termux-x11-nightly-*-universal.apk`, or from F-Droid). Open the app once
so Android registers it.

✅ `which termux-x11` prints a path; the Termux:X11 app icon is on your
launcher.

> **⚠ Mixing F-Droid Termux + GitHub APK (or the reverse) never works.**
> Different signing keys → the app and the shell can't see each other's
> sockets. If Termux came from F-Droid, get the APK from F-Droid; if it
> came from GitHub, get the APK from the same releases page.

---

## Step 1 — install sprout

```sh
pkg install rust git -y
git clone https://github.com/YRangel/sprout.git && cd sprout
cargo build --release --workspace
bash install.sh --verify
```

✅ `sprout --version` prints `sprout 0.5.1` (or newer).

**If this went wrong:** `install.sh` downloads two prebuilt `.so` files
from the GitHub release — your phone can't compile them (they're glibc
ABI). A network hiccup fails here loudly. Rerun until `--verify` passes.

---

## Step 2 — get a Linux rootfs

A rootfs is a directory full of distro files. Make one and fill it:

```sh
mkdir -p ~/linux
cd ~
curl -LO https://github.com/termux/proot-distro/releases/download/v4.37.0/debian-trixie-aarch64-pd-v4.37.0.tar.xz
sprout upkg debian-trixie-aarch64-pd-v4.37.0.tar.xz -C ~/linux
```

`sprout upkg` extracts the tarball with SELinux-aware rules (hardlinks
become content-copies, device nodes skipped). Other distros work too —
any `rootfs-<distro>-aarch64.tar.xz|gz|bz2` URL from a reputable source.

✅ Check both exist:

```sh
ls ~/linux/bin/ls
ls ~/linux/etc/os-release
```

**If this went wrong:**
- `sprout: command not found` → go back to Step 1.
- `guest loader not found` → your tarball is for the wrong CPU arch
  (x86_64 instead of arm64). Get the `aarch64` build.
- `No such file or directory` on the tarball path → you `cd`'d away.
  `cd ~` and re-run.

---

## Step 3 — boot a shell and make DNS work inside

```sh
sprout -r ~/linux --user=0:0 -- /bin/bash
```

✅ The prompt changes (you're *inside* the guest now). Try:

```sh
cat /etc/os-release | head -1
ping -c 1 8.8.8.8             # raw IP — works without DNS
ping -c 1 deb.debian.org      # needs DNS — fails right now, expected
```

The last one fails because Android's resolver lives on the *host*
loopback. Inside the guest, `127.0.0.1` is the guest's own empty
loopback.

**Fix DNS, from plain Termux** (exit the guest shell first with `exit`):

```sh
rm -f ~/linux/etc/resolv.conf
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > ~/linux/etc/resolv.conf
```

✅ Re-enter and `ping -c 1 deb.debian.org` resolves.

---

## Step 4 — install XFCE + dbus inside the guest

```sh
sprout -r ~/linux --user=0:0 -- bash
apt update
apt install -y xfce4 xfce4-goodies dbus-x11 x11-utils
exit
```

This takes 5–15 min. `dbus-x11` and `x11-utils` are not optional —
dbus-x11 provides the session bus launcher xfce depends on, and
`x11-utils` gives you `xset q` / `xwininfo` for handshake probes later.

✅ apt ends without red `E:` lines.

**If this went wrong:**
- `dpkg: ... returned an error exit status 2` mentioning a
  `/data/data/com.termux/...` path → a postinstall script used
  `realpath()` which returns the host path. Workaround inside the guest:

  ```sh
  dpkg --configure -a
  apt --fix-broken install
  # if one specific package keeps failing (example: dictionaries-common):
  rm -f /var/lib/dpkg/info/dictionaries-common.postinst
  printf '#!/bin/sh\nexit 0\n' > /var/lib/dpkg/info/dictionaries-common.postinst
  chmod 755 /var/lib/dpkg/info/dictionaries-common.postinst
  dpkg --configure -a && apt-get -f install
  ```

---

## Step 5 — the four background jobs a desktop needs

No systemd runs inside sprout (rootless = no PID 1 duties). Android apps
can't start daemons either. So a working desktop needs **four separate
processes alive**, in this dependency order:

```
┌─────────────────────┐
│  pulseaudio         │  host daemon — sounds via Android audio HAL
│  (Termux side)      │  listens on TCP 127.0.0.1 + a unix socket
└─────────┬───────────┘
          │ (independent)
┌─────────▼───────────┐
│  Termux:X11 :0      │  host daemon — the actual X server (the APK)
│  (Termux:API side)  │  opens /data/data/com.termux/files/usr/tmp/.X11-unix/X0
└─────────┬───────────┘
          │ (xfce's session bus must find a live X display)
┌─────────▼───────────┐
│  dbus session bus   │  guest process — apps discover each other here
│  (inside sprout)    │  launched by startxfce4's chain via dbus-x11
└─────────┬───────────┘
          │
┌─────────▼───────────┐
│  xfce4-session      │  guest process — the actual desktop
│  (inside sprout)    │  exits when you log out or close the X app
└─────────────────────┘
```

If any layer is missing, the layers above it crash with cryptic errors
(the exact failure cascade is in the troubleshooting section below).
That's why the launcher script exists at all — to bring each layer up in
the correct order, every time.

In this step you'll bring each one up **by hand**, understand what it
does, and only at the end use the one-line script.

---

## Step 6 — launch the desktop MANUALLY (no harness)

Do this once so you understand every piece. All of these run in your
Termux session (not inside sprout).

### 6a — PulseAudio (host side)

```sh
pulseaudio --kill 2>/dev/null; sleep 1
pulseaudio --start --exit-idle-time=-1
```

What this does: starts the sound daemon. `--exit-idle-time=-1` tells it
**never to quit on silence** — otherwise it exits after a minute of no
audio and your desktop goes mute.

✅ `pactl info | grep 'Default Sink'` prints a line.

**If this went wrong:** `pulseaudio --start` says failed. Likely a stale
pid file: `rm -f $PREFIX/var/run/pulse/pid` and retry. Still failing →
your audio will be absent but the desktop still runs (skip ahead).

### 6b — Termux:X11 server (host side)

```sh
# clean prior attempts: file and process leavings fool the next run
pkill -f 'termux-x11 :0' 2>/dev/null
rm -f $PREFIX/tmp/.X0-lock $PREFIX/tmp/.X11-unix/X0
sleep 2

# launch it for real
nohup termux-x11 :0 > $PREFIX/tmp/x11.log 2>&1 &
```

What this does: starts the X server with display `:0`. The server creates
the unix socket `$PREFIX/tmp/.X11-unix/X0` — that's the rendezvous point
the guest will connect to.

✅ After ~5 s, `ls -la $PREFIX/tmp/.X11-unix/X0` shows an `s` (socket)
type file.

**If this went wrong:** `FAIL: no socket after 5s` (look in
`$PREFIX/tmp/x11.log`). The Termux:X11 Android app isn't installed,
isn't running, or is signature-mismatched (Step 0 ⚠).

### 6c — Bring the app window forward

```sh
am start -n com.termux.x11/.MainActivity
```

This asks Android to bring the Termux:X11 app's window to the front.
You should now see a black screen or a waiting-for-client canvas in the
app.

### 6d — Give the guest its runtime dir + environment

Still in Termux:

```sh
sprout -r ~/linux --user=0:0 -- mkdir -p /tmp/runtime-root
chmod 700 ~/linux/tmp/runtime-root
```

`XDG_RUNTIME_DIR` default for uid 0 is `/tmp/runtime-root` and xfce's
session bus refuses to start without it (mode must be exactly 0700).

### 6e — Launch startxfce4 inside sprout

```sh
sprout -r ~/linux --shared-tmp --termux-x11 --user=0:0 -- startxfce4
```

What every flag does:

| Flag | Why the desktop needs it |
|---|---|
| `-r ~/linux` | which rootfs to enter |
| `--shared-tmp` | binds host `$PREFIX/tmp` into guest `/tmp` — the X socket file (created in 6b) appears inside the guest at the exact same path |
| `--termux-x11` | exports `DISPLAY=:0` and `PULSE_SERVER=127.0.0.1` into the guest so XFCE knows where to find display + sound |
| `--user=0:0` | fake-root identity inside the guest (dpkg-ish session scripts expect it) |
| `--` | **sprout options stop here** — `startxfce4` is the guest command |
| `startxfce4` | the actual xfce session; in turn launches dbus-launch, then xfce4-session, then the panel/desktop/WM |

✅ Termux:X11's window shows the XFCE desktop, mouse + keyboard work.

### Closing the session manually

- Log out from XFCE's menu, or close the Termux:X11 app.
- Back in Termux: `pkill -f startxfce4; pkill -f 'termux-x11 :0'` (and
  pulseaudio keeps running — that's fine, or `pulseaudio --kill`).

**If this step failed**, read the raw log the launch produced:

```sh
sprout -r ~/linux --shared-tmp --termux-x11 --user=0:0 -- startxfce4 2>&1 | head -20
```

Match the first failure line against the table at the bottom of this page.

---

## Step 7 — the shortcut: one script that does 6a→6e for you

You've now run each piece by hand and know what they do. For daily use,
the same five steps are bundled in one script that also adds health
checks and a stale-state sweeper (it auto-deletes that dead
`/tmp/.X11-unix/X0` file before launching, which is the most common
"desktop won't start" cause):

```sh
mkdir -p ~/bin
cp ~/sprout/examples/desktop/start-desktop.sh \
   ~/sprout/examples/desktop/pulse-guard.sh \
   ~/sprout/examples/desktop/x11-rescue.sh ~/bin/
chmod +x ~/bin/start-desktop.sh ~/bin/pulse-guard.sh ~/bin/x11-rescue.sh
```

Tell it which rootfs is yours:

```sh
export SPROUT_DESKTOP_ROOTFS=~/linux
```

To make that permanent across Termux restarts:

```sh
echo 'export SPROUT_DESKTOP_ROOTFS=~/linux' >> ~/.bashrc
```

### Daily commands

```sh
~/bin/start-desktop.sh start     # full sequence (pulse-guard + x11-rescue + session)
~/bin/start-desktop.sh stop      # clean stop
~/bin/start-desktop.sh restart   # bounce everything (use after resume from deep sleep)
~/bin/start-desktop.sh status    # what's alive right now
~/bin/start-desktop.sh shell     # extra shell INTO the running guest
```

### What `start` does, exactly

1. **Audio gate** — is `pactl info` healthy? If not, `pulseaudio
   --kill`, then start. If it still fails: warns and continues (no
   audio, but desktop still works). Then it spawns a tiny watchdog
   (`pulse-guard.sh`) that pings pulse every 20 s and cold-restarts it
   on silence — HyperOS-class kernels occasionally kill idle daemons.
2. **X gate** (`x11-rescue.sh`) — finds every leftover `termux-x11`
   process and numeric-kills it, removes the stale socket file,
   launches a fresh server, waits until an actual handshake probe
   (`xwininfo -root` via sprout) returns, and only then proceeds.
3. **Session launch** — `setsid sprout -r $SPROUT_DESKTOP_ROOTFS
   --shared-tmp --termux-x11 --user=0:0 -- /usr/bin/startxfce4` in the
   background. Then it polls `ps` every 5 s for `xfce4-session` and
   reports success/failure after up to 24 tries.

So `start` is **Step 6 with code around it**. You could do everything it
does manually (you just did, in Step 6); the script exists so you don't
have to.

---

## Common failure map (from real sessions)

| Error you saw | What it really means | Fix → step |
|---|---|---|
| `nohup: failed to run command '…/termux-x11': No such file or directory` | the pkg isn't installed | Step 0 |
| `== socket up: X0` then `FAIL handshake` | APK not running / signature mismatch | Step 0 ⚠ |
| `X server already running on display :0` then a wall of `Can't open display` | stale `/tmp/.X11-unix/X0` file | `rm -f $PREFIX/tmp/.X11-unix/X0` and relaunch (script does this) |
| `XDG_RUNTIME_DIR "/tmp/runtime-root" not available` | Step 6d never ran | redo Step 6d |
| `dbus-launch: ... failed to connect to X11 display` | startxfce4 ran but no live X server | 6b's handshake must pass first |
| `Unable to locate package neofetch` | DNS still broken | Step 3 |
| dpkg postinst errors with `/data/data/com.termux/...` paths | postinst saw the host path via `realpath` | Step 4's workaround |
| `pulseaudio --start failed` | stale pulse pid | Step 6a's workaround |

---

## Where next

- [sprout, by hand](./commands.md) — what every flag actually does
- [Troubleshooting](./troubleshooting.md) — the long-form symptom index
- [X11 / GPU guide](./x11-gpu.md) — Turnip-accelerated workloads, how
  `--shared-tmp` + ICD plumbing are wired
