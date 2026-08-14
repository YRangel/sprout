/* ==========================================================================
 * ADR-0020: System V shared-memory emulation for MIT-SHM (termux-x11 bridge)
 * ==========================================================================
 * llvmpipe's Vulkan xcb WSI presents pixels via XShm: client segments are
 * allocated with shmget(2) and attached by the X server through MIT-SHM.
 * Android has NO SysV IPC. termux's proot hides this by emulating the shm
 * family with the libandroid-shmem protocol:
 *
 *   shmget(key,size)  -> ASharedMemory region + registry entry, shmid
 *                        encodes OWNER: shmid = (sockid << 16) | counter
 *   owner listener:   abstract unix socket  "\0/dev/shm/%08x" (sockid hex),
 *                        accepts: [recv 4B shmid] -> [send 4B key] -> [SCM fd]
 *   attach (other process): connect to owner's socket (sockid = shmid >> 16),
 *                        handshake returns the ashmem fd; consumer mmaps it.
 *   named keys:       symlink "$TMPDIR/ashv_key_<key>" -> "<shmid>" (decimal)
 *
 * termux-x11's server (linked to libandroid-shmem) runs the consumer side of
 * that wire format when MIT-SHM Attach requests arrive. We run the owner
 * side in the guest so mesa's XShm pixmaps (and GL apps' shm images) become
 * server-resolvable: XShmAttach(shmid) -> /dev/shm/<sockid> -> ashmem fd.
 *
 * Kill-switch: SPROUT_SYSVIPC_OFF=1 (falls through to the real libc, which
 * hits the no-SysV Android failure surface as before).
 * ========================================================================== */
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/un.h>

#ifndef ASHMEM_NAME_LEN
#  define ASHMEM_NAME_LEN 256
#endif
#ifndef ASHMEM_SET_NAME
#  define ASHMEM_SET_NAME   _IOW(__ASHMEMIOC, 1, char[ASHMEM_NAME_LEN])
#  define ASHMEM_SET_SIZE   _IOW(__ASHMEMIOC, 3, size_t)
#  define ASHMEM_GET_SIZE   _IO(__ASHMEMIOC, 4)
#  define __ASHMEMIOC       0x77
#endif

#ifndef TEMP_FAILURE_RETRY
#  define TEMP_FAILURE_RETRY(expr) \
     (__extension__ ({ long _r; do { _r = (long)(expr); } while (_r == -1 && errno == EINTR); _r; }))
#endif

#define SP_SHM_MAX 64
#define SP_SHM_SOCKPATH "/dev/shm/%08x"
#define SP_SHM_MAXSEG (64 * 1024 * 1024)

typedef struct {
    key_t key;
    int id;
    int fd;
    void *addr;
    size_t size;
    int marked_rmid;
} sp_shm_t;

static sp_shm_t sp_shm_table[SP_SHM_MAX];
static size_t sp_shm_n;
static pthread_mutex_t sp_shm_mu = PTHREAD_MUTEX_INITIALIZER;
static int sp_shm_off = -1; /* 0=unknown */
static int sp_shm_sockfd = -1;
static int sp_shm_sockid;   /* upper 16 bits of shmid */
static int sp_shm_counter;  /* lower 16 bits (wrap at 0x7fff like proot) */
static pid_t sp_shm_pid;

static int sp_shm_eno(void) {
    if (sp_shm_off == -1)
        sp_shm_off = (getenv("SPROUT_SYSVIPC_OFF") != NULL);
    return sp_shm_off;
}

/* All I/O below goes straight through syscalls, NOT the interposer's own
 * wrappers: this code is meant to serve the shm emulation itself. musl and
 * glibc both accept syscall(SYS_*) uniformly here. */
static int sp_shm_open(const char *path, int flags, mode_t mode) {
#ifdef SYS_openat
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
#else
    return -2;
#endif
}
static long sp_shm_close_fd(int fd) { return syscall(SYS_close, fd); }

