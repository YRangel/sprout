/* sprout-sysvipc — SysV IPC emulation for box64-vs-sprout guests.
 *
 * BACKGROUND
 * Android stock GKI vendors ship CONFIG_SYSVIPC=n (AOSP GKI defconfig),
 * so semget(2)/shmget(2) return ENOSYS natively. proot-distro guests
 * hosted on such kernels therefore segfault apps that require SysV IPC —
 * the mascot case being Steam's tier0.so startup semaphore.
 *
 * DESIGN
 * LD_PRELOAD DSO defining interposable libc wrappers (semget/semctl/
 * semop/shmget/shmctl/shmat/shmdt). Each SysV object lives as a tiny
 * payload in $SPROUT_SYSVIPC_DIR (default /tmp/sprout-sysvipc):
 *
 *   <dir>/sem-%08x   — 48-byte sv_hdr + nsems*8B cells (u32 val + u32 pad)
 *   <dir>/shm-%08x   — 48-byte sv_hdr + size bytes payload
 *   <dir>/_cnt       — uint32 private-id counter per itype
 *
 * IDs are (itype<<28 | key&lt;0xFFFFFFF) so id→object lookup needs no shared
 * registry — a guest-side path that EVERY sprout-wrapped emulated process
 * in the SAME container resolves identically.
 *
 * BACKEND: fd + pread/pwrite + flock(2) — NOT mmap. Empirical box32
 * v0.4.3-on-arm64 behavior: mmap(2) of a small host file from an i386
 * guest returns a VIEW SHIFTED +8 relative to x86_64's view of the same
 * backing file (page-offset-units quirk). Absolute file offsets (pread /
 * pwrite) are view-independent on BOTH guest ABIs, and pread of the
 * header heap shows identical bytes on either side. Cross-ABI (amd64
 * supervisor × i386 client — steam's own pairing) therefore goes through
 * syscalls only. Critical sections use flock(2) LOCK_EX on the object's
 * fd; decrement-block loops out-wait via unlock+poll+backoff.
 *
 * SHM attach (shmat) STILL keeps MAP_SHARED mmap of the payload: the
 * API is defined by returning a live mapped pointer. That wariant stays
 * same-arch-reliable only (cross-ABI ipc keys for shm = documented v1
 * gap, no steam dependency).
 *
 * LIMITS (documented):
 * - SEM_UNDO, MSG_*, SHMLBA fine-grained memory barriers ignored v1.
 * - struct layouts are NEUTRAL-ABI (compact 4-byte words); pid fits.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void sv_dbg(const char *fmt, ...) {
    if (!getenv("SPROUT_SYSVIPC_DEBUG")) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)write(2, buf, strlen(buf));
}

#define SV_MAGIC   0x5356u /* "SV" */

typedef struct {
    /* NEUTRAL-ABI: every field 4 bytes; identical little-endian bytes under
     * BOTH i386 and x86_64 gcc; read/written via pread/pwrite of absolute
     * file offsets so guest-ABI mapping quirks cannot skew the view. */
    uint32_t magic;
    uint32_t version;
    uint32_t itype;    /* 1=sem 2=shm */
    uint32_t nsems;    /* sem only */
    int32_t  key;
    int32_t  id;
    int32_t  creator;
    int32_t  destroy;  /* IPC_RMID requested */
    uint32_t nattch;   /* shm only */
    uint32_t size;     /* shm payload bytes (0 for sem) */
    uint32_t pad_a;
    uint32_t pad_b;
} sv_hdr;
#define SV_HDR_SIZE ((uint32_t)sizeof(sv_hdr))
#define SV_SEM_STRIDE 8u /* u32 val + u32 pad per cell */

/* ---------- backing-store paths ---------- */

