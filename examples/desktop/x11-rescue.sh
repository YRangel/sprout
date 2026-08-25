#!/data/data/com.termux/files/usr/bin/sh
# x11-rescue.sh — kill zombie Lorie X servers by PID, wipe lock/socket debris,
# start ONE fresh server on the requested display, verify, done.
# Usage: x11-rescue.sh [display-number]   (default 0)
D="${1:-0}"
P=/data/data/com.termux/files/usr

echo "== existing termux-x11 processes:"
ps -eo uid,pid,args 2>/dev/null | grep "[t]ermux-x11 com.termux.x11" | while read -r uid pid rest; do
    echo "  killing uid=$uid pid=$pid ($rest)"
    kill -9 "$pid" 2>&1 || echo "  kill failed for $pid"
done
sleep 2

rm -f "$P/tmp/.X${D}-lock" "$P/tmp/.X11-unix/X${D}" "$P/tmp/.X11-unix/X0" "$P/tmp/.X0-lock" "$P/tmp/.X11-unix/X1" "$P/tmp/.X1-lock"

nohup "$P/bin/termux-x11" ":$D" >"$P/tmp/x11drv-${D}.log" 2>&1 &
sleep 5

if [ ! -S "$P/tmp/.X11-unix/X${D}" ]; then
    echo "FAIL: no socket after 5s — driver log:"
    tail -8 "$P/tmp/x11drv-${D}.log"
    exit 1
fi
echo "== socket up: X${D}"

# handshake inside the guest
DROOT_FS1=/data/data/com.termux/files/home/roots/debian
DROOT_FS2=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/ubuntu/rootfs
if [ -d "$DROOT_FS1" ]; then U=$DROOT_FS1; else U=$DROOT_FS2; fi
export SPROUT_PRELOAD_PATH=$P/bin/libsprout-core.so SPROOT_PTRACE_PATH=$P/bin/sprout-super
export DISPLAY=:$D
if [ -x "$U/root/xqx" ]; then
    OUT=$(timeout 30 sprout -r "$U" --shared-tmp --user=0:0 -- /root/xqx 2>/dev/null | head -1)
else
    OUT=$(timeout 30 sprout -r "$U" --shared-tmp --user=0:0 -- sh -c "export DISPLAY=:$D; xwininfo -root" 2>/dev/null | grep -avE '^\[|^$' | head -1)
fi
echo "== handshake: $OUT"
case "$OUT" in
    *display=0x*|*present=1*|*Dimensions:*|*"Window id"*) echo "== X  PLANE  OK  on :$D" ;;
    *) echo "FAIL handshake"; exit 1 ;;
esac
