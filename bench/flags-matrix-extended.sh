#!/data/data/com.termux/files/usr/bin/bash
# sprout — extended flag/argv/error-branch battery.
# Complements bench/flags-matrix.sh (the happy-path 25). Run from anywhere;
# uses the same probe convention: T <name> "<got>" "<want>".
B=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
A=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs
S=$PREFIX/bin/sprout
pass=0; fail=0
T(){
  local name="$1" got="$2" want="$3"
  if [ "$got" = "$want" ]; then pass=$((pass+1));
  else fail=$((fail+1)); printf "FAIL %-34s got='%s' want='%s'\n" "$name" "$got" "$want"; fi
}

# --- error branches & bad inputs
T "bad-rootfs-gives-error"      "$($S -r /nonexistent/path /usr/bin/true 2>&1 | head -1 | grep -c 'sprout\|No such')" "1"
T "bad-rootfs-rc-nonzero"       "$($S -r /nonexistent/path /usr/bin/true 2>/dev/null; echo rc=$?)" "rc=1"
T "bad-binding-rc"              "$($S -r $B -b garbage /usr/bin/true 2>/dev/null; echo rc=$?)" "rc=1"
T "bad-binding-msg"             "$($S -r $B -b garbage /usr/bin/true 2>&1 | grep -c 'invalid binding')" "1"
T "missing-program-msg"         "$($S -r $B /no-such-cmd 2>&1 | grep -c 'not found')" "1"
T "missing-program-rc"          "$($S -r $B /no-such-cmd 2>/dev/null; echo rc=$?)" "rc=1"
T "missing-file-as-script"      "$($S -r $B /tmp/no-such-file.sh 2>/dev/null; echo rc=$?)" "rc=1"
T "bad-w-nonexistent-dir"       "$($S -r $B -w /no/such/dir /usr/bin/true 2>/dev/null; echo rc=$?)" "rc=1"
T "bad-fallback-value-msg"      "$($S -r $B --fallback=banana /usr/bin/true 2>&1 | grep -c 'fallback')" "1"

# --- exit-status propagation (proot-parity)
T "exit39-passes-up"            "$($S -r $B /bin/bash -c 'exit 39' >/dev/null 2>&1; echo rc=$?)" "rc=39"
T "exit1-passes-up"             "$($S -r $B /bin/bash -c 'exit 1' >/dev/null 2>&1; echo rc=$?)" "rc=1"
T "true-rc0"                    "$($S -r $B /usr/bin/true >/dev/null 2>&1; echo rc=$?)" "rc=0"
T "false-rc1"                   "$($S -r $B /usr/bin/false >/dev/null 2>&1; echo rc=$?)" "rc=1"

# --- argv fidelity (proot-parity)
T "argv-space-preservation"     "$($S -r $B /usr/bin/printf '%s|' a 'b c' 'd  e' )" "a|b c|d  e|"
T "argv-empty-arg"              "$($S -r $B /usr/bin/printf '[%s]' a '' b)" "[a][][b]"
T "env-passthrough-visible"     "$(FOO=bar $S -r $B /bin/bash -c 'printf %s $FOO')" "bar"
T "env-passthrough-adds"        "$(FOO=bar $S -r $B /bin/bash -c 'echo X_$FOO')" "X_bar"

# --- env knobs surface
T "SPROUT_GUEST_PATH"           "$(SPROUT_GUEST_PATH=/made-up $S -r $B /bin/bash -c 'echo $PATH' 2>&1 | grep -c '/made-up')" "1"
T "SPROUT_DEBUG-visible"        "$(SPROUT_DEBUG=1 $S -r $B /usr/bin/true 2>&1 | awk '/notify|ptrace|sprout/ {n++} END {print (n>4)?2:0}')" "2"
T "SPROUT_USER_NOTIFY=0-glibc-statics" "$(SPROUT_USER_NOTIFY=0 $S -r $B /tmp/sp_asm >/dev/null 2>&1; echo rc=$?)" "rc=42"
T "SPROT_NOTIFY_BYPASS_MSG"     "$(SPROT_NOTIFY_NO_NOSTAT=1 $S -r $B /tmp/sp_statloop >/dev/null 2>&1; echo rc=$?)" "rc=42"

# --- alpine (musl) coverage of main flags
T "musl-default-cwd"            "$($S -r $A /bin/busybox sh -c pwd 2>/dev/null)" "/root"
T "musl-w-etc"                  "$($S -r $A -w /etc /bin/busybox pwd 2>/dev/null)" "/etc"
T "musl-b-bind"                 "$($S -r $A -b $TMPDIR/sp-a:/mkm /bin/cat /mkm/marker 2>/dev/null)" "mark"
T "musl-no-fakeroot"            "$($S -r $A --no-fakeroot /usr/bin/env 2>/dev/null | grep -c FAKEROOT)" "0"
T "musl-host-home"              "$($S -r $A --host-home /bin/busybox pwd 2>/dev/null)" "/data/data/com.termux/files/home"
T "musl-umask-default"          "$($S -r $A /bin/busybox sh -c umask 2>/dev/null)" "0022"
T "musl-notify-statics-off"     "$(SPROUT_NOTIFY_STATICS=0 $S -r $A /tmp/sp_asm >/dev/null 2>&1; echo rc=$?)" "rc=42"
T "musl-find-29"                "$($S -r $A find /usr/bin -type f 2>/dev/null | wc -l)" "29"
T "musl-exit39"                 "$($S -r $A /bin/busybox sh -c 'exit 39' 2>/dev/null; echo rc=$?)" "rc=39"

