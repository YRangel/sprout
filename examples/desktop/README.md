# sprout desktop example (xfce4 on Termux:X11)

Reference orchestrator for a full X11 desktop: pulse gating, stale-X rescue,
session launch, status. Ships three cooperating scripts that live together.

## Files

- `start-desktop.sh` — the entry point (`start|stop|status|restart|shell`).
- `x11-rescue.sh` — kills zombie Termux-X11 (lorie) servers, wipes lock +
  socket debris, starts one fresh server, verifies the handshake.
- `pulse-guard.sh` — watchdog for the HyperOS pulseaudio half-dead state:
  probes every 20s, numeric-kills and cold-restarts on silence.

## Use

```sh
# from anywhere; the script finds its helpers next to itself
./start-desktop.sh start
./start-desktop.sh status
./start-desktop.sh stop
```

Point it at a different guest rootfs without editing:

```sh
SPROUT_DESKTOP_ROOTFS=~/roots/alpine ./start-desktop.sh start
```

## Notes

- Requires `sprout` on PATH + an xfce4 (or equivalent) guest rootfs.
- Hard requirement: Termux:X11 app installed on the host (`am start` forward
  happens automatically on `start`).
- This is a *reference*, not a supported product surface. Real-world
  session state (gdk-pixbuf loader glibc chains, env-wipes, exec caches)
  is what shaped several preload fixes — read `git log` next to these files.
