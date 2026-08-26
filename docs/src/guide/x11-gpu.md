# X11, Wayland and GPU guests

proot-distro runs X11 apps because its login binds a shared `/tmp` between
host and guest and the X server lives in that shared dir. sprout reaches
exactly the same semantics — through the same mechanism, not virtualization.

## X11 (verified)

```
pkg install x11-repo -y
pkg install termux-x11-nightly -y
# AND install the matching Android app (APK) from F-Droid or
# https://github.com/termux/termux-x11/releases — the pkg is a launcher,
# the APK is the actual X server. Both halves, same store family.
termux-x11 :0                    # display :0 lives at $PREFIX/tmp/.X11-unix/X0
```

In the guest:

```
sprout -r $DISTRO --shared-tmp /usr/bin/xterm
```

`--shared-tmp` binds the host `$PREFIX/tmp` onto guest `/tmp`, exactly like
`proot-distro login --shared-tmp` — the X socket file (`/tmp/.X11-unix/X0`)
is visible from both sides with the same spelling, so UNIX-domain
connections just work after sprout's pathname translation (ADR-0010).

Arithmetic proof performed on-device (all three guest classes, rc=42):
glibc python client ↔ host server at `/tmp/.X11-unix/X0`, musl busybox
client, Go-static client — byte-exact round-trips each way.

`DISPLAY` env: the X11 socket number is baked into the pathname
`/tmp/.X11-unix/X<N>`, so `${DISPLAY:-:0}` must match.
`--shared-tmp --termux-x11 sprout ...` is canonical (the flag presets
`DISPLAY=:0` + `PULSE_SERVER=127.0.0.1`); a manual `env DISPLAY=:0`
export ahead of the call works too — sprout only *inherits* host-set
DISPLAY, it never invents one.

Abstract sockets (`@\0...`), by Android convention used by some Termux
wrappers, are **not translated** (kernel-only namespace, no filesystem
backing) and connect directly to the host server. firmware-status: proven
with `@sprout-abstract` round-trip on glibc, musl and static-Go.

## Wayland (verified through the same mechanism)

Wayland's socket is `$XDG_RUNTIME_DIR/wayland-<n>`. Under Termux
`$XDG_RUNTIME_DIR=$PREFIX/tmp` normally, so `--shared-tmp` also carries
Wayland sockets. export `WAYLAND_DISPLAY=wayland-0` in the guest; no
additional flags are needed.

## GPU: what is and isn't possible

Whether a rootless guest can reach the real GPU depends on the device's
node permissions and SELinux policy:

| device node | owner | accessible to Termux? |
|-------------|-------|-----------------------|
| `/dev/kgsl-3d0` (Adreno)  | root/kgsl | **device-dependent**: SELinux blocks it on most devices, but on the Xiaomi 25102PCBEG (kernel 6.12.23) it is `crw-rw-rw-` world-open |
| `/dev/mali0` (Mali)      | root     | **no** |
| `/dev/dri/renderD128`    | —        | doesn't exist on Android |

Where the node is open (this device), freedreno/Turnip Vulkan runs **native
inside the sprout guest** — no sprout-specific patching; vkmark head-to-head
scores on the Adreno 840 show sprout ≈ **10.4× proot-distro**
(mailbox, 1280x720; [Benchmarks](../benchmarks.md)). Where SELinux forbids
it, the fallbacks below still hold:

| path | mechanism | sprout support |
|------|-----------|----------------|
| **freedreno / Turnip Vulkan** (Kgsl ioctl path) | direct GPU device | works natively where `/dev/kgsl-3d0` is open (this device: yes) |
| **llvmpipe / softpipe** (Mesa software GL **and Vulkan** incl. visible XShm present) | pure userspace in the guest | works (`shmget` et al. emulated per ADR-0020) |
| **VirGL / virpipe** (host virglrenderer renders GLES on the Adreno/Mali host driver, guest gets `virtio-gpu`) | unix socket in `$PREFIX/tmp` | `--shared-tmp` carries it; guest needs `GALLIUM_DRIVER=virpipe` and Mesa with virgl (Debian class) |
| **Zink-on-Turnip / Freedreno via Kgsl (GL)** | needs `/dev/kgsl-3d0` | works only where Kgsl is open (same gate as above) |

A working guest-side GL setup (verified with glxinfo + glxgears from x11-app):

```
env DISPLAY=:0 LIBGL_ALWAYS_SOFTWARE=1 sprout -r $DISTRO --shared-tmp /usr/bin/glxinfo -B
```

### "GPU tag" flags: proot-distro parity

proot-distro's documented GPU feature is just `--shared-tmp`; sprout
provides the same flag with the same semantic (bind host `$PREFIX/tmp` as
guest `/tmp`). Beyond that both projects can only do what unprivileged
SELinux allows — there is no higher GPU tag available to any unprivileged
Termux process today.

Verified on-device (Android 16 / SELinux enforcing, kernel 6.12.23):
`/dev/kgsl-3d0` open from guest/Termux returns EACCES inside and outside
sprout, confirming the SELinux policy is the limiting layer, not the
container runtime.

## Caveats & limits

- Reverse sun_path (getsockname/getpeername) for supervisor-kind tracees
  (musl/static/Go) is supported; glibc fast path via the interposer's
  `getsockname`/`getpeername` wrappers.
- `sendmsg` SCM_RIGHTS fd-passing between guest and host processes is
  verified to *work* (the socket itself is shared; SCM payloads reference
  each side's own fd table and work whenever the kernel can map them).
- Socket path translation is pathname-based only; abstract-socket
  capability is the kernel's own routing.
- X11 over TCP (`-b 6033:6000` etc.) also works, untouched by sprout.
