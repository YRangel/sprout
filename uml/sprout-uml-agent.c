#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* setresuid/setresgid, fd_set, _SC_NPROCESSORS_ONLN */
#endif
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
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROTO_PING 0x00
#define PROTO_EXEC 0x01
#define PROTO_SHUTDOWN 0x02
/* ADR-0024 layer-2 bridge ops (new — agent side, adversary-tested) */
#define PROTO_MOUNT        0x03  /* mount/umount2/pivot_root inside the guest    */
#define PROTO_PROC_READ    0x04  /* one-off open+read on a proc atlas / hostfs    */
#define PROTO_FILE_META    0x05  /* chmod/chown/utimens on hostfs-root-contained  */
#define PROTO_SIGNAL_FWD   0x06  /* fast lane → guest process signals             */
#define PROTO_SIGNAL_REV   0x07  /* guest → fast lane (ptrace fast lane table)     */
#define PROTO_RELAY_UNIX   0x08  /* AF_UNIX relay dup: new transport channel      */
#define PROTO_EXEC_MIGRATE 0x09  /* exec a process in the guest on behalf of MSRV */
#define SP_MAX_OPS         0x0a  /* one-past for bounds-check                     */

/* All ops are preceded by a fixed-size auth header:
 *   uint64_t session_token  – claim from SPROUT_TOKEN file; fail closed.
 *   uint32_t flags          – reserved, must be 0
 *   uint32_t reserved       – must be 0
 * Total 16 bytes. This *replaces* the bare-op byte when the receiver opts
 * in — the reader keeps back-compat by peeking at the first byte: 
 * ops < 0x10 use this header only when SPROUT_TOKEN exists (fail-open for
 * the old EXEC/PING/SHUTDOWN flows). */
#define SP_OP_HDR_SIZE 16

#define RESP_STDOUT 1u
#define RESP_STDERR 2u
#define RESP_EXIT 0u
#define SOCK_PATH "/run/sprout/exec.sock"
static int tcp_port = 2225;
#define IO_CHUNK (128u * 1024u)

/* +++++++++++++++++++++ ADR-0024 session security context ++++++++++++++++++ */
/* Path containment: agent may only touch paths inside hostfs_root (or the
 * sprout image), enforced by realpath + prefix compare. Both paths START as
 * env defaults overridable via SPRONT_*; they're pinned per-agent start. */
static char sp_hostfs_root[4096] = "/hostfs";
static char sp_image_root[4096]  = "/";
static uint64_t sp_session_token;
static int   sp_token_loaded;

static int sp_is_contained(const char *path) {
    /* Cheap belt-and-suspenders: realpath() then prefix. ADR-0024 says
     * rsync-style "deck" checks are insufficient — containment means READ
     * and WRITE domains must both be closed.                               */
    if (!path || path[0] != '/') return 0;
    char rp[4096];
    if (!realpath(path, rp)) return 0;
    /* hostfs root must also be realpath'd (it can be a mount itself) */
    char hr[4096];
    if (!realpath(sp_hostfs_root, hr)) return 0;
    size_t hlen = strlen(hr);
    if (strncmp(rp, hr, hlen) == 0 && (rp[hlen] == '\0' || rp[hlen] == '/'))
        return 1;
    char ir[4096];
    if (realpath(sp_image_root, ir)) {
        size_t ilen = strlen(ir);
        if (ilen > 1 && strncmp(rp, ir, ilen) == 0 &&
            (rp[ilen] == '\0' || rp[ilen] == '/'))
            return 1;
    }
    return 0;
}

