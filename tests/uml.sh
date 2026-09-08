#!/data/data/com.termux/files/usr/bin/bash
# uml.sh — UML sidecar gates. Follows tests/smoke.sh PASS/FAIL vocabulary.
# Skips gracefully when no linux.uml binary (pre-Phase-1 CI).
# Usage: tests/uml.sh
set -u
SPROUT_BIN="${SPROUT_BIN:-./target/debug/sprout}"
pass=0; fail=0; skip=0
ok()   { echo "PASS: $1"; pass=$((pass+1)); }
no()   { echo "FAIL: $1 -- $2"; fail=$((fail+1)); }
sk()   { echo "SKIP: $1"; skip=$((skip+1)); }

echo "=== sprout uml gates ==="

# gate 0: CLI surface exists, fast lane untouched
if "$SPROUT_BIN" uml 2>&1 | grep -q "sprout uml up"; then ok "uml help surface";
else no "uml help surface" "help text missing"; fi

# gate 1: up/down idempotency WITHOUT a kernel (error paths must be clean)
if "$SPROUT_BIN" uml down --id uml-test 2>&1 | grep -qi "not running"; then
    ok "down idempotent (no guest)"
else no "down idempotent (no guest)" "unexpected output"; fi

if "$SPROUT_BIN" uml status --id uml-test >/dev/null 2>&1; then
    no "status down code" "expected exit != 0 when down"
else ok "status down code (non-zero when down)"; fi

if "$SPROUT_BIN" uml up --id uml-test 2>&1 | grep -qi "no linux.uml binary"; then
    ok "up fails clean without kernel"
else no "up fails clean without kernel" "wrong error"; fi

[ -f "$HOME/.sprout/uml/uml-test/pid" ] \
    && no "no stale pid on failed up" "pid file leaked" \
    || ok "no stale pid on failed up"

# gate 2: double-down is ok (plan §8 gate 1)
"$SPROUT_BIN" uml down --id uml-test >/dev/null 2>&1 \
    && ok "double down ok" || no "double down ok" "non-zero exit"

# gate 3: agent protocol loopback (needs the C agent; build to $TMPDIR)
# Uses a scratch port (no cross-talk with stray agents on 2225).
AGT_PORT=22466
if command -v cc >/dev/null 2>&1 && [ -f uml/sprout-uml-agent.c ]; then
    AGT="${TMPDIR:-/tmp}/sprout-uml-agent-test"
    rm -f "$AGT"
    if cc -O2 -std=c11 -o "$AGT" uml/sprout-uml-agent.c 2>/dev/null; then
        SOCKD="${TMPDIR:-/tmp}/uml-agent-test"; mkdir -p "$SOCKD"
        # agent binds /run/sprout/exec.sock OR 127.0.0.1:$AGT_PORT fallback;
        # probe the port with a real connect (bash /dev/tcp is unreliable).
        if python3 -c "import socket; socket.create_connection(('127.0.0.1', $AGT_PORT), timeout=2).close()" 2>/dev/null; then
            sk "agent loopback (port $AGT_PORT busy)"
        else
            ("$AGT" 8 "$AGT_PORT" >/dev/null 2>&1 & echo $! > "$AGT.pid")
            APID=$(cat "$AGT.pid" 2>/dev/null || echo 0)
            # wait for the listener (max 5s). NOTE: each readiness probe
            # consumes one prefork worker for 30s (SO_RCVTIMEO), so with the
            # default worker count the pool drains fast. The test binary is
            # started with 1 worker; the single readiness probe below takes
            # it, and the released worker returns to accept() after timeout
            # — instead of probing in a loop, sleep once then test directly.
            sleep 1
            if python3 -c "
import socket, struct
# one connection does ping AND exec (no separate readiness probe —
# each extra connection pins a worker for the 30s recv timeout).
s = socket.create_connection(('127.0.0.1', $AGT_PORT), timeout=5)
def w(b): s.sendall(b)
def su(n): w(struct.pack('<I', n))
def ss(b):
    if isinstance(b, str): b = b.encode()
    su(len(b)); w(b)
w(b'\x01')
su(2); ss('/bin/echo'); ss('uml-alive')
su(1); ss('PATH=/bin:/usr/bin')
ss('/'); su(0)
su(0); su(0); su(5000); su(0)
def rd(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise AssertionError('eof')
        b += c
    return b
out = b''
while True:
    st, ln = struct.unpack('<II', rd(8))
    if st == 0:
        code, = struct.unpack('<i', rd(4))
        assert code == 0, f'exit {code}'
        break
    out += rd(ln)
assert b'uml-alive' in out, out
print('agent exec roundtrip ok')
"; then ok "agent exec roundtrip (TCP loopback)";
            else no "agent exec roundtrip (TCP loopback)" "see above"; fi
            kill -9 $APID 2>/dev/null; wait 2>/dev/null
            pkill -9 -f "sprout-uml-agent-test" 2>/dev/null; sleep 0.5
            rm -f "$AGT"
        fi
    else sk "agent build (cc failed)"; fi
else sk "agent loopback (no cc or no agent source)"; fi

echo ""
echo "=== uml: $pass pass, $fail fail, $skip skip ==="
[ "$fail" -eq 0 ]
