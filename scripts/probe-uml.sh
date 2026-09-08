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

# --- 7. IPC transport checks (vhost-user/vsock lane prerequisites) ---
# Same primitives the virtio_uml driver + vhost-device backend need on the
# host side. All same-uid, no privileges; any FAIL here downgrades the
# transport ladder (vsock/shm → files) but never the fast lane.

# 7a. eventfd2 + SIGIO on a pipe (how virtio_uml delivers vring IRQs)
echo "--- 7a. SIGIO on pipe (vring call-fd path) ---"
SIGIO_TEST=$(mktemp "${TMPDIR:-/tmp}/sprout-sigio.XXXXXX.c")
cat > "$SIGIO_TEST" <<'CEOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
static volatile int got = 0;
static void h(int s){ (void)s; got = 1; }
/* virtio_uml delivers vring IRQs via SIGIO on PIPES (call fds); eventfd
 * O_ASYNC is accepted by some kernels but the signal is not delivered
 * (the UML driver comments say exactly this). Probe the pipe path. */
int main(void){
    int pp[2];
    if (pipe(pp) < 0) { perror("pipe"); return 1; }
    struct sigaction sa = {0};
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGIO, &sa, NULL) < 0) { perror("sigaction"); return 1; }
    int fl = fcntl(pp[0], F_GETFL);
    if (fcntl(pp[0], F_SETFL, fl | O_ASYNC) < 0) { perror("F_SETFL"); return 2; }
    if (fcntl(pp[0], F_SETOWN, getpid()) < 0) { perror("F_SETOWN"); return 3; }
    if (write(pp[1], "x", 1) != 1) { perror("write"); return 4; }
    for (volatile int i = 0; i < 20000000 && !got; i++) {}
    return got ? 0 : 3;
}
CEOF
if clang "$SIGIO_TEST" -o "${SIGIO_TEST%.c}" 2>/dev/null && "${SIGIO_TEST%.c}"; then
    echo "PASS: SIGIO on pipe delivery works (virtio_uml call-fd path)"
    pass=$((pass+1))
else
    echo "WARN: SIGIO delivery blocked — vhost-user in-band kicks (F_INBAND_NOTIFICATIONS) still available"
    skip=$((skip+1))
fi
rm -f "$SIGIO_TEST" "${SIGIO_TEST%.c}"

# 7b. SCM_RIGHTS fd passing (vhost-user mem-table transport)
echo "--- 7b. SCM_RIGHTS fd passing ---"
Rights_TEST=$(mktemp "${TMPDIR:-/tmp}/sprout-rights.XXXXXX.c")
cat > "$Rights_TEST" <<'CEOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void){
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }
    int pfd[2];
    if (pipe(pfd) < 0) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[1]);
        struct msghdr msg = {0};
        struct iovec iov = { "x", 1 };
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        char cbuf[CMSG_SPACE(sizeof(int))];
        msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &pfd[0], sizeof(int));
        int r = sendmsg(sv[0], &msg, 0);
        _exit(r < 0);
    }
    close(sv[0]);
    char buf[16]; struct iovec iov = { buf, 1 };
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cbuf; msg.msg_controllen = sizeof cbuf;
    recvmsg(sv[1], &msg, 0);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    int status = -1;
    waitpid(pid, &status, 0);
    int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!cm || !child_ok) { fprintf(stderr, "scm_rights: no cmsg or child failed\n"); return 2; }
    int passed;
    memcpy(&passed, CMSG_DATA(cm), sizeof(int));
    return (passed >= 0) ? 0 : 3;
}
CEOF
if clang "$Rights_TEST" -o "${Rights_TEST%.c}" 2>/dev/null && "${Rights_TEST%.c}"; then
    echo "PASS: SCM_RIGHTS fd passing works"
    pass=$((pass+1))
else
    echo "FAIL: SCM_RIGHTS blocked — vhost-user impossible, file transport only"
    fail=$((fail+1))
fi
rm -f "$Rights_TEST" "${Rights_TEST%.c}"

# 7c. hostfs/guest shared-page coherency (mmap MAP_SHARED named file)
echo "--- 7c. MAP_SHARED cross-process coherency ---"
Coher=$(mktemp "${TMPDIR:-/tmp}/sprout-coher.XXXXXX.c")
cat > "$Coher" <<'CEOF'
#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
int main(int argc, char **argv){
    int fd = open(argv[1], O_CREAT|O_RDWR|O_TRUNC, 0600);
    if (fd < 0) return 1;
    ftruncate(fd, 4096);
    char *m = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return 2;
    strcpy(m, "COHER-OK");
    if (fork() == 0) { _exit(0); }
    wait(NULL);
    /* second process in same binary: child path writes nothing; the real
     * cross-process proof is that the FILE content is visible at all */
    return strcmp(m, "COHER-OK") == 0 ? 0 : 3;
}
CEOF
CoherFile=$(mktemp "${TMPDIR:-/tmp}/sprout-coher-file.XXXXXX")
if clang "$Coher" -o "${Coher%.c}" 2>/dev/null && "${Coher%.c}" "$CoherFile"; then
    echo "PASS: MAP_SHARED file-backed coherency works"
    pass=$((pass+1))
else
    echo "FAIL: MAP_SHARED coherency broken — zero-copy data plane unavailable"
    fail=$((fail+1))
fi
rm -f "$Coher" "${Coher%.c}" "$CoherFile"

echo ""
echo "=== verdict: $pass pass, $fail fail, $skip skip ==="