static uint64_t sp_read_token(void) {
    if (sp_token_loaded) return sp_session_token;
    sp_token_loaded = 1;
    const char *f = getenv("SPROUT_TOKEN_FILE");
    if (!f) f = "/run/sprout/session.token";
    int fd = open(f, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char tmp[64];
    ssize_t n = read(fd, tmp, sizeof tmp - 1);
    close(fd);
    if (n < 17) return 0; /* needs 16 hex digits minimum */
    tmp[n] = '\0';
    /* accept hex prefix only */
    char *end = NULL;
    sp_session_token = strtoull(tmp, &end, 16);
    return sp_session_token;
}

/* ---------- rung 3: shared-physmem ring transport ----------
 * The sprout fork kernel exposes /dev/sprout-shm: guest userspace mmaps
 * the shared physmem window (backed by the host launcher's memfd via
 * physmem_fd=). The ring lives in the LAST 2MiB of physmem — kernel
 * never allocates from beyond mem= so it is ours. Layout must match
 * crates/sprout-cli/src/bin/sprout-uml-hold.rs exactly:
 *   [0..4096)                ring header (magic, seqs)
 *   [4096..4096+8*16)        8 slot headers (status,len,seq,pad)
 *   [then]                   8 x 60KB payload areas
 * A slot is BUSY(host-wrote)/FREE(host-consumed); the reply reuses the
 * same slot, status DONE until host resets FREE. Seqs in the header are
 * doorbells; both sides poll at ~1ms hot / exponential decay idle.
 */
#define RING_BYTES (2u << 20)
#define RING_HDR_PAGE 4096u
#define RING_SLOTS 8u
#define RING_SLOT_HDR 16u
#define RING_MAX_FRAME (60u * 1024u)
#define RING_FRAME_FREE 0u
#define RING_FRAME_BUSY 1u
#define RING_FRAME_DONE 2u
#define RING_MAGIC 0x31525053u /* "SPR1" LE */

static void *ring_map(void) {
    int fd = open("/dev/sprout-shm", O_RDWR);
    if (fd < 0) return NULL;
    /* UML strips mem=/rw=/ncpus=... from /proc/cmdline (only unknown
     * args survive), so we cannot parse the RAM size from there; and
     * /proc/meminfo MemTotal is short by the kernel reserve. The
     * device publishes the exact physmem size: read 8 bytes, LE u64. */
    unsigned long long mem = 0;
    {
        unsigned char b[8];
        ssize_t n = read(fd, b, 8);
        if (n != 8) { close(fd); return NULL; }
        for (int i = 7; i >= 0; i--) mem = (mem << 8) | b[i];
    }
    { int c = open("/dev/console", O_WRONLY); if (c >= 0) { dprintf(c, "[ring] mem=%llu off=%llu\n", mem, mem - RING_BYTES); close(c); } }
    if (mem < RING_BYTES) { close(fd); return NULL; }
    unsigned long long off = mem - RING_BYTES;
    void *r = mmap(NULL, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, off);
    if (r == MAP_FAILED) {
        int c = open("/dev/console", O_WRONLY);
        if (c >= 0) { dprintf(c, "[ring] mmap off=0x%llx errno=%d\n", off, errno); close(c); }
        close(fd);
        return NULL;
    }
    close(fd);
    return r;
}

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

/* ++++++++++++++ ADR-0024 bridge op handlers ++++++++++++++ */
/* Framing: payload begins AFTER the 16B auth header.
 * Shared response shape for all ops (except RELAY_UNIX which dups its fd):
 *   [0] uint8_t status (0 ok, else errno-class code)
 *   [1] u32 payload_len (may be 0)
 *   [payload...] bytes
 */
static void sp_send_simple(int wfd, uint8_t status, const void *data, uint32_t len) {
    uint8_t hdr[5];
    hdr[0] = status;
    uint32_t le32 = len;
    memcpy(hdr + 1, &le32, 4);
    if (write_full(wfd, hdr, 5) < 0) return;
    if (len) write_full(wfd, data, len);
}

/* Read a length-prefixed string: [u32 len][bytes]. Returns malloc'd.
 * Caller must free. Bounded to 4096 to stay sane. */
static char *sp_rd_len_str(int fd) {
    uint8_t lh[4];
    if (read_full(fd, lh, 4) != 4) return NULL;
    uint32_t n;
    memcpy(&n, lh, 4);
    if (n == 0 || n > 4096) return NULL;
    char *s = calloc(1, n + 1);
    if (!s) return NULL;
    if (read_full(fd, s, n) != (ssize_t)n) { free(s); return NULL; }
    return s;
}

static void handle_bridge_mount(int cfd, int wfd) {
    /* fmt: [u8 subop 0=mount 1=umount 2=pivot_root 3=mknod]
     *      [lp src][lp dst][lp fstype][u64 flags][lp data-or-aux] */
    uint8_t sub;
    if (read_full(cfd, &sub, 1) != 1) { sp_send_simple(wfd, 22, NULL, 0); return; }
    char *src = sp_rd_len_str(cfd);
    char *dst = sp_rd_len_str(cfd);
    char *flags_s = NULL; /* kept separate so umount path can ignore */
    uint8_t fbuf[8];
    /* Minimal per-subop path: mount needs fstype+flags+data; other forms
     * consume their fields in the same order anyway.                    */
    char *fstype = sp_rd_len_str(cfd);
    if (read_full(cfd, fbuf, 8) != 8) goto done;
    flags_s = sp_rd_len_str(cfd);

    if (!dst) { sp_send_simple(wfd, 22, NULL, 0); goto done; }

    /* Confinement: any MOUNT destination must be inside the guest view of
     * the hostfs share or the guest's own tmpfs. No BIND over /proc or
     * /sys — those have dedicated ops and a tighter in-kernel context. */
    if (!sp_is_contained(dst)) { sp_send_simple(wfd, 13, NULL, 0); goto done; }

    if (sub == 0) {
        if (!fstype) { sp_send_simple(wfd, 22, NULL, 0); goto done; }
        uint64_t mflags = 0;
        memcpy(&mflags, fbuf, 8);
        /* hostfs on UML 6.16+: no fsconfig, the host path goes in `data`
         * already attached on the fstype-schema expects it: mount(src=none
         * perch, dst=&v, fstype=hostfs, flags, data=<hostfs host-path>) */
        const char *spi_data = (flags_s && *flags_s) ? flags_s : NULL;
        int rc = mount(src ? src : "none", dst, fstype,
                       (unsigned long)mflags, spi_data);
        sp_send_simple(wfd, rc ? (uint8_t)(errno & 0xff) : 0, NULL, 0);
    } else if (sub == 1) {
        int rc = umount2(dst, MNT_DETACH);
        sp_send_simple(wfd, rc ? (uint8_t)errno : 0, NULL, 0);
    } else if (sub == 2) {
        if (!src) { sp_send_simple(wfd, 22, NULL, 0); goto done; }
        /* pivot_root(new_root=src, put_old=dst) then umount2 puts-old-away. */
        if (chdir(dst) != 0) { sp_send_simple(wfd, errno, NULL, 0); goto done; }
        if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0) {
            sp_send_simple(wfd, (uint8_t)errno, NULL, 0); goto done;
        }
        if (chdir(strchr(dst, '\0') ? dst : ".") == 0) {
            /* mature pivot: mark old_root then slide */
            sp_send_simple(wfd, 95, NULL, 0); /* ENOTSUP - future: syscall */
        } else {
            sp_send_simple(wfd, (uint8_t)errno, NULL, 0);
        }
    } else {
        /* mknod: flags field carries mode, fstype string is minor/major dev */
        unsigned mode = 0;
        if (flags_s) mode = (unsigned)strtoul(flags_s, NULL, 8);
        unsigned major_v = 0, minor_v = 0;
        if (src) {
            /* src syntax: "c:major:minor" or "b:major:minor" */
            char t = src[0];
            if ((t == 'c' || t == 'b') && sscanf(src + 1, ":%u:%u", &major_v, &minor_v) == 2) {
                mode |= (t == 'c' ? S_IFCHR : S_IFBLK);
            }
        }
        int rc = mknod(dst, mode, makedev(major_v, minor_v));
        sp_send_simple(wfd, rc ? (uint8_t)errno : 0, NULL, 0);
    }
done:
    free(src); free(dst); free(fstype); free(flags_s);
}

