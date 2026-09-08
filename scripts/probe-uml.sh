#!/data/data/com.termux/files/usr/bin/bash
# probe-uml.sh — Phase-0 go/no-go for the sprout+UML sidecar.
# No code touched. Prints PASS/FAIL per gate + a verdict.
# Usage: scripts/probe-uml.sh
set -u
OUT="${TMPDIR:-$HOME/.sprout/probe}/probe-uml-out.txt"
mkdir -p "$(dirname "$OUT")"
pass=0; fail=0; skip=0
gate() { # gate <name> <command...>
    local name="$1"; shift
    if "$@" >"$OUT" 2>&1; then
        echo "PASS: $name"; pass=$((pass+1))
    else
        echo "FAIL: $name -- $(head -c 200 "$OUT")"
        fail=$((fail+1))
    fi
}

echo "=== sprout UML sidecar Phase-0 probe ==="
echo "date: $(date -u +%FT%TZ)  kernel: $(uname -r)  arch: $(uname -m)"

# 1. ptrace scope (want 0; 1 may still pass with PR_SET_PTRACER)
echo "--- 1. ptrace ---"
if [ -r /proc/sys/kernel/yama/ptrace_scope ]; then
    scope=$(cat /proc/sys/kernel/yama/ptrace_scope)
    echo "ptrace_scope=$scope"
    # functional test: can we actually trace a child?
    gate "ptrace attach self-child" python3 -c "
import ctypes, os
libc = ctypes.CDLL(None)
PTRACE_TRACEME = 0
pid = os.fork()
if pid == 0:
    libc.ptrace(PTRACE_TRACEME, 0, 0, 0)
    os._exit(42)
_, status = os.waitpid(pid, 0)
assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 42, hex(status)
"
else
    echo "SKIP: no yama node (non-Yama kernel — ptrace likely OK)"
    skip=$((skip+1))
fi

# 2. mmap MAP_FIXED over an existing mapping (UML SKAS0 needs it)
echo "--- 2. mmap ---"
gate "mmap MAP_FIXED replace" python3 -c "
import ctypes
libc = ctypes.CDLL(None)
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_longlong]
libc.mmap.restype = ctypes.c_void_p
addr = libc.mmap(None, 65536, 3, 0x22, -1, 0)  # PRIVATE|ANON
assert addr != 0xFFFFFFFFFFFFFFFF, 'anon mmap failed'
r = libc.mmap(addr, 65536, 3, 0x22|0x10, -1, 0)  # +FIXED on same addr
assert r == addr, 'MAP_FIXED replace failed'
"

# 3. AF_UNIX socket file usable (uml transport fast path)
echo "--- 3. unix socket ---"
gate "AF_UNIX SOCK_SEQPACKET loopback" python3 -c "
import socket, os
p = os.path.join(os.environ.get('TMPDIR','/tmp'), 'probe-uml.sock')
try: os.unlink(p)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.bind(p); s.listen(1)
c = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
c.connect(p)
a,_ = s.accept()
c.send(b'ping')
assert a.recv(16) == b'ping', 'roundtrip mismatch'
c.close(); a.close(); s.close(); os.unlink(p)
"

# 3b. socket file on a hostfs-style shared path (~/.sprout probe dir)
echo "--- 3b. socket on share dir ---"
gate "AF_UNIX socket under ~/.sprout" bash -c "
d=\"\$HOME/.sprout/probe\"; mkdir -p \"\$d\";
python3 -c \"
import socket, os
p = os.path.expanduser('~/.sprout/probe/t.sock')
try: os.unlink(p)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(p); s.listen(1)
s.close(); os.unlink(p)
\""

# 4. UML binary present? (informational — not a failure if absent)
echo "--- 4. uml binary ---"
if command -v linux.uml >/dev/null 2>&1 || [ -x ./linux.uml ] || [ -n "${UML_BIN:-}" ]; then
    echo "PASS: uml binary available (${UML_BIN:-$(command -v linux.uml 2>/dev/null || echo ./linux.uml)})"
    pass=$((pass+1))
    # 5. headless boot attempt (only if binary exists; 120s cap)
    echo "--- 5. headless boot (120s cap) ---"
    echo "SKIP: automated boot check needs a rootfs image; run manually:"
    echo "  linux.uml ubd0=rootfs.ext4 root=/dev/ubda mem=512M con=null con0=null,fd:2 umid=sprout-probe"
    skip=$((skip+1))
else
    echo "SKIP: no linux.uml binary (built by CI per build-uml.sh: ARCH=um SUBARCH=x86_64)"
    skip=$((skip+1))
fi

# 6. unshare user namespace (tells us whether nspawn-style fallback exists)
echo "--- 6. userns ---"
if unshare -U true 2>/dev/null; then
    echo "PASS: user namespace available"
    pass=$((pass+1))
else
    echo "FAIL: user namespace denied (expected on stock Android — UML sidecar unaffected)"
    fail=$((fail+1))
fi

echo ""
echo "=== verdict: $pass pass, $fail fail, $skip skip ==="
if [ "$fail" -eq 0 ]; then
    echo "GO: host permits the UML sidecar path (pending real boot test)"
elif [ "$fail" -eq 1 ] && ! unshare -U true 2>/dev/null; then
    echo "GO (with note): only userns denied — irrelevant for UML, relevant for nspawn fallback"
else
    echo "NO-GO: fix FAILED gates before Phase-1"
fi
