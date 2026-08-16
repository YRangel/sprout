#!/data/data/com.termux/files/usr/bin/bash
B=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
A=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs
S=$PREFIX/bin/sprout
pass=0; fail=0
T(){
  local name="$1" got="$2" want="$3"
  if [ "$got" = "$want" ]; then pass=$((pass+1));
  else fail=$((fail+1)); printf "FAIL %-32s got='%s' want='%s'\n" "$name" "$got" "$want"; fi
}
# H6: range assertion for legitimately-drifting counts (env-surface exports,
# musl find counts grow with host-truth as triggers install more links).
TR(){
  local name="$1" got="$2" lo="$3" hi="$4"
  if [ "$got" -ge "$lo" ] 2>/dev/null && [ "$got" -le "$hi" ] 2>/dev/null; then pass=$((pass+1));
  else fail=$((fail+1)); printf "FAIL %-32s got='%s' want='%s..%s'\n" "$name" "$got" "$lo" "$hi"; fi
}
T "-r true"                 "$($S -r $B /usr/bin/true; echo rc=$?)" "rc=0"
T "-w /etc"                 "$($S -r $B -w /etc /bin/bash -c pwd)" "/etc"
T "-b bind"                 "$($S -r $B -b $TMPDIR/sp-a:/mkm /bin/cat /mkm/marker 2>/dev/null)" "mark"
T "default-cwd"             "$($S -r $B /bin/bash -c pwd)" "/root"
T "link2symlink-default"    "$($S -r $B /bin/bash -c 'rm -f /tmp/hl1 2>/dev/null; ln /etc/hostname /tmp/hl1 2>/dev/null && stat -c %F /tmp/hl1')" "symbolic link"
T "no-link2symlink"         "$($S -r $B --no-link2symlink /bin/bash -c 'rm -f /tmp/hl2 2>/dev/null; ln /etc/hostname /tmp/hl2 2>/dev/null && echo ok || echo fail')" "fail"
T "host-home-pwd"           "$($S -r $B --host-home /bin/bash -c pwd)" "/data/data/com.termux/files/home"
T "host-home-HOME"          "$($S -r $B --host-home /usr/bin/env 2>/dev/null | grep '^HOME=')" "HOME=/data/data/com.termux/files/home"
T "host-path"               "$($S -r $B --host-path /usr/bin/env 2>/dev/null | awk '/^PATH=/ {print (index($0,"files/usr/bin")>0)?1:0}')" "1"
T "fallback-ptrace-glibc"   "$($S -r $B --fallback ptrace /usr/bin/true; echo rc=$?)" "rc=0"
T "fallback-ptrace-statics" "$($S -r $B --fallback ptrace /tmp/sp_asm >/dev/null 2>&1; echo rc=$?)" "rc=42"
T "fallback-preload-statics" "$($S -r $B --fallback preload /tmp/sp_asm 2>/dev/null; echo rc=$?)" "rc=1"
T "notify-statics-off"      "$(SPROUT_NOTIFY_STATICS=0 $S -r $B /tmp/sp_asm >/dev/null 2>&1; echo rc=$?)" "rc=42"
TR "USER_NOTIFY=0-musl-find" "$(SPROUT_USER_NOTIFY=0 $S -r $A find /usr/bin -type f 2>/dev/null | wc -l)" 29 31
T "KEEP_UMASK"              "$(SPROUT_KEEP_UMASK=1 $S -r $B /bin/bash -c umask)" "0077"
T "default-umask"           "$($S -r $B /bin/bash -c umask)" "0022"
T "no-fakeroot-env"         "$($S -r $B --no-fakeroot /usr/bin/env 2>/dev/null | grep -c FAKEROOT)" "0"
T "default-fakeroot-env"    "$($S -r $B /usr/bin/env 2>/dev/null | grep SPROUT_FAKEROOT)" "SPROUT_FAKEROOT=1"
TR "musl-find"               "$($S -r $A find /usr/bin -type f 2>/dev/null | wc -l)" 29 31
T "missing-program-error"   "$($S -r $B /no-such-cmd 2>&1 | grep -c 'not found')" "1"
T "tmux-clean"              "$($S -r $B /bin/bash /tmp/probe3.sh 2>/dev/null | grep -c 'access not allowed')" "0"
T "statics-10-B"            "$(for i in $(seq 1 10); do $S -r $B /tmp/sp_asm >/dev/null 2>&1; [ $? -eq 42 ] || echo x; done | wc -l)" "0"
T "statics-10-A"            "$(for i in $(seq 1 10); do $S -r $A /tmp/sp_asm >/dev/null 2>&1; [ $? -eq 42 ] || echo x; done | wc -l)" "0"
T "shared-tmp-vis"          "$($S -r $B --shared-tmp /bin/bash -c 'ls /tmp | head -1 | wc -l')" "1"

TR "dry-run-exports"         "$($S -r $B --dry-run /usr/bin/true 2>&1 | grep -c '^export')" 16 18

T "-k release"              "$($S -r $B -k 5.4.42-sprout /bin/uname -r)" "5.4.42-sprout"
T "-k nested"               "$($S -r $B -k 5.4.42-sprout /bin/bash -c /bin/uname\ -r)" "5.4.42-sprout"
T "-V banner"               "$($S -V 2>/dev/null | head -1)" "sprout $(sed -n 's/^version = "\(.*\)"/\1/p' /data/data/com.termux/files/home/projeto/sprout/crates/sprout-cli/Cargo.toml | head -1)"
T "-h usage"                "$($S -h 2>/dev/null | grep -c '^Usage:')" "1"
T "-v level-accepted"       "$($S -r $B -v 2 /usr/bin/true >/dev/null 2>&1; echo rc=$?)" "rc=0"
cat > $B/tmp/sp_bind80.py <<'PYFIX'
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 80))
print(s.getsockname()[1])
PYFIX
cat > $B/tmp/sp_memfd.py <<'PYFIX'
import ctypes
c = ctypes.CDLL(None)
f = c.memfd_create(b"x", 0)
print(f > 0)
PYFIX
T "-p remap-1024+80"        "$($S -r $B -p /usr/bin/python3 /tmp/sp_bind80.py; sleep 1)" "1104"
T "--sysvipc-parse"         "$($S -r $B --sysvipc /usr/bin/true; echo rc=$?)" "rc=0"
T "--ashmem-memfd"          "$($S -r $B --ashmem-memfd /usr/bin/python3 /tmp/sp_memfd.py)" "True"
T "--kill-on-exit"          "$(P=$B/tmp/sp-koe.pid; rm -f $P; $S -r $B --kill-on-exit /bin/bash -c 'sleep 47 & echo "$!" > /tmp/sp-koe.pid' >/dev/null 2>&1; sleep 0.4; PID=$(cat $P 2>/dev/null); ( kill -0 $PID 2>/dev/null && echo alive ) || echo dead; rm -f $P)" "dead"
T "ptrace-only whoami"      "$(SPROUT_USER_NOTIFY=0 $S -r $B /usr/bin/whoami 2>/dev/null)" "root"
T "ptrace-only id"          "$(SPROUT_USER_NOTIFY=0 $S -r $B /usr/bin/id 2>/dev/null)" "uid=0(root) gid=0(root) groups=0(root)"

echo "SUMMARY: pass=$pass fail=$fail"