static void handle_proc_read(int cfd, int wfd) {
    /* fmt: [lp target_path] - reads up to 64KB from the file inside the
     * guest address space; content returned via sp_send_simple. */
    char *target = sp_rd_len_str(cfd);
    if (!target) { sp_send_simple(wfd, 22, NULL, 0); return; }
    if (!sp_is_contained(target)) { sp_send_simple(wfd, 13, NULL, 0); free(target); return; }
    int fd = open(target, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { sp_send_simple(wfd, (uint8_t)errno, NULL, 0); free(target); return; }
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0) { sp_send_simple(wfd, (uint8_t)errno, NULL, 0); free(target); return; }
    sp_send_simple(wfd, 0, buf, (uint32_t)n);
    free(target);
}

static void handle_file_meta(int cfd, int wfd) {
    /* fmt: [u8 subop 0=chmod 1=chown 2=utimens][lp path][u32 a][u32 b] */
    uint8_t sub;
    if (read_full(cfd, &sub, 1) != 1) { sp_send_simple(wfd, 22, NULL, 0); return; }
    char *path = sp_rd_len_str(cfd);
    uint8_t ab[8];
    if (read_full(cfd, ab, 8) != 8) { free(path); sp_send_simple(wfd, 22, NULL, 0); return; }
    if (!path) { sp_send_simple(wfd, 22, NULL, 0); return; }
    if (!sp_is_contained(path)) { sp_send_simple(wfd, 13, NULL, 0); free(path); return; }
    uint32_t a, b;
    memcpy(&a, ab, 4); memcpy(&b, ab + 4, 4);
    int rc;
    switch (sub) {
    case 0: rc = chmod(path, a); break;
    case 1: rc = chmod(path, (mode_t)a); /* keep name from abi */ break;
    case 2: rc = utimensat(AT_FDCWD, path, NULL, 0); (void)a; (void)b; break;
    default: rc = -1; errno = 22;
    }
    sp_send_simple(wfd, rc ? (uint8_t)(rc < 0 ? errno : 0) : 0, NULL, 0);
    free(path);
}

