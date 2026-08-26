# From zero to a desktop: step by step

Literally every step, why it exists, what to check before moving on. Copy each
block one at a time, check the line under it, **then** continue. If a check
fails, STOP — read the "if this went wrong" subsection for that step before
typing anything else.

Reader assumed: fresh Termux, never ran sprout, wants an XFCE desktop.

---

## Step 0 — prerequisites on the Android side (no guests yet)

sprout runs rootless inside your Termux session, but it cannot conjure
an X server out of nothing. You need:

1. **Termux itself** updated:
   ```sh
   pkg update && pkg upgrade -y
   ```
   ✅ You should see `All packages are up to date.`

2. **Termux:X11 packages** — the Termux half:
   ```sh
   pkg install x11-repo -y
   pkg install termux-x11-nightly -y
   ```
   ✅ Check: `which termux-x11` prints a path.

3. **Termux:X11 Android app** — the other half (`termux-x11` in Termux is
   just the launcher; the actual X server is an app):
   - Install it from F-Droid **or** from the GitHub release
     [termux/termux-x11 releases](https://github.com/termux/termux-x11/releases)
     (file name ends in `.apk`, e.g. `termux-x11-nightly-…-universal.apk`),
     then open the app once so Android registers it.
   - **⚠ Install from the SAME source family as Termux itself.** Mixed
     F-Droid-Termux + Play-Store-termux-x11 (or vice versa) gives silent
     `socket: No such file` errors — the two apps signed by different keys
     can't see each other's data.

   ✅ Check: you should have an app called "Termux:X11" on your launcher
   with a green X icon.

---

## Step 1 — install sprout

```sh
pkg install rust git -y
git clone https://github.com/YRangel/sprout.git && cd sprout
cargo build --release --workspace
bash install.sh --verify
```
✅ Check: `sprout --version` prints something like `sprout 0.5.1`.

**If this went wrong:** `install.sh` downloads the two prebuilt interposer
`.so` files from GitHub (your phone cannot build them, ARM guest ABI).
If download fails, the script says so and exits non-zero — check your
connection and re-run. Do NOT continue past this error.

---

## Step 2 — get a Linux rootfs onto disk

A rootfs is just a directory full of Debian/Alpine/Ubuntu files. sprout
ships a proot-compatible downloader equivalent called `upkg`.

Two minutes, glibc Debian example:

```sh
mkdir -p ~/deltahalo
cd ~
curl -LO https://github.com/termux/proot-distro/releases/download/v4.37.0/debian-trixie-aarch64-pd-v4.37.0.tar.xz
sprout upkg debian-trixie-aarch64-pd-v4.37.0.tar.xz -C ~/deltahalo
```

(The URL is one example; any `rootfs-<distro>-<arch>.tar.{xz,gz,bz2}` works.
`upkg` picks the decompressor automatically from magic bytes.)

✅ Checks:

```sh
ls ~/deltahalo/bin/ls       # exists
ls ~/deltahalo/etc/os-release   # exists
```

**If this went wrong:**
- `sprout upkg: open ...: No such file` → the tarball path is wrong. `cd` and retry.
- `mkdir: cannot create directory … : Permission denied` → you wrote `sudo` or
  ran with `su` somewhere. sprout does NOT want root. Run as plain Termux user.

---

## Step 3 — smoke the rootfs (no graphics yet)

```sh
sprout -r ~/deltahalo --user=0:0 -- /bin/bash -c 'cat /etc/os-release | head -1'
```
✅ Should print something like `PRETTY_NAME="Debian GNU/Linux 13 (trixie)"`.

**If this went wrong:**
- `sprout: guest loader not found: tried […]` → the tarball didn't contain
  the loader (`ld-linux-aarch64.so.1`), usually because you unpacked an x86_64
  rootfs on a phone. `upkg` does not convert architectures. Get an aarch64
  tarball.
- `sprout: command not found` → Step 1 didn't finish. Go back.

---

## Step 4 — DNS inside the guest (before any apt)

Android's resolver lives at `127.0.0.1:53` **on the host**. Your guest's
`127.0.0.1` is its own loopback — nothing there. Machine can't resolve names
until you write real IPs into guest resolv.conf.

From **Termux, one line**:

```sh
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > ~/deltahalo/etc/resolv.conf
```

If `~/deltahalo/etc/resolv.conf` was a dangling symlink (points at
`/run/systemd/resolve/...` that doesn't exist in a container), remove it first:

```sh
rm -f ~/deltahalo/etc/resolv.conf
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > ~/deltahalo/etc/resolv.conf
```

✅ Check: `sprout -r ~/deltahalo --user=0:0 -- ping -c 1 deb.debian.org`
prints a line with an IP in it.

**If this went wrong:** double-check you used a `>` (not `>>` typo, not a
newline-in-echo-escape). `cat ~/deltahalo/etc/resolv.conf` should show exactly
two lines beginning `nameserver`.

---

## Step 5 — install XFCE inside the guest

```sh
sprout -r ~/deltahalo --user=0:0 -- bash
# (prompt changes — you are now INSIDE the guest — note the localhost name)
apt update
apt install -y xfce4 xfce4-goodies dbus-x11
```

This takes 5–15 min depending on network. apt may ask for a keyboard layout
— pick whatever you like.

✅ When apt exits without red "E:" lines you're done.

**If this went wrong:**

- dpkg errors about `/data/data/com.termux/...` paths inside the guest
  (the *host* path leaking in): some postinst scripts use realpath() which
  sees through sprout's translation. Known-shim-gap; workaround that always
  works once the rest of the package set is fine:

  ```sh
  # inside the guest, run these one by one:
  dpkg --configure -a
  apt --fix-broken install
  # if a specific package still refuses, e.g. dictionaries-common:
  rm -f /var/lib/dpkg/info/dictionaries-common.postinst
  printf '#!/bin/sh\nexit 0\n' > /var/lib/dpkg/info/dictionaries-common.postinst
  chmod 755 /var/lib/dpkg/info/dictionaries-common.postinst
  dpkg --configure -a && apt-get -f install
  # optional cleanup once everything is green:
  apt install --reinstall dictionaries-common
  ```

- `Can't open display` errors during install are HARMLESS at this stage:
  there is no X server yet. apt is just autodetecting and warning. Will
  be fixed by Step 6. IGNORE them for now.

Exit the guest shell with `exit` to drop back to Termux when apt is done.

---

## Step 6 — give the guest an X server + audio (one-shot harness)

Sprout cannot guess which of your Termux:X11 instances, on which display,
with which audio daemon, you want. We ship the reference launcher that
does ALL of this in the right order:

```sh
# these three files come with the repo, no install needed beyond cp
cp ~/sprout/examples/desktop/start-desktop.sh  ~/
cp ~/sprout/examples/desktop/pulse-guard.sh    ~/
cp ~/sprout/examples/desktop/x11-rescue.sh     ~/
chmod +x ~/start-desktop.sh ~/pulse-guard.sh ~/x11-rescue.sh

# make it point at YOUR rootfs (any path works; no edits needed inside)
export SPROUT_DESKTOP_ROOTFS=~/deltahalo

# one-time prep INSIDE the guest: xfce expects this runtime dir
sprout -r ~/deltahalo --user=0:0 -- sh -c 'mkdir -p /tmp/runtime-root && chmod 700 /tmp/runtime-root'
```

✅ Both blocks ran without errors.

**If this went wrong**:
- `mkdir: cannot create directory '/tmp/runtime-root': Permission denied` —
  your guest's `/tmp` isn't writable by fake-root. Fix once:
  `rm -rf $PREFIX/tmp/runtime-root` (clean host side too) and re-run.

---

## Step 7 — start the desktop (the only command you need from now on)

```sh
~/start-desktop.sh start
```

What this does for you, in order, and what each check means:

1. `pulse-guard`: keeps pulseaudio alive in the background. If it prints
   `WARN: pulseaudio --start failed`, audio is dead but display still works.
   Not fatal.
2. `x11-rescue`: kills any leftover termux-x11 process, **removes the stale
   `/tmp/.X11-unix/X0` socket file**, launches a fresh server, and waits
   until a real handshake answers on the socket. **If this says "FAIL
   handshake"**, the Termux:X11 Android app is either not installed (Step 0 #3),
   not running, or was signed from a different repo than Termux itself
   (Step 0 #3 ⚠). Fix that FIRST before anything else.
3. Sprout session: spawns `xfce4-session` inside the guest with
   `--shared-tmp --termux-x11 --user=0:0`.

✅ Your Termux:X11 app should pop up on screen with the XFCE desktop inside.

---

## Step 8 — actually using the thing

While the desktop is running:

- `~/start-desktop.sh stop` — cleanly stop everything.
- `~/start-desktop.sh status` — print what's running.
- `~/start-desktop.sh restart` — bounce X+audio+session (use when lockscreen
  confuses the X server).
- `~/start-desktop.sh shell` — give you a shell INTO the desktop's guest
  (apt install more packages, poke around).
- `~/start-desktop.sh` — alone shows this list again.

You do **not** type `sprout -r ~/deltahalo --shared-tmp --termux-x11
startxfce4` directly. Without the preflight steps the launch dies with a
confusing `Cannot open display` error message — the harness exists because
everyone hits it and nobody enjoys debugging dbus error sprawl.

---

## Common failure map

| Error | Real problem | Fix |
|---|---|---|
| `FAIL: no socket after 5s — nohup: failed to run command '.../termux-x11'` | `termux-x11-nightly` pkg not installed | Step 0 #2 |
| `FAIL handshake` | APK not running, or APK+Termux from different stores | Step 0 #3 |
| `X server already running on display :0` then immediate `Can't open display :0` etc | Stale `/tmp/.X11-unix/X0` from a killed run | `rm -f $PREFIX/tmp/.X11-unix/X0` and rerun `~/start-desktop.sh start` |
| `xfce4-session: Cannot open display` | You bypassed the harness; no DISPLAY set, or `:0` never came up | Use `~/start-desktop.sh start`, not raw `sprout ... startxfce4` |
| `dbus-launch ... failed to connect to X11 display` | X socket dead → session bus couldn't set up | same as above; harness handles ordering |
| `XDG_RUNTIME_DIR "/tmp/runtime-root" not available` | Step 6 last command never ran (or guest /tmp wiped) | re-mkdir it: `sprout -r … -- mkdir -p /tmp/runtime-root` then `chmod 700` outside |
| `Unable to locate package neofetch` | DNS still broken | Step 4 (resolv.conf) |
| `dpkg: … dictionaries-common` postinst exit 2 | realpath() leaked host path inside guest | dpkg-postinst fence in Step 5 |
| `unable to connect to D-Bus` inside session | `dbus-x11` pkg missing in the guest | inside guest: `apt install dbus-x11`, restart via the harness |

---

## Where to read more (once it works)

- `../guide/proot-compat.md` — every proot flag and sprout's exact status
- `../guide/environment.md` — env vars that shape behavior
- `../adr/` — design decisions, why things are the way they are
- `../guide/faq.md` — the long-form maintenance questions