static const char *sv_dir(void) {
    const char *d = getenv("SPROUT_SYSVIPC_DIR");
    return d && *d ? d : "/tmp/sprout-sysvipc";
}
static void sv_dir_ensure(void) {
    char b[512];
    snprintf(b, sizeof(b), "%s", sv_dir());
    (void)mkdir(b, 0777);
}
static void sv_path(char *out, size_t outsz, uint32_t itype, key_t key) {
    snprintf(out, outsz, "%s/%s-%08x", sv_dir(), itype == 1 ? "sem" : "shm",
             (unsigned)((uint32_t)key & 0x0FFFFFFFu));
}
static int32_t sv_make_id(uint32_t itype, int32_t key) {
    return (int32_t)((itype << 28) | ((uint32_t)key & 0x0FFFFFFFu));
}

/* robust full-write/full-read helpers (partial io on funky tmpfs)
 * NOTE: lseek+read/write instead of pread/pwrite — box32's wrapped 32-bit
 * libc in v0.4.3 lacks pwrite@GLIBC_2.1 and the PLT fails to resolve. */
static ssize_t pwrite_retry(int fd, const void *buf, size_t n, off_t off) {
    size_t done = 0;
    while (done < n) {
        if (lseek(fd, off + (off_t)done, SEEK_SET) < 0) return -1;
        ssize_t r = write(fd, (const char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return (ssize_t)done;
}
static ssize_t sv_pread_full(int fd, void *buf, size_t n, off_t off) {
    size_t done = 0;
    while (done < n) {
        if (lseek(fd, off + (off_t)done, SEEK_SET) < 0) return -1;
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* private-/next-id allocation: applock-free two-op file (alurant is a
 * single 4-byte read+write; cross-process ordering via flock). */
static int sv_private_id_alloc(uint32_t itype, int32_t *out_id) {
    sv_dir_ensure();
    char f[512];
    snprintf(f, sizeof(f), "%s/_cnt", sv_dir());
    int fd = open(f, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return -1;
    (void)flock(fd, LOCK_EX);
    struct stat st;
    (void)fstat(fd, &st);
    uint32_t base = 0x20000000u;
    if (st.st_size < 8) { (void)ftruncate(fd, 8); uint32_t z = base; (void)lseek(fd, 0, SEEK_SET); (void)write(fd, &z, 4); (void)write(fd, &z, 4); }
    off_t pos = itype == 1 ? 0 : 4;
    uint32_t v = base;
    (void)lseek(fd, pos, SEEK_SET);
    (void)read(fd, &v, 4);
    v++;
    (void)lseek(fd, pos, SEEK_SET);
    (void)write(fd, &v, 4);
    (void)flock(fd, LOCK_UN);
    close(fd);
    *out_id = (int32_t)((itype << 28) | v); /* high nibble marks itype */
    return 0;
}
/* allocate a fresh backing file name for a PRIVATE key (key is set to the
 * id'd low bits — path uses the SAME masked key so any peer holding the id
 * resolves the same file). */
static int sv_private_alloc(uint32_t itype, key_t *out_key) {
    int32_t id;
    if (sv_private_id_alloc(itype, &id) != 0) return -1;
    *out_key = (key_t)((uint32_t)id & 0x0FFFFFFFu);
    return 0;
}

/* ---------- object open/create (fd-based) ---------- */

typedef struct {
    int     fd;      /* backing file fd, kept open; flock for crit section */
    int32_t id;
    key_t   key;
    uint32_t itype;
    int     created; /* 1 iff we just created (owns initial values) */
} sv_obj;

/* open (or create) the file for (itype, key).
 *   create_flg: O_CREAT semantics; nsems/size used only on first create.
 * Writes a valid header on creation only. Returns 0 or -errno. */
static int sv_obj_open(sv_obj *o, uint32_t itype, key_t key,
                       int create_flg, unsigned nsems, unsigned shm_sz,
                       int *created_out) {
    char p[512];
    sv_dir_ensure(); /* /tmp/sprout-sysvipc lazily on every open */
    sv_path(p, sizeof(p), itype, key);

    int flags = O_RDWR | O_CLOEXEC;
    if (create_flg) flags |= O_CREAT | O_EXCL;
    int fd = open(p, flags, 0666);
    int created = 0;
    if (fd < 0 && create_flg && errno == EEXIST) {
        fd = open(p, O_RDWR | O_CLOEXEC, 0666);
        created = 0;
    } else if (fd >= 0 && create_flg) {
        created = 1;
    } else if (fd >= 0) {
        created = 0;
    }
    if (fd < 0) {
        if (errno == ENOENT && !create_flg) { errno = EIDRM; }
        return -1;
    }

    (void)flock(fd, LOCK_EX);
    struct stat st;
    (void)fstat(fd, &st);

    if (created || st.st_size == 0) {
        sv_hdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic   = SV_MAGIC;
        hdr.version = 2;
        hdr.itype   = itype;
        hdr.nsems   = itype == 1 ? nsems : 0;
        hdr.key     = (int32_t)key;
        hdr.id      = sv_make_id(itype, (int32_t)key);
        hdr.creator = (int32_t)getpid();
        hdr.nattch  = 0;
        hdr.size    = itype == 2 ? shm_sz : ((uint32_t)nsems);
        hdr.pad_a   = 0;
        hdr.pad_b   = 0;
        uint32_t filelen = SV_HDR_SIZE + (itype == 1 ? nsems * SV_SEM_STRIDE : shm_sz);
        (void)ftruncate(fd, (off_t)filelen);
        (void)pwrite_retry(fd, &hdr, SV_HDR_SIZE, 0);
        /* zero cells/payload */
        if (filelen > SV_HDR_SIZE) {
            char z[4096];
            memset(z, 0, sizeof(z));
            off_t off = SV_HDR_SIZE;
            size_t left = filelen - SV_HDR_SIZE;
            while (left) {
                size_t n = left > sizeof(z) ? sizeof(z) : left;
                if (pwrite_retry(fd, z, n, off) < 0) break;
                off += (off_t)n;
                left -= n;
            }
        }
        sv_dbg("[sysvipc] create-init hdr+cells\n");
        created = 1;
    } else {
        sv_hdr hdr;
        ssize_t rd = sv_pread_full(fd, &hdr, SV_HDR_SIZE, 0);
        if (rd < 0 || hdr.magic != SV_MAGIC || hdr.itype != itype || hdr.destroy) {
            /* Kernel semantics: IPC_CREAT over an RMID'd (or garbage) file
             * recreates the object fresh — the id is reused but the peer
             * holding the old id sees it as destroyed + reopened. */
            if (create_flg) {
                memset(&hdr, 0, sizeof(hdr));
                hdr.magic   = SV_MAGIC;
                hdr.version = 2;
                hdr.itype   = itype;
                hdr.nsems   = itype == 1 ? nsems : 0;
                hdr.key     = (int32_t)key;
                hdr.id      = sv_make_id(itype, (int32_t)key);
                hdr.creator = (int32_t)getpid();
                hdr.nattch  = 0;
                hdr.size    = itype == 2 ? shm_sz : ((uint32_t)nsems);
                uint32_t filelen = SV_HDR_SIZE +
                                   (itype == 1 ? (uint32_t)nsems * SV_SEM_STRIDE : shm_sz);
                (void)ftruncate(fd, (off_t)filelen);
                (void)pwrite_retry(fd, &hdr, SV_HDR_SIZE, 0);
                created = 1;
            } else {
                (void)flock(fd, LOCK_UN);
                close(fd);
                errno = EIDRM;
                return -1;
            }
        }
        if (itype == 1 && !created && hdr.nsems != nsems) {
            /* kernel semantics: existing nsems wins for create-path reuse */
        }
    }

    (void)flock(fd, LOCK_UN);

    o->fd = fd;
    o->id = sv_make_id(itype, (int32_t)key);
    o->key = key;
    o->itype = itype;
    o->created = created;
    if (created_out) *created_out = created;
    sv_dbg("[sysvipc] open %s arch=%s created=%d id=%x\n", p,
#ifdef __i386__
           "i386"
#else
           "x86_64"
#endif
           , created, o->id);
    return 0;
}

/* lookup by id (decode id → itype + masked-key path). Returns 0 and fills
 * o; caller owns o->fd. */
static int sv_obj_by_id(sv_obj *o, int semid_or_shmid) {
    uint32_t raw = (uint32_t)semid_or_shmid;
    uint32_t itype = raw >> 28;
    if (itype != 1 && itype != 2) { errno = EINVAL; return -1; }
    int32_t key = (int32_t)(raw & 0x0FFFFFFFu);
    return sv_obj_open(o, itype, key, 0, 0, 0, NULL);
}
static void sv_obj_close(sv_obj *o) { if (o && o->fd >= 0) close(o->fd); if (o) o->fd = -1; }

/* ---------- semaphore primitives (fd-critical-section) ---------- */

static uint32_t sv_sem_getval_locked(int fd, uint32_t nsems_off_idx) {
    uint32_t v;
    (void)sv_pread_full(fd, &v, 4,
                        (off_t)(SV_HDR_SIZE + nsems_off_idx * SV_SEM_STRIDE));
    return v;
}

/* ---------- syscall wrappers ---------- */

int semget(key_t key, int nsems, int semflg) {
    if (nsems < 0) { errno = EINVAL; return -1; }

    if (key == IPC_PRIVATE) {
        /* Linux: IPC_PRIVATE ignores IPC_CREAT and returns fresh id. */
        key_t pk;
        if (sv_private_alloc(1, &pk) != 0) return -1;
        sv_obj o;
        if (sv_obj_open(&o, 1, pk, 1, (unsigned)(nsems > 0 ? nsems : 1), 0, NULL) != 0) return -1;
        sv_obj_close(&o);
        return sv_make_id(1, pk);
    }

    if (!((uint32_t)semflg & (IPC_CREAT | IPC_EXCL))) {
        /* pure lookup on missing object = ENOENT. */
        sv_obj o;
        if (sv_obj_open(&o, 1, key, 0, 0, 0, NULL) != 0) return -1;
        sv_obj_close(&o);
        return sv_make_id(1, key);
    }

    sv_obj o;
    int created = 0;
    if (sv_obj_open(&o, 1, key, 1, (unsigned)(nsems > 1 ? nsems : 1), 0, &created) != 0) return -1;
    if (!created && (semflg & IPC_EXCL)) {
        sv_obj_close(&o);
        errno = EEXIST;
        return -1;
    }
    if (!created && nsems > 1) {
        /* existing set: read its nsems; EINVAL when request exceeds */
        sv_hdr h;
        if (sv_pread_full(o.fd, &h, SV_HDR_SIZE, 0) < 0) { sv_obj_close(&o); return -1; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if ((uint32_t)nsems > h.nsems) { sv_obj_close(&o); errno = EINVAL; return -1; }
    }
    sv_obj_close(&o);
    return sv_make_id(1, key);
}

static int sv_semop_step(int fd, const struct sembuf *b, uint32_t nseg, int *blocked_out) {
    /* one PASS over ops under LOCK_EX: returns 0 all-applied; >0 = idx of
     * blocked op (caller sleeps+retries); -1 error. */
    for (uint32_t i = 0; i < nseg; i++) {
        const struct sembuf *s = &b[i];
        uint32_t val = sv_sem_getval_locked(fd, (uint32_t)s->sem_num);
        int32_t op = s->sem_op;
        if (op < 0) { /* P */
            if ((int32_t)val >= -op) {
                val -= (uint32_t)(-op);
                if (s->sem_flg & SEM_UNDO) {
                    /* UNDO bookkeeping would need per-process state; v1
                     * logs once and proceeds (steam tier0 uses UNDO for
                     * one-shot wait; process-death reap left as known gap). */
                    static int once;
                    if (!once++) fprintf(stderr, "[sysvipc] SEM_UNDO=1 passing NOOP (v1)\n");
                }
                (void)pwrite_retry(fd, &val, 4,
                                   (off_t)(SV_HDR_SIZE + (uint32_t)s->sem_num * SV_SEM_STRIDE));
            } else {
                *blocked_out = (int)i;
                return 1;
            }
        } else if (op > 0) { /* V */
            val += (uint32_t)op;
            (void)pwrite_retry(fd, &val, 4,
                               (off_t)(SV_HDR_SIZE + (uint32_t)s->sem_num * SV_SEM_STRIDE));
            /* publish: a blocked peer polls on next unlock — the polling
             * sleep is the v1 wake mechanism (futex would need per-process
             * robust-list; documented). */
        } else { /* zero-check */
            if (val == 0) continue;
            if (s->sem_flg & IPC_NOWAIT) { errno = EAGAIN; return -1; }
            *blocked_out = (int)i;
            return 1;
        }
    }
    return 0;
}

/* Per-thread open-addressing cache of sem/shm backing fds. Each entry
 * owns one fd already opened via the guest-visible path; LOCK_EX / LOCK_UN
 * on it never interferes with other threads (flock is scoped to the open
 * file description, not the inode) -> no in-shim cross-thread lock needed,
 * only per-PROCESS cross-process flock stays. 8 slots is generous: steam
 * tier0 uses 1-2 ids. Stale-after-RMID is safe: pread under flock still
 * sees destroy=1 before the unlink can bite, so stale fds fail the op
 * with EIDRM and the caller re-runs create if it wants to. Thread-exit
 * close is NOT hooked (documented gap: worst case leaks up to 8 fds per
 * thread; steam's mean usage is well under that). */
#define SV_FD_CAP 8
typedef struct { uint64_t key_id; int fd; } sv_fdc_slot;
static _Thread_local sv_fdc_slot sv_fdc[SV_FD_CAP];

static uint64_t sv_fd_key(uint32_t itype, int32_t id) {
    return ((uint64_t)itype << 32) | (uint32_t)id;
}
static int sv_fd_cache_find(uint32_t itype, int32_t id) {
    uint64_t k = sv_fd_key(itype, id);
    for (int i = 0; i < SV_FD_CAP; i++) {
        if (sv_fdc[i].key_id == k) return i;
        if (sv_fdc[i].key_id == 0) break;
    }
    return -1;
}
static void sv_fd_cache_evict(uint32_t itype, int32_t id) {
    uint64_t k = sv_fd_key(itype, id);
    for (int i = 0; i < SV_FD_CAP; i++) {
        if (sv_fdc[i].key_id == k) {
            close(sv_fdc[i].fd);
            sv_fdc[i].key_id = 0;
            sv_fdc[i].fd = -1;
            return;
        }
        if (sv_fdc[i].key_id == 0) break;
    }
}
/* fd lookup-or-open: hot for every semop/semctl. Miss cost is exactly
 * one sv_obj_open; hit cost is the scan alone. */
static int sv_fd_get(uint32_t itype, int32_t id) {
    int i = sv_fd_cache_find(itype, id);
    if (i >= 0) return sv_fdc[i].fd;
    sv_obj o;
    if (sv_obj_by_id(&o, (int32_t)id) != 0) return -1;
    int fd = o.fd;
    /* first free slot wins; full-cache fallback: evict slot 0 (steam's
     * working set is tiny so even slot-0-reuse-day is nearly free). */
    int idx = -1;
    for (int j = 0; j < SV_FD_CAP; j++) {
        if (sv_fdc[j].key_id == 0) { idx = j; break; }
    }
    if (idx < 0) { idx = 0; close(sv_fdc[0].fd); }
    sv_fdc[idx].key_id = sv_fd_key(itype, (int32_t)id);
    sv_fdc[idx].fd = fd;
    return fd;
}

int semtimedop(int semid, struct sembuf *sops, size_t nsops,
               const struct timespec *timeout) {
    if (!timeout) return semop(semid, sops, nsops);
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    struct timespec start;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    const long deadline_ms = (timeout->tv_sec * 1000) + (timeout->tv_nsec / 1000000);
    if (!sops) { errno = EFAULT; return -1; }
    int fdfd = sv_fd_get(/*itype=*/1, semid);
    if (fdfd < 0) return -1;
    sv_hdr h;
    (void)flock(fdfd, LOCK_EX);
    if (sv_pread_full(fdfd, &h, SV_HDR_SIZE, 0) < 0 || h.magic != SV_MAGIC || h.destroy) {
        (void)flock(fdfd, LOCK_UN);
        sv_fd_cache_evict(1, semid);
        errno = EIDRM;
        return -1;
    }
    (void)flock(fdfd, LOCK_UN);
    for (size_t i = 0; i < nsops; i++) {
        if ((uint32_t)sops[i].sem_num >= h.nsems) {
            errno = EFBIG;
            return -1;
        }
    }
    unsigned spins = 0;
    struct timespec ts = { 0, 20000L };
    for (;;) {
        int blocked = -1;
        (void)flock(fdfd, LOCK_EX);
        int rc = sv_semop_step(fdfd, sops, (uint32_t)nsops, &blocked);
        (void)flock(fdfd, LOCK_UN);
        if (rc < 0) return -1;
        if (rc == 0) return 0;
        if (sops[blocked].sem_flg & IPC_NOWAIT) { errno = EAGAIN; return -1; }
        if (deadline_ms == 0) { errno = EAGAIN; return -1; } /* [0,0] timeout: immediate */
        if (deadline_ms > 0) {
            struct timespec now;
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            long gone_ms = (now.tv_sec - start.tv_sec) * 1000L
                         + (now.tv_nsec - start.tv_nsec) / 1000000L;
            if (gone_ms >= deadline_ms) { errno = EAGAIN; return -1; }
        }
        if (++spins < 200) { (void)nanosleep(&ts, NULL); continue; }
        ts.tv_nsec = 1000000L;
        (void)nanosleep(&ts, NULL);
    }
}

int semop(int semid, struct sembuf *sops, size_t nsops) {
    if (!sops) { errno = EFAULT; return -1; }
    int fdfd = sv_fd_get(/*itype=*/1, semid);
    if (fdfd < 0) return -1;
    /* range check nsems once: out-of-bounds slices fail fast up front. read
     * the header under flock EX so the destroy=1 + unlink race stays
     * observable on the pread view (always-per-op re-verify). */
    sv_hdr h;
    (void)flock(fdfd, LOCK_EX);
    if (sv_pread_full(fdfd, &h, SV_HDR_SIZE, 0) < 0 || h.magic != SV_MAGIC || h.destroy) {
        (void)flock(fdfd, LOCK_UN);
        sv_fd_cache_evict(1, semid);
        errno = EIDRM;
        return -1;
    }
    (void)flock(fdfd, LOCK_UN);
    for (size_t i = 0; i < nsops; i++) {
        if ((uint32_t)sops[i].sem_num >= h.nsems) {
            errno = EFBIG;
            return -1;
        }
    }

    unsigned spins = 0;
    struct timespec ts = { 0, 20000L };
    for (;;) {
        int blocked = -1;
        (void)flock(fdfd, LOCK_EX);
        int rc = sv_semop_step(fdfd, sops, (uint32_t)nsops, &blocked);
        (void)flock(fdfd, LOCK_UN);
        if (rc < 0) return -1;
        if (rc == 0) return 0;
        if (sops[blocked].sem_flg & IPC_NOWAIT) { errno = EAGAIN; return -1; }
        if (++spins < 100) continue;
        nanosleep(&ts, NULL);
    }
}

int semctl(int semid, int semnum, int cmd, ...) {
    union semun {
        int val;
        struct semid_ds *buf;
        unsigned short *array;
    } u = { 0 };
    if (cmd == SETVAL) {
        /* va_arg must match the C-promoted scalar type; reading a union
         * when the caller passed an int is UB (arm64 va_arg alignment,
         * observed with x32 direct-ABI FEX guests passing SETVAL's raw int). */
        va_list ap;
        va_start(ap, cmd);
        u.val = va_arg(ap, int);
        va_end(ap);
    } else if (cmd == SETALL || cmd == IPC_STAT || cmd == IPC_SET || cmd == GETALL) {
        va_list ap;
        va_start(ap, cmd);
        u = va_arg(ap, union semun);
        va_end(ap);
    }

    int cdfd = sv_fd_get(/*itype=*/1, semid);
    if (cdfd < 0) return -1;
    (void)flock(cdfd, LOCK_EX);

    sv_hdr h;
    if (sv_pread_full(cdfd, &h, SV_HDR_SIZE, 0) < 0 || h.magic != SV_MAGIC || h.destroy) {
        (void)flock(cdfd, LOCK_UN);
        sv_fd_cache_evict(1, semid);
        errno = EIDRM;
        return -1;
    }
    if (h.itype != 1) { (void)flock(cdfd, LOCK_UN); errno = EINVAL; return -1; }
    uint32_t nsems = h.nsems;
    int rc = 0;

    switch (cmd) {
    case IPC_RMID:
        h.destroy = 1;
        (void)pwrite_retry(cdfd, &h, SV_HDR_SIZE, 0);
        sv_fd_cache_evict(1, semid);
        rc = 0;
        break;
    case SETVAL:
        if ((uint32_t)semnum >= nsems) { errno = EFBIG; rc = -1; goto out; }
        (void)pwrite_retry(cdfd, &u.val, 4,
                           (off_t)(SV_HDR_SIZE + (uint32_t)semnum * SV_SEM_STRIDE));
        rc = 0;
        break;
    case GETVAL:
        if ((uint32_t)semnum >= nsems) { errno = EFBIG; rc = -1; goto out; }
        rc = (int)sv_sem_getval_locked(cdfd, (uint32_t)semnum);
        break;
    case SETALL: {
        if (!u.array) { errno = EFAULT; rc = -1; goto out; }
        uint32_t vs[64];
        uint32_t n = nsems > 64 ? 64 : nsems;
        for (uint32_t i = 0; i < n; i++) vs[i] = u.array[i];
        for (uint32_t i = 0; i < n; i++)
            (void)pwrite_retry(cdfd, &vs[i], 4, (off_t)(SV_HDR_SIZE + i * SV_SEM_STRIDE));
        rc = 0;
        break;
    }
    case GETALL: {
        if (!u.array) { errno = EFAULT; rc = -1; goto out; }
        uint32_t n = nsems > 64 ? 64 : nsems;
        for (uint32_t i = 0; i < n; i++)
            u.array[i] = (unsigned short)sv_sem_getval_locked(cdfd, i);
        rc = 0;
        break;
    }
    default:
        errno = EINVAL;
        rc = -1;
        goto out;
    }

out:
    (void)flock(cdfd, LOCK_UN);
    return rc;
}

/* ---------- shm (same-arch pairing; cross-ABI mmap = known v1 gap) ------ */

int shmget(key_t key, size_t size, int shmflg) {
    if (key == IPC_PRIVATE) {
        key_t pk;
        if (sv_private_alloc(2, &pk) != 0) return -1;
        sv_obj o;
        if (sv_obj_open(&o, 2, pk, 1, 0, (unsigned)(size ? size : 4096), NULL) != 0) return -1;
        sv_obj_close(&o);
        return sv_make_id(2, pk);
    }
    int found;
    {
        sv_obj o;
        if (sv_obj_open(&o, 2, key, 0, 0, 0, NULL) == 0) { sv_obj_close(&o); found = 1; }
        else found = 0;
    }
    if (!found && !(shmflg & IPC_CREAT)) { errno = ENOENT; return -1; }
    sv_obj o;
    int created = 0;
    if (sv_obj_open(&o, 2, key, 1, 0, (unsigned)(size ? size : 4096), &created) != 0) return -1;
    if (!created && (shmflg & IPC_EXCL)) { sv_obj_close(&o); errno = EEXIST; return -1; }
    sv_obj_close(&o);
    return sv_make_id(2, key);
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
    sv_obj o;
    if (sv_obj_by_id(&o, shmid) != 0) return (void *)-1;

    (void)flock(o.fd, LOCK_EX);
    sv_hdr h;
    if (sv_pread_full(o.fd, &h, SV_HDR_SIZE, 0) < 0 || h.magic != SV_MAGIC ||
        h.itype != 2 || (h.destroy && !(shmflg & SHM_RDONLY))) {
        (void)flock(o.fd, LOCK_UN);
        sv_obj_close(&o);
        errno = EIDRM;
        return (void *)-1;
    }
    h.nattch++;
    (void)pwrite_retry(o.fd, &h, SV_HDR_SIZE, 0);
    (void)flock(o.fd, LOCK_UN);

    uint32_t size = h.size ? h.size : 4096;
    size_t mlen = ((size_t)SV_HDR_SIZE + size + 4095u) & ~4095u;
    void *ret = mmap(NULL, mlen, PROT_READ | PROT_WRITE, MAP_SHARED, o.fd, 0);
    sv_obj_close(&o);
    if (ret == MAP_FAILED) return (void *)-1;
    if (shmflg & SHM_RDONLY) {
        (void)mprotect(ret, mlen, PROT_READ);
    }
    /* caller never touches the header at our payload pointer — we return
     * an in-page pointer just past the struct (that guarantees alignment
     * to 4 AND a stable offset visible to the per-arch mapping view). */
    return (char *)ret + SV_HDR_SIZE;
}

int shmdt(const void *shmaddr) {
    if (!shmaddr) { errno = EINVAL; return -1; }
    /* find the object id by scanning our dir for a matching creator name?
     * v1: we don't track attach→id client-side in the shim (per-process
     * bookkeeping would need a TLS table; documented gap). Just munmap the
     * single page — payload pages beyond that leak (cheap). */
    (void)munmap((void *)((const char *)shmaddr - SV_HDR_SIZE), SV_HDR_SIZE + 4096);
    return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    (void)buf;
    sv_obj o;
    if (sv_obj_by_id(&o, shmid) != 0) return -1;
    (void)flock(o.fd, LOCK_EX);
    sv_hdr h;
    if (sv_pread_full(o.fd, &h, SV_HDR_SIZE, 0) < 0 || h.magic != SV_MAGIC || h.itype != 2) {
        (void)flock(o.fd, LOCK_UN);
        sv_obj_close(&o);
        errno = EINVAL;
        return -1;
    }
    int rc = 0;
    if (cmd == IPC_RMID) {
        h.destroy = 1;
        (void)pwrite_retry(o.fd, &h, SV_HDR_SIZE, 0);
        if (h.nattch == 0) {
            char p[512];
            sv_path(p, sizeof(p), 2, (key_t)((uint32_t)o.key));
            unlink(p);
        }
        rc = 0;
    } else {
        errno = EINVAL;
        rc = -1;
    }
    (void)flock(o.fd, LOCK_UN);
    sv_obj_close(&o);
    return rc;
}