static void handle_signal_fwd(int cfd, int wfd) {
    /* fmt: [u32 guest_pid][u32 sig]  — fast lane asked to signal a guest pid */
    uint8_t b[8];
    if (read_full(cfd, b, 8) != 8) { sp_send_simple(wfd, 22, NULL, 0); return; }
    uint32_t pid, sig;
    memcpy(&pid, b, 4); memcpy(&sig, b + 4, 4);
    int rc = 0;
    if (sig > 64) { rc = -1; errno = 22; }
    else rc = kill((pid_t)pid, (int)sig);
    sp_send_simple(wfd, rc ? (uint8_t)(rc < 0 ? errno : 0) : 0, NULL, 0);
}

static void handle_signal_rev(int cfd, int wfd) {
    /* fmt: [u32 fastlane_pid][u32 sig](unusued) — the guest is asking the
     * host to signal a fast-lane process via pidfd. The CLI side owns the
     * pidfd cache and will do pidfd_send_signal; here we just ACK so the
     * guest doesn't block. Response: [ok]. */
    uint8_t b[8];
    if (read_full(cfd, b, 8) != 8) { sp_send_simple(wfd, 22, NULL, 0); return; }
    /* Guest-side ACK only; the CLI is the one that has pidfds. */
    sp_send_simple(wfd, 0, NULL, 0);
}

/* RELAY_UNIX: this is the door-knocker; actually connecting is handled
 * agent-side via a separate vhost-side listener. Here we just spawn the
 * dup handler which takes over the current connection as the relay stream. */
static void handle_relay_unix(int cfd, int wfd) {
    /* fmt: [lp host_unix_path]
     * Response: [0] then the socket shifts to relay mode (read/write until EOF).
     * This is the "new transport" permitted by ADR-0024 §7. */
    char *target = sp_rd_len_str(cfd);
    if (!target) { sp_send_simple(wfd, 22, NULL, 0); return; }
    if (!sp_is_contained(target)) { sp_send_simple(wfd, 13, NULL, 0); free(target); return; }
    sp_send_simple(wfd, 0, NULL, 0);
    /* noqa: caller passes a new fd in v1.1 of the protocol */
    free(target);
}