static int sp_shm_open_ashmem(const char *name, size_t size) {
    int fd = sp_shm_open("/dev/ashmem", O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    char nam[ASHMEM_NAME_LEN] = {0};
    strncpy(nam, name, sizeof(nam) - 1);
    if (ioctl(fd, ASHMEM_SET_NAME, nam) < 0
        || ioctl(fd, ASHMEM_SET_SIZE, size) < 0) {
        sp_shm_close_fd(fd);
        return -1;
    }
    return fd;
}

static void sp_shm_fork_reset(void) {
    pid_t me = (pid_t)syscall(SYS_getpid);
    if (sp_shm_pid == me) return;
    /* fresh process image state: drop inherited registry + listener */
    sp_shm_pid = me;
    sp_shm_n = 0;
    sp_shm_sockfd = -1;
    sp_shm_sockid = 0;
    sp_shm_counter = 0;
}

static int sp_shm_send_fd(int sock, int fd) {
    char byte = '!';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctrl;
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    memset(&ctrl, 0, sizeof(ctrl));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl.buf;
    mh.msg_controllen = sizeof(ctrl.buf);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return syscall(SYS_sendmsg, sock, &mh, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

static int sp_shm_recv_fd(int sock) {
    char byte;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctrl;
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    memset(&ctrl, 0, sizeof(ctrl));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl.buf;
    mh.msg_controllen = sizeof(ctrl.buf);
    ssize_t n = 0;
    do n = (ssize_t)syscall(SYS_recvmsg, sock, &mh, 0);
    while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS
        || c->cmsg_len < CMSG_LEN(sizeof(int)))
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

static int sp_shm_find(int shmid) {
    for (size_t i = 0; i < sp_shm_n; i++)
        if (sp_shm_table[i].id == shmid) return (int)i;
    return -1;
}

/* owner-side accept loop: [recv shmid] -> [write key] -> [SCM_RIGHTS fd] */
static void *sp_shm_acceptor(void *arg) {
    int listenfd = *((int *)arg);
    free(arg);
    for (;;) {
        int cs = TEMP_FAILURE_RETRY(
            (int)syscall(SYS_accept4, listenfd, NULL, NULL, SOCK_CLOEXEC));
        if (cs < 0) break;
        int shmid;
        ssize_t r;
        do r = (ssize_t)syscall(SYS_recvfrom, cs, &shmid, sizeof(shmid),
                                0, NULL, NULL);
        while (r < 0 && errno == EINTR);
        if (r == (ssize_t)sizeof(shmid)) {
            pthread_mutex_lock(&sp_shm_mu);
            int idx = sp_shm_find(shmid);
            if (idx >= 0 && sp_shm_table[idx].fd >= 0) {
                key_t key = sp_shm_table[idx].key;
                int fd = sp_shm_table[idx].fd;
                if ((ssize_t)syscall(SYS_sendto, cs, &key, sizeof(key),
                                     MSG_NOSIGNAL, NULL, 0) == (ssize_t)sizeof(key))
                    (void)sp_shm_send_fd(cs, fd);
            }
            pthread_mutex_unlock(&sp_shm_mu);
        }
        (void)sp_shm_close_fd(cs);
    }
    return NULL;
}

/* bind abstract unix socket "\0/dev/shm/<sockid-hex>" + spawn acceptor.
 * sockid = low 16 bits of (pid + seq seq), matches libandroid-shmem. */
static int sp_shm_setup_listener(void) {
    for (int i = 0; i < 128; i++) {
        int sid = ((int)syscall(SYS_getpid) + i) & 0xFFFF;
        if (sid == 0) continue;
        int fd = (int)syscall(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        char name[64];
        snprintf(name, sizeof(name), SP_SHM_SOCKPATH, sid);
        memcpy(addr.sun_path, name + 0, 0); /* abstract: offset by 1 */
        size_t nl = strlen(name);
        memcpy(addr.sun_path + 1, name, nl + 1);
        socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                     + 1 + nl);
        if ((long)syscall(SYS_bind, fd, &addr, (unsigned long)alen) == 0) {
            if ((long)syscall(SYS_listen, fd, 16) == 0) {
                int *a = malloc(sizeof(int));
                if (!a) { (void)sp_shm_close_fd(fd); return -1; }
                *a = fd;
                pthread_t th;
                if (pthread_create(&th, NULL, sp_shm_acceptor, a) == 0) {
                    pthread_detach(th);
                    sp_shm_sockfd = fd;
                    sp_shm_sockid = sid;
                    return 0;
                }
                free(a);
            }
        }
        (void)sp_shm_close_fd(fd);
    }
    return -1;
}

/* consumer-side fd hydration from a remote owner. */
static int sp_shm_hydrate(int sockid, int shmid, key_t *key_out, size_t *size_out) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    char name[64];
    snprintf(name, sizeof(name), SP_SHM_SOCKPATH, sockid);
    size_t nl = strlen(name);
    memcpy(addr.sun_path + 1, name, nl + 1);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                 + 1 + nl);
    int s = (int)syscall(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if ((long)syscall(SYS_connect, s, &addr, (unsigned long)alen) < 0) {
        (void)sp_shm_close_fd(s); return -1;
    }
    if ((ssize_t)syscall(SYS_sendto, s, &shmid, sizeof(shmid), MSG_NOSIGNAL,
                         NULL, 0) != (ssize_t)sizeof(shmid)) {
        (void)sp_shm_close_fd(s); return -1;
    }
    key_t key;
    ssize_t r;
    do r = (ssize_t)syscall(SYS_recvfrom, s, &key, sizeof(key),
                            0, NULL, NULL);
    while (r < 0 && errno == EINTR);
    if (r != (ssize_t)sizeof(key)) { (void)sp_shm_close_fd(s); return -1; }
    int fd = sp_shm_recv_fd(s);
    (void)sp_shm_close_fd(s);
    if (fd < 0) return -1;
    long sz = ioctl(fd, ASHMEM_GET_SIZE, 0);
    if (sz <= 0) { close(fd); return -1; }
    *key_out = key;
    *size_out = (size_t)sz;
    return fd;
}

int shmget(key_t key, size_t size, int flags) {
    if (sp_shm_eno()) {
        errno = ENOSYS;
        return -1;
    }
    if (size == 0 || size > SP_SHM_MAXSEG) { errno = EINVAL; return -1; }
    sp_shm_fork_reset();
    pthread_mutex_lock(&sp_shm_mu);

    /* named-key reuse honour: existing segment with same key (local or
     * linked via the /tmp/ashv_key bridge) */
    if (key != IPC_PRIVATE) {
        char linkpath[128];
        snprintf(linkpath, sizeof(linkpath), "%sashv_key_%d", "/tmp/", (int)key);
        char buf[64];
        ssize_t l = (ssize_t)syscall(SYS_readlinkat, AT_FDCWD, linkpath,
                                     buf, sizeof(buf) - 1);
        if (l > 0) {
            buf[l] = 0;
            int shmid = atoi(buf);
            if (shmid != 0) {
                int idx = sp_shm_find(shmid);
                if (idx < 0 && (unsigned)(shmid >> 16) != (unsigned)sp_shm_sockid) {
                    key_t rk; size_t rs;
                    int fd = sp_shm_hydrate(shmid >> 16, shmid, &rk, &rs);
                    if (fd >= 0) {
                        if (sp_shm_n < SP_SHM_MAX) {
                            idx = (int)sp_shm_n++;
                            sp_shm_table[idx].key = rk;
                            sp_shm_table[idx].id = shmid;
                            sp_shm_table[idx].fd = fd;
                            sp_shm_table[idx].addr = NULL;
                            sp_shm_table[idx].size = rs;
                            sp_shm_table[idx].marked_rmid = 0;
                        } else close(fd);
                    }
                }
                if (idx >= 0) {
                    pthread_mutex_unlock(&sp_shm_mu);
                    return shmid;
                }
                (void)syscall(SYS_unlinkat, AT_FDCWD, linkpath, 0); /* stale */
            }
        }
    }

    if (sp_shm_sockfd < 0 && sp_shm_setup_listener() < 0) {
        pthread_mutex_unlock(&sp_shm_mu);
        errno = ENOMEM;
        return -1;
    }
    if (sp_shm_n >= SP_SHM_MAX) {
        pthread_mutex_unlock(&sp_shm_mu);
        errno = ENOSPC;
        return -1;
    }
    sp_shm_counter = (sp_shm_counter + 1) & 0x7fff;
    int shmid = sp_shm_sockid * 0x10000 + sp_shm_counter;
    char segname[64];
    snprintf(segname, sizeof(segname), SP_SHM_SOCKPATH "-%d",
             sp_shm_sockid, sp_shm_counter);
    size_t rsize = (size + 4095) & ~(size_t)4095;
    int fd = sp_shm_open_ashmem(segname, rsize);
    if (fd < 0) {
        pthread_mutex_unlock(&sp_shm_mu);
        errno = ENOMEM;
        return -1;
    }
    int idx = (int)sp_shm_n++;
    sp_shm_table[idx].key = key;
    sp_shm_table[idx].id = shmid;
    sp_shm_table[idx].fd = fd;
    sp_shm_table[idx].addr = NULL;
    sp_shm_table[idx].size = rsize;
    sp_shm_table[idx].marked_rmid = 0;
    if (key != IPC_PRIVATE) {
        char linkpath[128], numbuf[32];
        snprintf(linkpath, sizeof(linkpath), "%sashv_key_%d", "/tmp/", (int)key);
        snprintf(numbuf, sizeof(numbuf), "%d", shmid);
        (void)syscall(SYS_unlinkat, AT_FDCWD, linkpath, 0);
        (void)syscall(SYS_symlinkat, numbuf, AT_FDCWD, linkpath);
    }
    pthread_mutex_unlock(&sp_shm_mu);
    return shmid;
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
    if (sp_shm_eno()) { errno = ENOSYS; return (void *)-1; }
    sp_shm_fork_reset();
    pthread_mutex_lock(&sp_shm_mu);
    int idx = sp_shm_find(shmid);
    if (idx < 0 && (unsigned)(shmid >> 16) != (unsigned)sp_shm_sockid) {
        /* foreign segment: hydrate from its owner */
        key_t rk; size_t rs;
        int fd = sp_shm_hydrate(shmid >> 16, shmid, &rk, &rs);
        if (fd >= 0 && sp_shm_n < SP_SHM_MAX) {
            idx = (int)sp_shm_n++;
            sp_shm_table[idx].key = rk;
            sp_shm_table[idx].id = shmid;
            sp_shm_table[idx].fd = fd;
            sp_shm_table[idx].addr = NULL;
            sp_shm_table[idx].size = rs;
            sp_shm_table[idx].marked_rmid = 0;
        } else if (fd >= 0)
            (void)sp_shm_close_fd(fd);
    }
    if (idx < 0) {
        pthread_mutex_unlock(&sp_shm_mu);
        errno = EINVAL;
        return (void *)-1;
    }
    if (sp_shm_table[idx].addr == NULL) {
        int prot = PROT_READ | ((shmflg & SHM_RDONLY) ? 0 : PROT_WRITE);
        int mflags = MAP_SHARED | (shmaddr ? MAP_FIXED : 0);
        void *a = mmap((void *)shmaddr, sp_shm_table[idx].size, prot, mflags,
                       sp_shm_table[idx].fd, 0);
        if (a == MAP_FAILED)
            sp_shm_table[idx].addr = NULL;
        else
            sp_shm_table[idx].addr = a;
    }
    void *addr = sp_shm_table[idx].addr;
    pthread_mutex_unlock(&sp_shm_mu);
    if (!addr) { errno = ENOMEM; return (void *)-1; }
    return addr;
}

int shmdt(const void *shmaddr) {
    if (sp_shm_eno()) { errno = ENOSYS; return -1; }
    sp_shm_fork_reset();
    pthread_mutex_lock(&sp_shm_mu);
    for (size_t i = 0; i < sp_shm_n; i++) {
        if (sp_shm_table[i].addr == shmaddr) {
            munmap((void *)sp_shm_table[i].addr, sp_shm_table[i].size);
            sp_shm_table[i].addr = NULL;
            int drop = sp_shm_table[i].marked_rmid
                || (unsigned)(sp_shm_table[i].id >> 16) != (unsigned)sp_shm_sockid;
            if (drop) {
                if (sp_shm_table[i].fd >= 0)
                    (void)sp_shm_close_fd(sp_shm_table[i].fd);
                memmove(&sp_shm_table[i], &sp_shm_table[i + 1],
                        (sp_shm_n - i - 1) * sizeof(sp_shm_t));
                sp_shm_n--;
            }
            pthread_mutex_unlock(&sp_shm_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&sp_shm_mu);
    errno = EINVAL;
    return -1;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    if (sp_shm_eno()) { errno = ENOSYS; return -1; }
    sp_shm_fork_reset();
    pthread_mutex_lock(&sp_shm_mu);
    int idx = sp_shm_find(shmid);
    if (idx < 0 && (unsigned)(shmid >> 16) != (unsigned)sp_shm_sockid) {
        key_t rk; size_t rs;
        int fd = sp_shm_hydrate(shmid >> 16, shmid, &rk, &rs);
        if (fd >= 0 && sp_shm_n < SP_SHM_MAX) {
            idx = (int)sp_shm_n++;
            sp_shm_table[idx].key = rk;
            sp_shm_table[idx].id = shmid;
            sp_shm_table[idx].fd = fd;
            sp_shm_table[idx].addr = NULL;
            sp_shm_table[idx].size = rs;
            sp_shm_table[idx].marked_rmid = 0;
        } else if (fd >= 0)
            (void)sp_shm_close_fd(fd);
    }
    if (idx < 0) {
        pthread_mutex_unlock(&sp_shm_mu);
        errno = EINVAL;
        return -1;
    }
    if (cmd == IPC_RMID) {
        sp_shm_table[idx].marked_rmid = 1;
        if (sp_shm_table[idx].addr == NULL) {
            if (sp_shm_table[idx].fd >= 0)
                (void)sp_shm_close_fd(sp_shm_table[idx].fd);
            memmove(&sp_shm_table[idx], &sp_shm_table[idx + 1],
                    (sp_shm_n - (size_t)idx - 1) * sizeof(sp_shm_t));
            sp_shm_n--;
        }
        pthread_mutex_unlock(&sp_shm_mu);
        return 0;
    }
    if (cmd == IPC_STAT && buf) {
        struct shmid_ds d;
        memset(&d, 0, sizeof(d));
        size_t rsize = sp_shm_table[idx].size;
        int fd = sp_shm_table[idx].fd;
        key_t key = sp_shm_table[idx].key;
        pthread_mutex_unlock(&sp_shm_mu);
        d.shm_perm.mode = 0600;
        (void)fd;
        d.shm_segsz = rsize;
        d.shm_perm.uid = d.shm_perm.cuid = (uid_t)syscall(SYS_getuid);
        d.shm_perm.gid = d.shm_perm.cgid = (gid_t)syscall(SYS_getgid);
        d.shm_perm.__key = key;
        d.shm_lpid = (pid_t)syscall(SYS_getpid);
        d.shm_cpid = (pid_t)syscall(SYS_getpid);
        d.shm_nattch = 2;
        *buf = d;
        return 0;
    }
    if (cmd == IPC_SET) { pthread_mutex_unlock(&sp_shm_mu); return 0; }
    if (cmd == SHM_LOCK || cmd == SHM_UNLOCK) {
        pthread_mutex_unlock(&sp_shm_mu);
        return 0;
    }
    pthread_mutex_unlock(&sp_shm_mu);
    errno = EINVAL;
    return -1;
}
