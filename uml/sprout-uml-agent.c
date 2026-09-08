//! `sprout-uml-agent` — guest-side exec daemon for the UML sidecar.
//!
//! Runs as a systemd unit inside the UML guest. Preforks N workers that
//! block in accept(); each request is vfork+exec'd with the requested
//! uid/gid/cwd/env. Stdio is spliced to the socket. Protocol v1 matches
//! sprout-cli/src/uml.rs (sprout-uml-plan.md §3.2).
//!
//! Build (inside guest or any Linux with a C toolchain):
//!   cc -O2 -std=c11 -Wall -o sprout-uml-agent sprout-uml-agent.c

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROTO_PING 0x00
#define PROTO_EXEC 0x01
#define PROTO_SHUTDOWN 0x02
#define RESP_STDOUT 1u
#define RESP_STDERR 2u
#define RESP_EXIT 0u
#define SOCK_PATH "/run/sprout/exec.sock"
static int tcp_port = 2225;
#define IO_CHUNK (128u * 1024u)

static ssize_t read_full(int fd, void *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (char *)b + off, n - off);
        if (r == 0) return (ssize_t)off; /* EOF */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}
static int write_full(int fd, const void *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = write(fd, (const char *)b + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}
static int read_u32(int fd, uint32_t *o) {
    uint8_t b[4];
    if (read_full(fd, b, 4) != 4) return -1;
    memcpy(o, b, 4);
    return 0;
}
static int send_frame(int fd, uint32_t stream, const void *b, size_t n) {
    /* chunk to IO_CHUNK so one malloc never grows with output size */
    const uint8_t *p = b;
    if (n == 0 && stream != RESP_EXIT) {
        uint8_t h[8] = {0};
        memcpy(h, &stream, 4);
        return write_full(fd, h, 8);
    }
    while (n > 0 || (b == NULL && stream == RESP_EXIT)) {
        size_t c = n > IO_CHUNK ? IO_CHUNK : n;
        uint8_t h[8];
        memcpy(h, &stream, 4);
        uint32_t cl = (uint32_t)c;
        memcpy(h + 4, &cl, 4);
        if (write_full(fd, h, 8) < 0) return -1;
        if (c && write_full(fd, p, c) < 0) return -1;
        p += c;
        n -= c;
        if (stream == RESP_EXIT) break;
    }
    return 0;
}

/* pump fd -> socket as RESP frames until EOF. single-threaded select loop
 * over stdout+stderr of the child. */
static void pump(int wfd, int out_fd, int err_fd) {
    uint8_t buf[IO_CHUNK];
    int o_open = 1, e_open = 1;
    while (o_open || e_open) {
        fd_set rf;
        FD_ZERO(&rf);
        if (o_open) FD_SET(out_fd, &rf);
        if (e_open) FD_SET(err_fd, &rf);
        int mx = out_fd > err_fd ? out_fd : err_fd;
        if (select(mx + 1, &rf, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (o_open && FD_ISSET(out_fd, &rf)) {
            ssize_t r = read(out_fd, buf, sizeof buf);
            if (r <= 0) o_open = 0;
            else if (send_frame(wfd, RESP_STDOUT, buf, (size_t)r) < 0) break;
        }
        if (e_open && FD_ISSET(err_fd, &rf)) {
            ssize_t r = read(err_fd, buf, sizeof buf);
            if (r <= 0) e_open = 0;
            else if (send_frame(wfd, RESP_STDERR, buf, (size_t)r) < 0) break;
        }
    }
}

static char *read_str(int fd) {
    uint32_t n;
    if (read_u32(fd, &n) < 0) return NULL;
    if (n > (16u << 20u)) return NULL; /* 16MB sanity cap per string */
    char *s = malloc(n + 1);
    if (!s) return NULL;
    if (n && read_full(fd, s, n) != (ssize_t)n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

/* drop privileges per request; returns 0 ok. root stays root when uid=0. */
static int become(uint32_t uid, uint32_t gid) {
    if (gid && setresgid(gid, gid, gid) < 0) return -1;
    if (gid) {
        /* shed supplementary groups, then adopt target's */
        struct passwd *pw = getpwuid(uid);
        if (pw) initgroups(pw->pw_name, gid);
    }
    if (uid && setresuid(uid, uid, uid) < 0) return -1;
    return 0;
}

static void handle_exec(int cfd, int wfd) {
    uint32_t argc, envc, i;
    char **argv = NULL, **envp = NULL, *cwd = NULL, *stdin_b = NULL;
    uint32_t stdin_n = 0, uid = 0, gid = 0, tmo = 0, flags = 0;
    if (read_u32(cfd, &argc) < 0 || argc == 0 || argc > 4096) goto bad;
    argv = calloc(argc + 1, sizeof *argv);
    if (!argv) goto bad;
    for (i = 0; i < argc; i++) {
        argv[i] = read_str(cfd);
        if (!argv[i]) goto bad;
    }
    if (read_u32(cfd, &envc) < 0 || envc > 4096) goto bad;
    envp = calloc(envc + 1, sizeof *envp);
    if (!envp) goto bad;
    for (i = 0; i < envc; i++) {
        envp[i] = read_str(cfd);
        if (!envp[i]) goto bad;
    }
    cwd = read_str(cfd);
    if (!cwd) goto bad;
    if (read_u32(cfd, &stdin_n) < 0 || stdin_n > (64u << 20u)) goto bad;
    if (stdin_n) {
        stdin_b = malloc(stdin_n);
        if (!stdin_b) goto bad;
        if (read_full(cfd, stdin_b, stdin_n) != (ssize_t)stdin_n) goto bad;
    }
    if (read_u32(cfd, &uid) < 0 || read_u32(cfd, &gid) < 0 ||
        read_u32(cfd, &tmo) < 0 || read_u32(cfd, &flags) < 0) goto bad;
    (void)tmo;
    (void)flags;

    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
    if (pipe(in_p) < 0 || pipe(out_p) < 0 || pipe(err_p) < 0) goto bad2;

    /* fork(), not vfork(): the child runs become() (NSS malloc) and
     * execvpe before exec. Under bionic, vfork shares the parent's
     * memory (CLONE_VM), so any allocation here is undefined behavior
     * and was observed losing the child's stdout on Android.
     * The hot path never spawns, so fork() cost is fine. */
    pid_t pid = fork();
    if (pid < 0) goto bad2;
    if (pid == 0) {
        /* child: minimal work between vfork and execve */
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        dup2(err_p[1], 2);
        close(in_p[0]); close(in_p[1]);
        close(out_p[0]); close(out_p[1]);
        close(err_p[0]); close(err_p[1]);
        if (cwd[0] && chdir(cwd) < 0) _exit(127);
        /* become() uses NSS (malloc) — not vfork-safe in theory; in
         * practice glibc NSS is cached post-startup. Strictly-correct
         * path would be posix_spawn with file actions + id callbacks;
         * kept simple: common uids resolve from nscd/host cache. */
        if (become(uid, gid) < 0) _exit(126);
        execvpe(argv[0], argv, envp ? envp : environ);
        _exit(127);
    }
    close(in_p[0]); close(out_p[1]); close(err_p[1]);
    if (stdin_n) {
        write_full(in_p[1], stdin_b, stdin_n);
    }
    close(in_p[1]);
    pump(wfd, out_p[0], err_p[0]);
    close(out_p[0]); close(err_p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int32_t code = WIFEXITED(st) ? (int32_t)WEXITSTATUS(st)
                 : WIFSIGNALED(st) ? -(int32_t)WTERMSIG(st) : -99;
    send_frame(wfd, RESP_EXIT, NULL, 0);
    write_full(wfd, &code, 4);
    goto done;
bad2:;
    /* best-effort error exit */
    send_frame(wfd, RESP_EXIT, NULL, 0);
    {
        int32_t code = 125;
        write_full(wfd, &code, 4);
    }
    goto done;
bad:
    send_frame(wfd, RESP_EXIT, NULL, 0);
    {
        int32_t code = 125;
        write_full(wfd, &code, 4);
    }
done:
    if (argv) { for (i = 0; i < argc; i++) free(argv[i]); free(argv); }
    if (envp) { for (i = 0; i < envc; i++) free(envp[i]); free(envp); }
    free(cwd);
    free(stdin_b);
}

static void handle_conn(int cfd, int wfd) {
    uint8_t op;
    if (read_full(cfd, &op, 1) != 1) return;
    if (op == PROTO_PING) {
        write_full(wfd, &op, 1);
    } else if (op == PROTO_EXEC) {
        handle_exec(cfd, wfd);
    } else if (op == PROTO_SHUTDOWN) {
        /* ack then terminate the whole daemon (systemd restarts us) */
        write_full(wfd, &op, 1);
        _exit(0);
    }
}

/* ---------- vsock transport (fast host path over virtio-vsock) ----------
 * Guest listens on VMADDR_CID_ANY:<port>; the host CLI connects to
 * CID_HOST (2):<port> through the vhost-device-vsock UDS bridge.
 * Protocol v1, byte-identical to the unix/file transports. */
#define VSOCK_PORT 2225

static int make_vsock_listener(int port) {
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_vm a;
    memset(&a, 0, sizeof a);
    a.svm_family = AF_VSOCK;
    a.svm_cid = VMADDR_CID_ANY;
    a.svm_port = port;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* vsock worker loop: accept serially; each conn served inline (prefork
 * pool is for the unix fast path; vsock conns are one-per-exec). */
static void vsock_loop(int lfd) {
    FILE *df = fopen("/run/sprout/state.log", "a");
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (df) { fprintf(df, "vsock accept=%d\n", c); fflush(df); }
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_conn(c, c);
        if (df) { fprintf(df, "vsock conn done\n"); fflush(df); }
        close(c);
    }
    if (df) fclose(df);
    _exit(0);
}

/* ---------- file transport (host <-> guest via the hostfs share) ----------
 * The hostfs share cannot carry AF_UNIX to the host: a socket node created
 * inside the guest is only a placeholder file on the host (hostfs mknod),
 * so host connect() always gets ECONNREFUSED.  Requests are therefore
 * passed as files: the host writes req.<n> (EXEC request body, same wire
 * format as the socket op payload), the agent answers resp.<n> with the
 * same frame stream the socket path uses, then removes req.<n>.
 * Latency is polling-bound (DIR_POLL_MS); correctness first, the socket
 * path remains the in-guest fast path. */
#define DIR_POLL_MS 300

static void file_handle_req(const char *path, const char *reppath) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    FILE *in = fdopen(fd, "rb");
    if (!in) { close(fd); return; }
    /* read the request body into memory, then reuse the socket handler by
     * feeding it through socketpair: least code, one code path to trust. */
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz < 0 || sz > (32L << 20)) { fclose(in); return; }
    char *body = malloc(sz ? sz : 1);
    if (!body || (sz && fread(body, 1, sz, in) != (size_t)sz)) {
        free(body); fclose(in); return;
    }
    fclose(in);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { free(body); return; }
    pid_t w = fork();
    if (w < 0) { close(sv[0]); close(sv[1]); free(body); return; }
    if (w == 0) {
        close(sv[0]);
        if (write_full(sv[1], body, sz) < 0) _exit(1);
        close(sv[1]);
        _exit(0);
    }
    free(body);
    close(sv[1]);
    /* serve the fake connection; responses go to the reply file */
    int out = open(reppath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    handle_conn(sv[0], out >= 0 ? out : sv[0]);
    if (out >= 0) close(out);
    close(sv[0]);
    int st = 0;
    while (waitpid(w, &st, 0) < 0 && errno == EINTR) {}
    unlink(path);
}

static void file_transport_loop(const char *dir) {
    int tick = 0;
    for (;;) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            long youngest = -1;
            char ypath[512], rpath[512];
            ypath[0] = 0;
            while ((e = readdir(d))) {
                if (strncmp(e->d_name, "req.", 4)) continue;
                snprintf(ypath, sizeof ypath, "%s/%s", dir, e->d_name);
                struct stat stt;
                if (stat(ypath, &stt) == 0 &&
                    (youngest < 0 || stt.st_mtime < youngest)) {
                    youngest = stt.st_mtime;
                }
            }
            closedir(d);
            if (ypath[0]) {
                snprintf(rpath, sizeof rpath, "%s/resp.%s", dir,
                         strrchr(ypath, '.') + 1);
                file_handle_req(ypath, rpath);
                continue; /* drain immediately */
            }
        }
        if ((++tick % 20) == 0) {
            FILE *df = fopen("/run/sprout/agent-debug.log", "a");
            if (df) { fprintf(df, "poll tick %d\n", tick); fclose(df); }
        }
        usleep(DIR_POLL_MS * 1000);
    }
}

static int make_listener(void) {
    /* Prefer the unix socket (fast path); TCP is the v1 fallback. */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un u;
        memset(&u, 0, sizeof u);
        u.sun_family = AF_UNIX;
        strncpy(u.sun_path, SOCK_PATH, sizeof u.sun_path - 1);
        unlink(SOCK_PATH);
        if (bind(fd, (struct sockaddr *)&u, sizeof u) == 0 &&
            chmod(SOCK_PATH, 0700) == 0 && listen(fd, 64) == 0)
            return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)tcp_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    int workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 2) workers = 2;
    if (workers > 16) workers = 16;
    workers = 8; /* test default: enough for probe + real conn at once */
    if (argc > 1) workers = atoi(argv[1]);
    if (workers < 1) workers = 1;
    /* argv[2]: TCP port override (default 2225). Lets tests use a scratch
     * port so stray agents can never cross-talk with the test. */
    if (argc > 2) {
        int p = atoi(argv[2]);
        if (p > 0 && p < 65536) tcp_port = p;
    }

    int lfd = make_listener();
    if (lfd < 0) {
        perror("listen");
        FILE *df = fopen("/run/sprout/agent-debug.log", "a");
        if (df) { fprintf(df, "listen failed errno=%d\n", errno); fclose(df); }
        return 1;
    }
    {
        FILE *df = fopen("/run/sprout/agent-debug.log", "a");
        if (df) { fprintf(df, "listening pid=%d\n", getpid()); fclose(df); }
    }
    /* vsock: fast host path (no fs, no polling). One dedicated process;
     * its death never touches the unix pool. */
    {
        int vfd = make_vsock_listener(VSOCK_PORT);
        {
            FILE *df = fopen("/run/sprout/state.log", "a");
            if (df) {
                fprintf(df, "vsock listener fd=%d (%s)\n", vfd,
                        vfd >= 0 ? "ok" : strerror(errno));
                fclose(df);
            }
        }
        if (vfd >= 0) {
            pid_t v = fork();
            if (v == 0) {
                vsock_loop(vfd);
                _exit(0);
            }
            close(vfd);
        }
        /* vsock unavailable (no device / old kernel): file + unix still
         * carry everything. Degradation, not failure. */
    }
    /* file transport: one extra process polls the share dir (hostfs has
     * no usable inotify on this port) so the socket pool stays untouched */
    {
        pid_t f = fork();
        if (f == 0) {
            file_transport_loop("/run/sprout");
            _exit(0);
        }
    }
    /* prefork pool: no fork on the hot path, workers block in accept() */
    for (int i = 1; i < workers; i++) {
        pid_t p = fork();
        if (p == 0) break;
        if (p < 0) break;
    }
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Recv timeout: half-open probes (connect, idle, never send) must
         * not pin a prefork worker forever. 30s is generous for real use.
         * Test binaries run with worker=1, so readiness probes in scripts
         * must be single-shot (each probe pins the only worker). */
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle_conn(c, c);
        close(c);
    }
    return 0;
}
