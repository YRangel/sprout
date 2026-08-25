#!/data/data/com.termux/files/usr/bin/sh
# pulse-guard.sh — HyperOS pulseaudio hang watchdog.
# The daemon intermittent-rots into a half-loaded shell that answers NOTHING
# (unix socket + 4713 gone, `pulseaudio --kill` often misfires against it).
# Guard: probe every 20s; on failure numeric-kill every pulseaudio pid, scrub
# runtime state, restart. Singleton via flock on $PREFIX/tmp/pulse-guard.lock.
P=/data/data/com.termux/files/usr
LOGFILE=$P/tmp/pulse-guard.log
LOCK=$P/tmp/pulse-guard.lock
exec 9>"$LOCK"
flock -n 9 || exit 0    # another guard already running

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >>"$LOGFILE"; }

probe() {
    probe1_RC=1
    probe2_RC=1
    timeout 4 "$P/bin/pactl" info 2>/dev/null | grep -aq "Default Sink" && probe1_RC=0
    timeout 4 python3 - <<'EOF' >/dev/null 2>&1
import socket
s=socket.create_connection(('127.0.0.1',4713),timeout=3); s.close()
EOF
    [ $? -eq 0 ] && probe2_RC=0
    [ $probe1_RC -eq 0 ] && [ $probe2_RC -eq 0 ]
}

revive() {
    log "probe failed (pactl=$probe1_RC tcp=$probe2_RC) — reviving"
    for PID in $(ps -eo pid,args 2>/dev/null | grep -a "[p]ulseaudio" | awk '{print $1}'); do
        kill -9 "$PID" 2>/dev/null
    done
    sleep 1
    rm -rf "$P"/tmp/pulse-* "$HOME"/.config/pulse/*runtime 2>/dev/null
    pulseaudio --start 2>/dev/null
    sleep 3
    if probe; then log "revived OK"; else log "revive attempt still failing"; fi
}

log "guard start pid=$$"
while :; do
    probe || revive
    sleep 20
done
