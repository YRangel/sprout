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
T "USER_NOTIFY=0-musl-find" "$(SPROUT_USER_NOTIFY=0 $S -r $A find /usr/bin -type f 2>/dev/null | wc -l)" "29"
T "KEEP_UMASK"              "$(SPROUT_KEEP_UMASK=1 $S -r $B /bin/bash -c umask)" "0077"
T "default-umask"           "$($S -r $B /bin/bash -c umask)" "0022"
T "no-fakeroot-env"         "$($S -r $B --no-fakeroot /usr/bin/env 2>/dev/null | grep -c FAKEROOT)" "0"
T "default-fakeroot-env"    "$($S -r $B /usr/bin/env 2>/dev/null | grep SPROUT_FAKEROOT)" "SPROUT_FAKEROOT=1"
T "musl-find"               "$($S -r $A find /usr/bin -type f 2>/dev/null | wc -l)" "29"
T "missing-program-error"   "$($S -r $B /no-such-cmd 2>&1 | grep -c 'not found')" "1"
T "tmux-clean"              "$($S -r $B /bin/bash /tmp/probe3.sh 2>/dev/null | grep -c 'access not allowed')" "0"
T "statics-10-B"            "$(for i in $(seq 1 10); do $S -r $B /tmp/sp_asm >/dev/null 2>&1; [ $? -eq 42 ] || echo x; done | wc -l)" "0"
T "statics-10-A"            "$(for i in $(seq 1 10); do $S -r $A /tmp/sp_asm >/dev/null 2>&1; [ $? -eq 42 ] || echo x; done | wc -l)" "0"
T "shared-tmp-vis"          "$($S -r $B --shared-tmp /bin/bash -c 'ls /tmp | head -1 | wc -l')" "1"
T "dry-run-exports"         "$($S -r $B --dry-run /usr/bin/true 2>&1 | grep -c '^export')" "16"
echo "SUMMARY: pass=$pass fail=$fail"
