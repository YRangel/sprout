#!/data/data/com.termux/files/usr/bin/sh
# start-desktop.sh — one-command debian/xfce4 desktop on Termux:X11 via sprout.
# Usage: start-desktop.sh [start|stop|status|restart|shell]
#
# Helpers x11-rescue.sh + pulse-guard.sh live next to this script
# (examples/desktop/ in the sprout repo). Rootfs override: SPROUT_DESKTOP_ROOTFS.
set -u
P=/data/data/com.termux/files/usr
D="$(cd "$(dirname "$0")" && pwd)"
U="${SPROUT_DESKTOP_ROOTFS:-/data/data/com.termux/files/home/roots/debian}"
export SPROUT_PRELOAD_PATH=$P/bin/libsprout-core.so
export SPROOT_PTRACE_PATH=$P/bin/sprout-super

say()  { echo ">> $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

xfce_running() { ps -eo args 2>/dev/null | grep -a "xfce4-session" | grep -avc grep >/dev/null 2>&1 && return 0 || return 1; }

audio_gate() {
    say "audio daemon:"
    # fast probe first: a happy daemon answers inside ~2s; a slow/hung one
    # will not hold the gate (desktop audio degrades gracefully to none)
    if pactl info 2>/dev/null | grep -aq "Default Sink"; then
        pactl info 2>/dev/null | grep -a "Default Sink" | tr -d '\t'
        return 0
    fi
    say "  daemon silent — (re)starting"
    pulseaudio --kill 2>/dev/null; sleep 1
    if ! pulseaudio --start 2>/dev/null; then
        say "  WARN: pulseaudio --start failed; continuing WITHOUT audio"
        return 0
    fi
    i=0
    while [ $i -lt 6 ]; do
        i=$((i+1)); sleep 1
        pactl info 2>/dev/null | grep -aq "Default Sink" && break
    done
    SUM=$(pactl info 2>/dev/null | grep -a "Default Sink" | tr -d '\t' || true)
    if [ -z "$SUM" ]; then say "  WARN: pulse still not answering (${i}s); continuing WITHOUT audio"; else echo "$SUM"; fi
    setsid "$D/pulse-guard.sh" </dev/null >/dev/null 2>&1 &
    disown 2>/dev/null || true
}

x_gate() {
    say "X server (kill-stale + fresh + handshake):"
    "$D/x11-rescue.sh" 0 || fail "X plane"
}

launch_desktop() {
    say "xfce4 session:"
    # no guest-side helper script: --user=0:0 already anchors
    # HOME/SHELL/USER/LOGNAME=/root..., --termux-x11 exports DISPLAY=:0 and
    # PULSE_SERVER. startxfce4's own lifetime = the session lifetime (the old
    # desk.sh's `& wait` shape), all session output goes to xfce.log below.
    setsid sprout -r "$U" --shared-tmp --termux-x11 --user=0:0 \
        -- /usr/bin/startxfce4 \
        </dev/null >"$P/tmp/xfce.log" 2>&1 &
    sleep 1
    disown 2>/dev/null || true
    i=0
    while [ $i -lt 24 ]; do
        i=$((i+1)); sleep 5
        if xfce_running; then break; fi
        say "  ...waiting for xfce4-session (${i}/24)"
    done
    xfce_running || { tail -5 "$P/tmp/xfce.log" | cut -c1-120; fail "xfce4-session never appeared"; }
}

app_forward() {
    am start -n com.termux.x11/com.termux.x11.MainActivity >/dev/null 2>&1 \
        && say "Termux:X11 app forward"
}

status_report() {
    N=$(sprout -r "$U" --shared-tmp --user=0:0 -- sh -c \
        'export DISPLAY=:0; xwininfo -root -children 2>/dev/null | grep "  0x" | wc -l' \
        2>/dev/null | grep -avE '^\[' | tail -1)
    say "procs: $(ps -eo args 2>/dev/null | grep -aoE 'usr/bin/[a-zA-Z0-9_.-]+' | sort -u | grep -acE 'xfwm4|xfdesktop|xfce4-panel|xfce4-session')/4 core, $N X windows"
}

pulse_line() { pactl info 2>/dev/null | grep -a "Default Sink" | tr -d '\t' || echo "audio: no answer"; }

cmd="${1:-start}"
case "$cmd" in
    stop)
        say "stopping desktop"
        ps -eo uid,pid,args 2>/dev/null | grep "[t]ermux-x11 com.termux.x11" | while read -r _u pid _r; do kill -9 "$pid" 2>/dev/null; done
        pkill -x sprout 2>/dev/null; pkill -x sprout-super 2>/dev/null
        rm -f "$P/tmp/.X0-lock" "$P/tmp/.X11-unix/X0"
        say "done. (pulseaudio left running on purpose)"
        exit 0 ;;
    status)
        xfce_running && say "desktop RUNNING" || say "desktop NOT running"
        pactl info 2>/dev/null | grep -a "Default Sink" | tr -d '\t' || say "pulse: DOWN"
        xfce_running && status_report
        exit 0 ;;
    start)
        if xfce_running; then
            say "xfce4-session already up — not starting a second instance"
            app_forward; status_report; exit 0
        fi
        audio_gate
        x_gate
        launch_desktop
        app_forward
        status_report
        say "open the Termux:X11 app — desktop paints within ~30s"
        ;;
    restart) "$0" stop; sleep 2; exec "$0" start ;;
    shell)
        say "login bash inside a pty (host tty protected)"
        exec script -qec "sprout -r \"$U\" --shared-tmp --termux-x11 --user=0:0 -- /bin/bash -li" /dev/null ;;
    *) echo "usage: $0 [start|stop|status|restart|shell]" >&2; exit 2 ;;
esac
