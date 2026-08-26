# sprout desktop example (xfce4 on Termux:X11)

Reference orchestrator that brings up a full X11 desktop on Termux. Three
scripts, one job: get you from "fresh Termux" to "running XFCE" without
having to remember the flag soup.

## ⚠ BEFORE you run start

The script cannot install its own prerequisites. Do these **once**, in order,
before `start`:

```sh
# 1. Termux packages
pkg update && pkg upgrade -y
pkg install x11-repo -y
pkg install termux-x11-nightly -y

# 2. THE COMPANION APP — the pkg is only a launcher, the actual X server
#    is an Android app. Install termux-x11 APK from F-Droid or
#    https://github.com/termux/termux-x11/releases, then OPEN it once.
#    ⚠ APK must come from the same store family as Termux itself
#    (F-Droid+F-Droid or Play+Play). Mixing them gives silent
#    "socket not found" failures later.

# 3. A guest rootfs with XFCE inside it. From the sprout guide:
mkdir -p ~/myrootfs
curl -LO <any aarch64 rootfs.tar.xz URL>
sprout upkg rootfs.tar.xz -C ~/myrootfs
sprout -r ~/myrootfs --user=0:0 -- bash   # inside the guest:
apt update && apt install -y xfce4 xfce4-goodies dbus-x11
exit                                       # back to Termux

# 4. One guest-side runtime dir (skip this and xfce errors with
#    'XDG_RUNTIME_DIR "/tmp/runtime-root" not available')
sprout -r ~/myrootfs --user=0:0 -- mkdir -p /tmp/runtime-root
chmod 700 ~/myrootfs/tmp/runtime-root

# 5. Tell the harness where your rootfs lives
export SPROUT_DESKTOP_ROOTFS=~/myrootfs
```

When all five done, read on.

## Files

| Script | Purpose |
|---|---|
| `start-desktop.sh` | entry point (`start` / `stop` / `status` / `restart` / `shell`) |
| `x11-rescue.sh` | kills zombie Termux:X11, removes stale `/tmp/.X11-unix/X0`, launches a fresh server, waits for handshake |
| `pulse-guard.sh` | keeps pulseaudio running in background; every 20s probes and cold-restarts on silence |

Copy all three into the SAME directory (anywhere) and `chmod +x` them.
`start-desktop.sh` finds the other two relative to itself.

## Use

```sh
./start-desktop.sh start     # bring everything up
./start-desktop.sh status    # what's running right now
./start-desktop.sh stop      # cleanly stop all
./start-desktop.sh restart   # bounce X + audio + session
./start-desktop.sh shell     # root shell INSIDE the guest
```

## What `start` actually does, step by step

1. **pulse-guard** starts in the background. If it prints
   `WARN: pulseaudio --start failed`, audio is broken but video still works.
2. **x11-rescue** kills any zombie termux-x11 process, *deletes the stale
   `/tmp/.X11-unix/X0` file* (this is the thing that makes a raw
   `startxfce4` say "X server already running" — the file exists but
   nothing is listening), starts a fresh server, and waits until the
   handshake actually answers.
3. **sprout session**: spawns `xfce4-session` inside your rootfs with
   `--shared-tmp --termux-x11 --user=0:0`.

## When things fail

| What you see | What it means | Fix |
|---|---|---|
| `FAIL: no socket after 5s — nohup: failed to run command '.../termux-x11'` | `termux-x11-nightly` pkg missing | prerequisite #1 |
| `FAIL handshake` | APK not running, or APK and Termux installed from different stores | prerequisite #2 |
| `X server already running on display :0` then cascade of display errors | stale socket file | `rm -f $PREFIX/tmp/.X11-unix/X0` and rerun `start` |
| `XDG_RUNTIME_DIR "/tmp/runtime-root" not available` | prerequisite #4 skipped | run the mkdir again |
| guest errors referencing `/data/data/com.termux/...` paths | some postinst script called `realpath` and saw the host path; known shim gap | see `docs/src/guide/getting-started.md` Step 5 workaround |

## Notes

- Requires `sprout` on PATH + an xfce4 (or similar) guest rootfs.
- Hard requirement: the Termux:X11 **Android app** (not just the pkg) must be
  installed and runnable.
- This is a **reference**, not a supported product surface. Real-world session
  state (gdk-pixbuf loader glibc chains, env-wipes, exec caches) shaped several
  preload fixes — read `git log` next to these files for context.
