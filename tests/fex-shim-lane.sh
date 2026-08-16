#!/data/data/com.termux/files/usr/bin/bash
# Emulator host-shim lane battery — gates landing of the sprout preload
# emulator SysV-IPC shim lane (main.rs family detection + plan.rs env +
# preload sp_binfmt shim injection; FEX, qemu-*, box64, box32 basenames).
# Run on-device: bash tests/fex-shim-lane.sh
set -u
PREFIX=/data/data/com.termux/files/usr
B=$PREFIX/var/lib/proot-distro/containers/debian/rootfs
FEX=/root/FEX/Build/Bin/FEX
FAILS=""

note() { printf '  %s\n' "$1"; }
ok()   { note "PASS  $1"; }
bad()  { note "FAIL  $1"; FAILS="$FAILS $1"; }

# 1. native baseline (regression cage)
OUT=$(sprout -r $B -- /bin/echo NATIVE_OK 2>/dev/null)
[ "$OUT" = "NATIVE_OK" ] && ok "native exec" || bad "native exec"

# 2. mount-family containment (Android seccomp safe-answer)
RC=$(sprout -r $B -- /bin/mount 2>/dev/null; echo $?)
[ "$RC" != "134" ] && ok "mount family contained" || bad "mount family contained"

# 3. box64 dyn lane exists and runs
RC=$(sprout -r $B -- ls /usr/bin/box64 >/dev/null 2>&1; echo $?)
note "box64 presence rc=$RC"
[ "$RC" = "0" ] && ok "box64 present" || note "box64 absent (non-fatal)"

# 4. FEX lane: host-env LD_PRELOAD carries the arm64 shim AT FRONT
#    (preload-side injection into the FEX process env AT exec time)
ENVDUMP=$(env SPROUT_BINFMT_X86_64=$FEX FEX_ROOTFS=Fedora_44-ext sprout -r $B -q $FEX -- /root/x86probe/env 2>/dev/null)
FIRST=$(printf '%s' "$ENVDUMP" | grep "^LD_PRELOAD=" | head -1)
case "$FIRST" in
  LD_PRELOAD=/usr/lib/sprout-sysvipc/arm64/*) ok "arm64 shim injected at LD_PRELOAD front" ;;
  *) bad "arm64 shim injected at LD_PRELOAD front (got: ${FIRST:-<none>})" ;;
esac

# 5. SPROUT_SYSVIPC_EMU_OFF suppresses the arm64-shim injection
ENVDUMP=$(env SPROUT_SYSVIPC_EMU_OFF=1 SPROUT_BINFMT_X86_64=$FEX FEX_ROOTFS=Fedora_44-ext sprout -r $B -q $FEX -- /root/x86probe/env 2>/dev/null)
if printf '%s' "$ENVDUMP" | grep "^LD_PRELOAD=" | head -1 | grep -q "sprout-sysvipc/arm64"; then
  bad "opt-out suppresses arm64 shim"
else
  ok "opt-out suppresses arm64 shim"
fi

# 6. arm64 shim present for FEX lane
sprout -r $B -- test -f /usr/lib/sprout-sysvipc/arm64/libsprout-sysvipc.so \
  && ok "arm64 shim installed" || bad "arm64 shim installed"

# 7. FEX x32 relaunch probe (self-exec resolution, e674887)
if sprout -r $B -- test -f /tmp/relaunch32 2>/dev/null; then
  OUT=$(env SPROUT_BINFMT_X86_64=$FEX FEX_ROOTFS=Fedora_44-ext \
        sprout -r $B -q $FEX -- /tmp/relaunch32 2>/dev/null | grep -c "RELAUNCH32_CHILD_OK" || true)
  [ "$OUT" = "1" ] && ok "FEX x32 relaunch self-exec" || bad "FEX x32 relaunch self-exec"
else
  note "SKIP  relaunch32 probe absent in guest"
fi

# 8. static wrap lane alive (2bcd005 guard: no shim into static images)
OUT=$(sprout -r $B -- /bin/true 2>/dev/null; echo $?)
[ "$OUT" = "0" ] && ok "plain static exec" || bad "plain static exec"

if [ -n "$FAILS" ]; then
  echo "BATTERY FAIL:$FAILS"
  exit 1
fi
echo "BATTERY PASS"
exit 0
