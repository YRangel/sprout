# Quickstart (Termux, on-device)

## Install

```
pkg install rust git
cd sprout && cargo build --release --workspace
bash install.sh                    # $PREFIX/bin: sprout + interposer + supervisor
```

## Run

```
B=$PREFIX/var/lib/proot-distro/containers/debian/rootfs   # or any glibc rootfs
A=$PREFIX/var/lib/proot-distro/containers/alpine/rootfs   # or any musl rootfs

sprout -r $B /bin/echo hello
sprout -r $B -w /root /bin/bash                    # interactive shell
sprout -r $B -b $PREFIX/tmp/x:/mnt /bin/cat /mnt/f # binds, proot-style
sprout -r $B --shared-tmp /usr/bin/xterm           # X11 (termux-x11 :0)
sprout -r $A /bin/busybox sh -c 'ls /etc/apk'      # musl automatic (supervisor)
sprout -r $B ./static-binary; echo $?              # exit codes pass through
```

Debug the launch without running: `--dry-run`. Trace: `-v`.
Force supervisor: `--fallback ptrace` (for testing).

## Packages work inside guests

The apt / apk cycle (download, verification via sqv, unpack, post-install
scripts) runs rootless with no extra flags:

```
sprout -r $B /usr/bin/apt-get update
sprout -r $B -0 --link2symlink /usr/bin/apt-get install -y nodejs
sprout -r $A /sbin/apk add nodejs
sprout -r $B /usr/bin/curl -fsSL -o /tmp/x https://example.com/file
```

Verified: cowsay run, node--binaries execute, Go run, static / musl /
Go-static / Go-dynamic binaries, X11 (termux-x11) handshake, cloudflared
static binary executing. Unsupported host GPU nodes/devices stay
SELinux-blocked, same as proot-distro.

Semantics match proot (`-r -b -w -0 --link2symlink`) plus `--shared-tmp`
parity with proot-distro login. See docs/src/guide/.