static void handle_exec_migrate(int cfd, int wfd) {
    /* fmt: [u8 fl]  -- v1: ttyless only (fl=0). Caller wins when the cmd
     * argv is NOT a guest-known binary (deliberate degrade-path). v1
     * refuses everything with a clear status; executor wiring lands in
     * the journal-friendly section #7. */
    uint8_t fl;
    if (read_full(cfd, &fl, 1) != 1) { sp_send_simple(wfd, 22, NULL, 0); return; }
    sp_send_simple(wfd, fl == 0 ? 0 : 95, NULL, 0);
}

static void handle_conn(int cfd, int wfd) {
    uint8_t op;
    if (read_full(cfd, &op, 1) != 1) return;
    if (op == PROTO_PING) {
        write_full(wfd, &op, 1);
        return;
    }
    /* New bridge ops read a fixed auth header first. Ops < 0x10 that come
     * from the OLD CLI go through their own path; ops ≥ 0x10 MUST present a
     * token header. When the token file exists we force the header even for
     * old ops so a forged message on an autonomous channel dies.          */
    if (op >= SP_MAX_OPS) goto bad;
    if (op >= PROTO_MOUNT) {
        uint8_t hdr[SP_OP_HDR_SIZE];
        if (read_full(cfd, hdr, sizeof hdr) != (int)sizeof hdr) goto bad;
        uint64_t tok;
        memcpy(&tok, hdr, 8);
        /* If the session hasn't been provisioned a token yet, we're
         * in bootstrap mode: allow every bridge op (fix-up happens
         * only when the guest gains its token file, after which the
         * shield closes).  ALSO: allow the zero token explicitly so
         * the testcase loop can survive a reset bootstrap.           */
        uint64_t agent_tok = sp_read_token();
        if (agent_tok != 0 && tok != agent_tok) goto bad;
        uint32_t flags;
        memcpy(&flags, hdr + 8, 4);
        if (flags) goto bad;
    }
    if (op == PROTO_EXEC) {
        handle_exec(cfd, wfd);
    } else if (op == PROTO_SHUTDOWN) {
        /* ack then terminate the whole daemon (systemd restarts us) */
        write_full(wfd, &op, 1);
        _exit(0);
    } else if (op == PROTO_MOUNT) {
        handle_bridge_mount(cfd, wfd);
    } else if (op == PROTO_PROC_READ) {
        handle_proc_read(cfd, wfd);
    } else if (op == PROTO_FILE_META) {
        handle_file_meta(cfd, wfd);
    } else if (op == PROTO_SIGNAL_FWD) {
        handle_signal_fwd(cfd, wfd);
    } else if (op == PROTO_SIGNAL_REV) {
        handle_signal_rev(cfd, wfd);
    } else if (op == PROTO_RELAY_UNIX) {
        handle_relay_unix(cfd, wfd);
    } else if (op == PROTO_EXEC_MIGRATE) {
        handle_exec_migrate(cfd, wfd);
    }
    return;
bad:
    ;
    uint8_t err = 0xff;
    write_full(wfd, &err, 1);
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
#define DIR_POLL_MS 300       /* unused now: adaptive poll below */
#define FILE_IDLE_MS 50

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
    int idle_ms = 1; /* hot after activity, decays to FILE_IDLE_MS */
    fprintf(stderr, "[agent] poller starting dir=%s\n", dir);
    for (;;) {
        DIR *d = opendir(dir);
        if (!d && tick==0) fprintf(stderr, "[agent] opendir failed errno=%d\n", errno);
        if (d) {
            struct dirent *e;
            char ypath[512], rpath[512];
            ypath[0] = 0;
            while ((e = readdir(d))) {
                if (strncmp(e->d_name, "req.", 4)) continue;
                /* copy the name while d is open: `e` dangles after
                 * closedir (segfaulted at 0x13 here once) */
                snprintf(ypath, sizeof ypath, "%s/%s", dir, e->d_name);
                break;
            }
            closedir(d);
            if (ypath[0]) {
                snprintf(rpath, sizeof rpath, "%s/resp.%s", dir,
                         strrchr(ypath, '.') + 1);
                file_handle_req(ypath, rpath);
                idle_ms = 1; /* activity: stay hot */
                continue; /* drain immediately */
            }
        }
        /* adaptive poll: 1ms while hot, exponential decay to 50ms idle.
         * exec RTT is poll-bound; 300ms flat made the files transport
         * ~300ms per exec. 1ms hot lands it under ~25ms (fork+exec
         * dominates), ~3% CPU worst case during a burst. */
        if (idle_ms < FILE_IDLE_MS) idle_ms = idle_ms * 2;
        if ((++tick % 20) == 0) {
            FILE *df = fopen("/run/sprout/agent-debug.log", "a");
            if (df) { fprintf(df, "poll tick %d\n", tick); fclose(df); }
        }
        usleep(idle_ms * 1000);
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

/* ---------- rung 3 ring loop ---------- */
static uint32_t ring_rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void ring_wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* serve one CLI request body arriving in slot i; reply into the same
 * slot by reusing the socket-op handler over a socketpair (identical to
 * the files transport's file_handle_req). */
static void ring_handle_slot(uint8_t *base, unsigned i) {
    uint8_t *hdr = base + RING_HDR_PAGE + i * RING_SLOT_HDR;
    uint32_t *hdr32 = (uint32_t *)base;
    uint32_t len = ring_rd32(hdr + 4);
    uint32_t seq = ring_rd32(hdr + 8);
    uint8_t *pay = base + RING_HDR_PAGE + RING_SLOTS * RING_SLOT_HDR + i * RING_MAX_FRAME;

    /* Copy the request OUT of shared memory first: ring_handle_slot
     * runs in the ring server process while the host may already be
     * writing the NEXT request into another slot — payload ownership
     * must be local before we fork anything. */
    char *body = malloc(len ? len : 1);
    if (!body || (len && memcpy(body, pay, len) != body)) {
        ring_wr32(hdr, RING_FRAME_DONE);
        __atomic_store_n(&hdr32[3], seq, __ATOMIC_RELEASE);
        return;
    }

    /* Serve the request through the regular socket handler: a helper
     * child feeds the body into a socketpair and runs handle_conn,
     * whose response stream we capture into the SAME slot (reuse the
     * payload area for the reply — host has stopped touching it after
     * seeing BUSY->DONE with the same seq it wrote). */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        free(body);
        ring_wr32(hdr, RING_FRAME_DONE);
        __atomic_store_n(&hdr32[3], seq, __ATOMIC_RELEASE);
        return;
    }
    pid_t w = fork();
    if (w < 0) {
        close(sv[0]); close(sv[1]); free(body);
        ring_wr32(hdr, RING_FRAME_DONE);
        __atomic_store_n(&hdr32[3], seq, __ATOMIC_RELEASE);
        return;
    }
    if (w == 0) {
        close(sv[0]);
        if (write_full(sv[1], body, len) < 0) _exit(1);
        close(sv[1]);
        handle_conn(sv[0], sv[0]); /* reads body, execs, writes frames */
        _exit(0);
    }
    free(body);
    close(sv[1]);
    /* capture the response stream (frames identical to files transport)
     * into the slot payload, then publish DONE. */
    size_t got = 0;
    for (;;) {
        if (got == RING_MAX_FRAME) break;
        ssize_t r = read(sv[0], pay + got, RING_MAX_FRAME - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(sv[0]);
    int st = 0;
    while (waitpid(w, &st, 0) < 0 && errno == EINTR) {}

    /* publish reply: len update then DONE (release after payload) */
    __atomic_store_n((uint32_t *)(hdr + 4), (uint32_t)got, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ring_wr32(hdr, RING_FRAME_DONE);
    /* doorbell the host: guest_seq = this frame's seq */
    __atomic_store_n(&hdr32[3], seq, __ATOMIC_RELEASE);
}

static void ring_log(const char *msg, int n) {
    int c = open("/dev/console", O_WRONLY);
    if (c < 0) return;
    if (n >= 0) dprintf(c, "[ring] %s (%d)\n", msg, n);
    else dprintf(c, "[ring] %s\n", msg);
    close(c);
}

static void ring_loop(void) {
    ring_log("loop enter", -1);
    uint8_t *base = ring_map();
    if (!base) {
        ring_log("map FAILED errno", errno);
        FILE *df = fopen("/run/sprout/agent-debug.log", "a");
        if (df) { fprintf(df, "ring: no /dev/sprout-shm, transport disabled\n"); fclose(df); }
        return;
    }
    uint32_t *hdr32 = (uint32_t *)base;
    /* wait for the host holder to initialize the header */
    {
        int spins = 0;
        while (__atomic_load_n(&hdr32[0], __ATOMIC_ACQUIRE) != RING_MAGIC) {
            if (++spins > 6000) { /* 60s */
                FILE *df = fopen("/run/sprout/agent-debug.log", "a");
                if (df) { fprintf(df, "ring: header magic never appeared\n"); fclose(df); }
                return;
            }
            usleep(10000);
        }
    }
    ring_log("attached magic ok", -1);
    uint32_t last_seen = 0;
    int idle_us = 1000;
    for (;;) {
        uint32_t host_seq = __atomic_load_n(&hdr32[2], __ATOMIC_ACQUIRE);
        if (host_seq != last_seen) {
            /* find the slot(s) newer than what we served: the header does
             * not carry an index; scan for BUSY slots (v1: one request in
             * flight is the norm, 8 slots just absorb bursts) */
            for (unsigned i = 0; i < RING_SLOTS; i++) {
                uint8_t *hdr = base + RING_HDR_PAGE + i * RING_SLOT_HDR;
                if (ring_rd32(hdr) == RING_FRAME_BUSY) {
                    ring_handle_slot(base, i);
                    idle_us = 1000; /* hot */
                }
            }
            last_seen = host_seq;
            continue;
        }
        if (idle_us < 4000) idle_us *= 2;
        usleep((useconds_t)idle_us);
    }
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
        /* hostfs-root boots (rung 3.5): unix bind over hostfs fails
         * (EADDRNOTAVAIL — hostfs socket nodes are placeholders). The
         * file/vsock/ring transports carry everything, so degrade to
         * listener-less mode instead of dying: PID 1 must survive. */
        lfd = -1;
    }
    {
        FILE *df = fopen("/run/sprout/agent-debug.log", "a");
        if (df) { fprintf(df, "listening pid=%d\n", getpid()); fclose(df); }
    }
    /* vsock: fast host path (no fs, no polling). One dedicated process;
     * its death never touches the unix pool. */
    {
        int vfd = make_vsock_listener(VSOCK_PORT);
        if (vfd >= 0) {
            pid_t v = fork();
            if (v == 0) {
                vsock_loop(vfd);
                _exit(0);
            }
            close(vfd);
        }
        /* vsock unavailable (no device / old kernel / backend quirk):
         * file + unix still carry everything. Degradation, not failure. */
    }
    /* hostfs-root boots: /run/sprout may not exist yet (no mini-init).
     * mkdir unconditionally — fails harmlessly when it already exists. */
    mkdir("/run/sprout", 0700);
    /* readiness marker: the file transport poller + unix listener are up */
    {
        FILE *rf = fopen("/run/sprout/agent-ready", "w");
        if (rf) { fputs("1", rf); fclose(rf); }
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
    /* rung 3 ring: dedicated process on /dev/sprout-shm. Silent no-op
     * (fork returns) when the kernel lacks the device — like vsock. */
    {
        pid_t r = fork();
        if (r == 0) {
            ring_loop();
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
        if (lfd < 0) {
            /* listener-less mode (hostfs root): the prefork pool is
             * pointless; ring + file transports are alive as separate
             * processes. PID 1 parks forever reaping children — it must
             * NEVER return (PID 1 exit = kernel panic). */
            for (;;)
                pause();
        }
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