# --- script/shebang class
T "script-smoke"                "$($S -r $B /bin/bash -c 'rm -f /tmp/noob.sh; printf "#!/bin/nonexistent\\n" > /tmp/noob.sh; chmod +x /tmp/noob.sh; /tmp/noob.sh; echo rc=$?; exit 0' 2>&1 | tail -1)" "rc=127"
T "dry-run-statics-env"         "$($S -r $B --dry-run /tmp/sp_asm 2>&1 | grep -c '^export')" "12"

# --- command arg edge cases
T "argv-dash-leading"           "$($S -r $B /bin/bash -c 'printf "<%s>" "$1"' _ -x)" "<-x>"
T "argv-big-count"              "$($S -r $B /bin/bash -c '/usr/bin/gcc -o /dev/null $(for i in $(seq 1 400); do printf /temp/x$i.c; printf " "; done) 2>/dev/null 1>&2; echo rc=$?; exit 0' | tail -1)" "rc=1"
T "host-path-addit-not-override" "$($S -r $B --host-path /bin/bash -c 'echo $PATH' | awk -v RS=':' '/files\/usr\/bin/ { hits++; next } /\/usr\/bin/ { guest++ } END { print (hits>0 && guest>0)?1:0 }')" "1"

# --- --user anchor (proot -i / proot-distro --user parity)
T "user-daemon-id"              "$($S -r $B --user daemon /bin/bash -c 'echo $(id -u):$(id -g)' 2>/dev/null)" "1:1"
T "user-daemon-whoami"          "$($S -r $B --user daemon /usr/bin/whoami 2>/dev/null)" "daemon"
T "user-numeric-1-2"            "$($S -r $B --user 1:2 /bin/bash -c 'echo $(id -u):$(id -g)' 2>/dev/null)" "1:2"
T "user-nobody-home-fallback"   "$($S -r $B --user nobody /bin/bash -c pwd 2>/dev/null)" "/"
T "user-daemon-home-cwd"        "$($S -r $B --user daemon /bin/bash -c pwd 2>/dev/null)" "/usr/sbin"
T "user-env-USER-LOGNAME"       "$($S -r $B --user daemon /usr/bin/env 2>/dev/null | grep -cE '^(USER|LOGNAME)=daemon')" "2"
T "user-shell-from-passwd"      "$($S -r $B --user daemon /usr/bin/env 2>/dev/null | grep '^SHELL=')" "SHELL=/usr/sbin/nologin"
T "user-anchor-owner-spoof"     "$($S -r $B --user daemon /bin/bash -c 'rm -f /tmp/uu; touch /tmp/uu; stat -c %U /tmp/uu' 2>/dev/null)" "daemon"
T "user-anchor-peercred"        "$(timeout 30 $S -r $B --user daemon python3 /tmp/pc.py 2>/dev/null | awk -F'[= ]' '{print $5":"$7}' | head -1)" "1:1"
T "user-unknown-rc"             "$($S -r $B --user ghost /usr/bin/true 2>/dev/null; echo rc=$?)" "rc=1"
T "user-unknown-msg"            "$($S -r $B --user ghost /usr/bin/true 2>&1 | grep -c 'not found')" "1"
T "user-conflict-rc2"           "$($S -r $B --user daemon --no-fakeroot /usr/bin/true 2>/dev/null >/dev/null; echo rc=$?)" "rc=2"
T "user-musl-bin"               "$($S -r $A --user bin /bin/busybox id -u 2>/dev/null)" "1"
T "user-musl-nobody"            "$($S -r $A --user nobody /bin/busybox id -u 2>/dev/null)" "65534"
T "user-default-unchanged"      "$($S -r $B /bin/bash -c 'echo $(id -u):$(id -g)' 2>/dev/null)" "0:0"

# --- guest user lifecycle (useradd/audit netlink/nlink lock parity)
T "audit-netlink-eprotonosupport" "$($S -r $B python3 /tmp/aud.py 2>/dev/null | grep -c 'errno=93')" "1"
T "useradd-lifecycle"            "$($S -r $B /bin/bash -c 'rm -f /etc/passwd.lock /etc/group.lock /etc/shadow.lock /etc/gshadow.lock; userdel tprobe 2>/dev/null; rm -rf /home/tprobe; useradd -m tprobe 2>&1 >/dev/null; rc=$?; grep -c ^tprobe: /etc/passwd; exit $rc' | tail -1)" "1"
T "user-created-home-drop"        "$($S -r $B --user tester /bin/bash -c 'pwd' 2>/dev/null)" "/home/tester"
T "user-created-whoami"           "$($S -r $B --user tester /usr/bin/whoami 2>/dev/null)" "tester"
T "apt-as-daemon-runs"           "$(timeout 90 $S -r $B --user daemon apt-get update 2>/dev/null 1>&2; echo rc=$?)" "rc=0"

# --- sanity: supervisor not left running as zombie after each call
T "no-zombie-sprout"            "$(n=0; for p in $(pgrep -f sprout-super 2>/dev/null); do [ "$(basename $(readlink /proc/$p/exe 2>/dev/null))" = "sprout-super" ] && n=$((n+1)); done; echo $n)" "0"

echo "SUMMARY: pass=$pass fail=$fail"
