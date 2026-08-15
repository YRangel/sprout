/*
 * sprout_preload.c — the sprout LD_PRELOAD path-translation core.
 *
 * Interposes the path-taking libc entry points that matter for running a
 * glibc userspace under a relocated root on Android, and rewrites their
 * string arguments in-place before tail-calling the real symbols. No
 * allocation, no syscalls of our own except what libc does underneath;
 * safe to dlopen into Node/Python/Chromium.
 *
 * Design contract (docs/src/architecture/interception.md):
 *   - translation is pure and separately unit-tested (test_translate.c)
 *   - wrappers only assemble the real symbol + do the rewrite; no logic
 *   - idempotent: translating a translated path is a no-op, so stacked
 *     wrappers (e.g. execve→execv→execve) never double-prefix
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sprout_preload.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static size_t strip_trailing_slashes(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') s[--n] = '\0';
    return n;
}

void sp_config_load(sp_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    const char *root = getenv("SPROUT_ROOTFS");
    if (root && root[0] == '/') {
        copy_str(cfg->rootfs, sizeof(cfg->rootfs), root);
        cfg->rootfs_len = strip_trailing_slashes(cfg->rootfs);
    }

    const char *dbg = getenv("SPROUT_DEBUG");
    cfg->debug = dbg && dbg[0] == '1';
    const char *fake = getenv("SPROUT_FAKEROOT");
    cfg->fakeroot = fake && fake[0] == '1';
    const char *l2s = getenv("SPROUT_LINK2SYMLINK");
    cfg->link2symlink = l2s && l2s[0] == '1';

    /* Host pseudo-filesystem mountpoints (never re-prefixed with rootfs;
     * consulted AFTER SPROUT_BIND entries so binds can override). */
    static const char *default_pt[] = { "/proc", "/sys", "/dev" };
    const char *pt_env = getenv("SPROUT_PASSTHROUGH");
    if (!pt_env || !*pt_env) {
        cfg->npassthrough = 3;
        for (int i = 0; i < 3; i++) {
            cfg->passthrough[i].prefix = default_pt[i];
            cfg->passthrough[i].len = strlen(default_pt[i]);
        }
    } else {
        char pbuf[2048];
        copy_str(pbuf, sizeof(pbuf), pt_env);
        char *psave = NULL;
        for (char *tok = strtok_r(pbuf, ";", &psave);
             tok && cfg->npassthrough < SP_MAX_PASSTHROUGH;
             tok = strtok_r(NULL, ";", &psave)) {
            if (tok[0] != '/') continue;
            cfg->passthrough[cfg->npassthrough].prefix = strdup(tok);
            cfg->passthrough[cfg->npassthrough].len = strlen(tok);
            cfg->npassthrough++;
        }
    }

    /* SPROUT_BIND: "host=guest;host=guest;..." */
    const char *binds = getenv("SPROUT_BIND");
    if (!binds) return;

    char buf[8192];
    copy_str(buf, sizeof(buf), binds);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ";", &save);
         tok && cfg->nbinds < SP_MAX_BINDS;
         tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        if (tok[0] != '/' || eq[1] != '/') continue;

        sp_bind_t *b = &cfg->binds[cfg->nbinds];
        copy_str(b->host, sizeof(b->host), tok);
        copy_str(b->guest, sizeof(b->guest), eq + 1);
        b->host_len = strip_trailing_slashes(b->host);
        b->guest_len = strip_trailing_slashes(b->guest);
        cfg->nbinds++;
    }

    /* Insertion-sort guest prefixes longest-first so the most specific
     * binding wins on lookup. */
    for (int i = 1; i < cfg->nbinds; i++) {
        sp_bind_t key = cfg->binds[i];
        int j = i - 1;
        while (j >= 0 && cfg->binds[j].guest_len < key.guest_len) {
            cfg->binds[j + 1] = cfg->binds[j];
            j--;
        }
        cfg->binds[j + 1] = key;
    }
}

/* ------------------------------------------------------------------ */
/* Translation (pure; unit-tested independently)                       */
/* ------------------------------------------------------------------ */

static int path_within(const char *prefix, size_t plen, const char *path) {
    return strncmp(prefix, path, plen) == 0 &&
           (path[plen] == '\0' || path[plen] == '/');
}

int sp_translate(const sp_config_t *cfg, const char *path, char out[SP_PATH_MAX]) {
    if (!path || path[0] != '/') return 0; /* relative paths resolve against cwd, untouched */

    /* Idempotence: a path that is already host-side must not be re-prefixed. */
    if (cfg->rootfs_len > 0 && path_within(cfg->rootfs, cfg->rootfs_len, path))
        return 0;

    /* User binds are MORE SPECIFIC than the pseudo-fs passthrough
     * (/proc,/sys,/dev): consult them FIRST so an explicit /dev/shm or
     * /proc/self/stat bind can override the passthrough. */
    for (int i = 0; i < cfg->nbinds; i++) {
        const sp_bind_t *b = &cfg->binds[i];
        if (!path_within(b->guest, b->guest_len, path)) continue;
        size_t rest = strlen(path + b->guest_len);
        if (b->host_len + rest + 1 > SP_PATH_MAX) return 0;
        memcpy(out, b->host, b->host_len);
        memcpy(out + b->host_len, path + b->guest_len, rest + 1);
        return 1;
    }

    /* Never translate host pseudo-filesystem mountpoints. */
    for (int i = 0; i < cfg->npassthrough; i++) {
        if (path_within(cfg->passthrough[i].prefix, cfg->passthrough[i].len, path))
            return 0;
    }

    if (cfg->rootfs_len == 0) return 0;
    size_t n = strlen(path);
    if (cfg->rootfs_len + n + 1 > SP_PATH_MAX) return 0;
    memcpy(out, cfg->rootfs, cfg->rootfs_len);
    memcpy(out + cfg->rootfs_len, path, n + 1);
    return 1;
}

size_t sp_reverse(const sp_config_t *cfg, const char *host, char *out, size_t outsz) {
    for (int i = 0; i < cfg->nbinds; i++) {
        const sp_bind_t *b = &cfg->binds[i];
        if (!path_within(b->host, b->host_len, host)) continue;
        size_t rest = strlen(host + b->host_len);
        size_t total = b->guest_len + rest;
        if (total + 1 > outsz) { total = outsz - 1; }
        memcpy(out, b->guest, b->guest_len < total ? b->guest_len : total);
        size_t room = total - b->guest_len;
        if (room > 0) memcpy(out + b->guest_len, host + b->host_len, room);
        out[total] = '\0';
        return total;
    }

    if (cfg->rootfs_len > 0 && path_within(cfg->rootfs, cfg->rootfs_len, host)) {
        const char *rel = host + cfg->rootfs_len;
        if (*rel == '\0') rel = "/";
        size_t n = strlen(rel);
        if (n + 1 > outsz) n = outsz - 1;
        memcpy(out, rel, n);
        out[n] = '\0';
        return n;
    }

    size_t n = strlen(host);
    if (n + 1 > outsz) n = outsz - 1;
    memcpy(out, host, n);
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* Interposition                                                       */
/* ------------------------------------------------------------------ */
#ifdef SPROUT_INTERPOSE

static sp_config_t g_cfg;

/* Fake-identity anchor state (definitions live next to the set*id stubs;
 * referenced by the constructor above + spoof/peercred helpers below). */
extern uid_t g_fake_uid;
extern gid_t g_fake_gid;
extern gid_t g_fake_groups[64];
extern int   g_fake_ngroups;

__attribute__((constructor)) static void sprout_init(void) {
    sp_config_load(&g_cfg);
    /* --user anchor (proot -i / proot-distro --user parity): the resolved
     * guest identity is forwarded by the launcher as SPROUT_FAKE_UID/GID;
     * absent = root anchor (0/0). Musl-safe digit loop (no sscanf/atoi). */
    {
        const char *fu = getenv("SPROUT_FAKE_UID");
        const char *fg = getenv("SPROUT_FAKE_GID");
        unsigned long u = 0, g = 0;
        if (fu && *fu) { for (const char *p = fu; *p >= '0' && *p <= '9'; p++) u = u * 10u + (unsigned)(*p - '0'); }
        if (fg && *fg) { for (const char *p = fg; *p >= '0' && *p <= '9'; p++) g = g * 10u + (unsigned)(*p - '0'); }
        g_fake_uid = (uid_t)u;
        g_fake_gid = (gid_t)g;
        g_fake_groups[0] = g_fake_gid;
        g_fake_ngroups = 1;
    }
}

static void sp_trace_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define SP_SYS_openat 56
#define SP_SYS_write 64
#define SP_SYS_close 57
static void sp_trace_line(const char *fmt, ...) {
    const char *logf = getenv("SPROUT_TRACELOG");
    if (!logf || !*logf) return;
    /* RAW syscall path: no PLT, no recursion through our own wrappers */
    int fd = (int)syscall(SP_SYS_openat, AT_FDCWD, logf,
                          O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) (void)!syscall(SP_SYS_write, fd, buf, (size_t)n);
    (void)syscall(SP_SYS_close, fd);
}

#define SP_TRACE(name, orig, trans)                                        \
    do {                                                                   \
        if (g_cfg.debug) {                                                 \
            fprintf(stderr, "[sprout] %s(\"%s\") -> \"%s\"\n", name,        \
                    orig ? orig : "(null)", trans);                        \
        }                                                                  \
        sp_trace_line("TR %s pid=%d '%s' -> '%s'\n", name,                \
                    (int)getpid(), orig ? orig : "(null)", trans);          \
    } while (0)

/* configure-style guests redirect ALL child stderr to /dev/null or files,
 * hiding every debug print the interposer wants to emit. SPROUT_TRACELOG
 * names a host-fs file that receives a JSON-ish event stream per process.
 * This opened the fcntl-AC_CHECK_FUNCS-producing-mystery to diagnosis:
 * fcntl spawned pos_spawn(children) whose stderr never reached us before. */
static void sp_trace_exec(const char *path, char *const argv[], int class_) {
    const char *logf = getenv("SPROUT_TRACELOG");
    if (!logf || !*logf) return;
    int fd = (int)syscall(SP_SYS_openat, AT_FDCWD, logf,
                          O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return;
    char buf[4096];
    int n = 0;
    n = snprintf(buf + n, sizeof(buf) - (size_t)n, "TRACE pid=%d class=%d path=%s",
                 (int)getpid(), class_, path ? path : "(null)");
    if (argv) {
        for (int i = 0; argv[i] && i < 64 && n < (int)sizeof(buf) - 128; i++)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " |%s", argv[i]);
    }
    buf[n++] = '\n';
    (void)!write(fd, buf, (size_t)n);
    close(fd);
}

#define SP_REAL(name) real_##name

/* POSIX explicitly blesses assigning dlsym()'s void* to a function pointer;
 * ISO C pedantically forbids it, so we take the one documented POSIX escape
 * hatch (memcpy through a union). The union is typed with the *pointer* so
 * the assignment stays object-pointer, never function-declared. */
static void *sp_sym(const char *name) { return dlsym(RTLD_NEXT, name); }

#define SP_RESOLVE(name)                                                   \
    do {                                                                   \
        if (!SP_REAL(name)) {                                              \
            union {                                                        \
                void *raw;                                                 \
                __typeof__(SP_REAL(name)) fn;                              \
            } u = { NULL };                                                \
            u.raw = sp_sym(#name);                                         \
            SP_REAL(name) = u.fn;                                          \
        }                                                                  \
    } while (0)

/* Translate memoization. sp_translate()+chase is a pure function of the
 * guest path (per-process env+binds are frozen at init), yet the chase
 * costs an lstat+readlink (~1.1ms bulk) for EVERY translated path —
 * dominates exec-chains (20x /bin/true = 19 wasted lookups). Small
 * process-lifetime open-addressing cache kills the repeat cost.
 * Positive results only; unresolvable/excluded paths are cheap already.
 * See ADR-0010. */
#define SP_XCACHE_CAP 128
struct sp_xentry { char g[SP_PATH_MAX]; char h[SP_PATH_MAX]; unsigned char used; };
static struct sp_xentry sp_xcache[SP_XCACHE_CAP];
static unsigned long sp_xhash(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}
static int sp_xcache_get(const char *g, char h[SP_PATH_MAX]) {
    unsigned long i = sp_xhash(g) % SP_XCACHE_CAP;
    for (int hop = 0; hop < 8; hop++) {
        struct sp_xentry *e = &sp_xcache[(i + hop) % SP_XCACHE_CAP];
        if (!e->used) return 0;
        if (!strcmp(e->g, g)) { strcpy(h, e->h); return 1; }
    }
    return 0;
}
static void sp_xcache_put(const char *g, const char *h) {
    unsigned long i = sp_xhash(g) % SP_XCACHE_CAP;
    for (int hop = 0; hop < 8; hop++) {
        struct sp_xentry *e = &sp_xcache[(i + hop) % SP_XCACHE_CAP];
        if (!e->used || !strcmp(e->g, g)) {
            strncpy(e->g, g, SP_PATH_MAX - 1); e->g[SP_PATH_MAX - 1] = 0;
            strncpy(e->h, h, SP_PATH_MAX - 1); e->h[SP_PATH_MAX - 1] = 0;
            e->used = 1;
            return;
        }
    }
}

/* Translate + absolute-symlink chase. Alpine lays every applet out as an
 * absolute symlink to /bin/busybox; the host kernel would resolve those
 * targets on the HOST (missing). Only when the translation moved the path
 * do we chase (host passthrough paths are the kernel's business).
 * RTLD_NEXT syscalls keep us free of interposer recursion. */
static int (*sp_real_lstat)(const char *, struct stat *) = NULL;
static ssize_t (*sp_real_readlink)(const char *, char *, size_t) = NULL;
/* -Wreturn-local-addr false-positive: every return path carries either
 * caller storage (path/buf) or `buf`; `joined` only flows into buf via
 * memcpy. GCC's points-to for the array params can't prove that. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"
#endif
static const char *sp_translate_xf(const char *path, char buf[SP_PATH_MAX], int follow_final) {
    /* Relative paths: hang onto the real cwd in HOST view, then run the
     * normal absolute translation (apk's db-write mix of absolute charm +
     * relative journal journaling across mixed cwd changes lands at the
     * right rootfs location under any cwd). */
    char joined[SP_PATH_MAX];
    if (path && path[0] != '/') {
        char cw[SP_PATH_MAX];
        ssize_t cn = readlink("/proc/self/cwd", cw, sizeof(cw) - 1);
        if (cn > 0 && (size_t)cn < sizeof(cw)) {
            cw[cn] = '\0';
            size_t rl = g_cfg.rootfs_len;
            if (rl && strncmp(cw, g_cfg.rootfs, rl) == 0) {
                int w = snprintf(joined, sizeof(joined), "%s/%s", cw + rl, path);
                if (w > 0 && (size_t)w < sizeof(joined)) path = joined;
            }
        }
    }
    /* fast path: cache hit (covers the exec-chain hot loop). The cache is
     * chase-dependent, so only follow_final=1 results are cached/serve. */
    if (follow_final && sp_xcache_get(path, buf)) return buf;
    /* Materialize a joined relative path into the caller-owned buffer: a
     * translate-decline here (passthrough prefix, e.g. guest /proc) would
     * otherwise leave `out` pointing at this frame's stack — a dangling
     * Heisenbug. */
    const char *out;
    if (path == joined) {
        if (!sp_translate(&g_cfg, path, buf)) {
            size_t jl = strlen(joined);
            if (jl >= SP_PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
            memcpy(buf, joined, jl + 1);
        }
        out = buf;
    } else {
        out = sp_translate(&g_cfg, path, buf) ? buf : path;
    }
    if (out != buf) return out;
    if (!follow_final) return out;
    static int l2s_off = -1;
    if (l2s_off < 0) l2s_off = getenv("SPROUT_DISABLE_L2S") ? 1 : 0;
    if (l2s_off) return out;
    char dir[SP_PATH_MAX], tmp[SP_PATH_MAX], lnk[SP_PATH_MAX];
    int hop;
    for (hop = 0; hop < 8; hop++) {
        struct stat st;
        if (!sp_real_lstat) sp_real_lstat = dlsym(RTLD_NEXT, "lstat");
        if (!sp_real_lstat) break;
        if (sp_real_lstat(buf, &st) != 0 || !S_ISLNK(st.st_mode)) break;
        if (!sp_real_readlink) sp_real_readlink = dlsym(RTLD_NEXT, "readlink");
        if (!sp_real_readlink) break;
        ssize_t n = sp_real_readlink(buf, lnk, sizeof(lnk) - 1);
        if (n < 0) break;
        lnk[n] = '\0';
        if (lnk[0] == '/') {
            char back[SP_PATH_MAX];
            if (!sp_translate(&g_cfg, lnk, back)) break;
            snprintf(buf, SP_PATH_MAX, "%s", back);
        } else {
            snprintf(dir, sizeof(dir), "%s", buf);
            char *sl = strrchr(dir, '/');
            if (!sl) break;
            *sl = '\0';
            int w = snprintf(tmp, sizeof(tmp), "%s/%s", dir, lnk);
            if (w <= 0 || (size_t)w >= sizeof(tmp)) break;
            w = snprintf(buf, SP_PATH_MAX, "%s", tmp);
            if (w <= 0 || (size_t)w >= SP_PATH_MAX) break;
        }
    }
    /* Cache only when no chase hop happened: a followed symlink target
     * may rotate (ln -sf → rustup/npm) and the cache has no invalidation
     * on guest mutations; pure path-prefix mappings never change. */
    if (hop == 0) sp_xcache_put(path, buf);
    return buf;
}
static const char *sp_translate_x(const char *path, char buf[SP_PATH_MAX]) {
    return sp_translate_xf(path, buf, 1);
}
/* l-variant: translate but NEVER chase the final component. Required by the
 * lstat/unlink/rename/lutimes/... family: chasing would falsify the object
 * the syscall targets (e.g. utimensat(AT_SYMLINK_NOFOLLOW) hitting the link
 * TARGET instead of the link — breaks dpkg's symlink tar processing). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static const char *sp_translate_l(const char *path, char buf[SP_PATH_MAX]) {
    return sp_translate_xf(path, buf, 0);
}

/* open-family: const char* path */
int open(const char *path, int flags, ...) {
    static int (*SP_REAL(open))(const char *, int, ...) = NULL;
    SP_RESOLVE(open);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("open", path, p);
    return SP_REAL(open)(p, flags, mode);
}

int open64(const char *path, int flags, ...) {
    static int (*SP_REAL(open64))(const char *, int, ...) = NULL;
    SP_RESOLVE(open64);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("open64", path, p);
    return SP_REAL(open64)(p, flags, mode);
}

/* Directory-relative passthrough: a dirfd-relative path belongs to the
 * HOST's fd table; translating it against cwd would falsify the target
 * (apk db rotation, nftw walk patterns). Raw pass-through is exact. */
int openat(int dirfd, const char *path, int flags, ...) {
    static int (*SP_REAL(openat))(int, const char *, int, ...) = NULL;
    SP_RESOLVE(openat);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(openat)(dirfd, path, flags, mode);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("openat", path, p);
    return SP_REAL(openat)(dirfd, p, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static int (*SP_REAL(openat64))(int, const char *, int, ...) = NULL;
    SP_RESOLVE(openat64);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(openat64)(dirfd, path, flags, mode);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("openat64", path, p);
    return SP_REAL(openat64)(dirfd, p, flags, mode);
}

/* ---- access-family EMULATION (blocked-syscall class) ------------------
 * Android untrusted-app seccomp kills faccessat(48) and faccessat2(439).
 * glibc's access() implements via those *inside* libc, so calling glibc's
 * versions from a wrapper only dies later. Answer POSIX here directly,
 * via newfstatat(79 — allowed) + uid/gid bits. No libc patching, no lies:
 * [ -r file ] test = real answer. */
static uid_t  (*sp_real_getuid)(void) = NULL;
static uid_t  (*sp_real_geteuid)(void) = NULL;
static gid_t  (*sp_real_getgid)(void) = NULL;
static gid_t  (*sp_real_getegid)(void) = NULL;
static int    (*sp_real_getgroups)(int, gid_t *) = NULL;
static int    (*sp_real_fstatat)(int, const char *, struct stat *, int) = NULL;
static void sp_resolve_access_emul(void) {
    if (!sp_real_fstatat) {
        sp_real_fstatat   = dlsym(RTLD_NEXT, "fstatat");
        sp_real_getuid    = dlsym(RTLD_NEXT, "getuid");
        sp_real_geteuid   = dlsym(RTLD_NEXT, "geteuid");
        sp_real_getgid    = dlsym(RTLD_NEXT, "getgid");
        sp_real_getegid   = dlsym(RTLD_NEXT, "getegid");
        sp_real_getgroups = dlsym(RTLD_NEXT, "getgroups");
    }
}
static int sp_emulate_access_impl(int dirfd, const char *path, int mode, int flags,
                                  int use_eid) {
    sp_resolve_access_emul();
    if (!sp_real_fstatat || !sp_real_getuid || !sp_real_getgid) { errno = ENOSYS; return -1; }
    struct stat st;
    int atflags = flags & (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT);
    if (sp_real_fstatat(dirfd, path, &st, atflags) != 0) return -1; /* errno from fstatat */
    if (mode == F_OK) return 0;
    uid_t uid = use_eid && sp_real_geteuid ? sp_real_geteuid() : sp_real_getuid();
    gid_t gid = use_eid && sp_real_getegid ? sp_real_getegid() : sp_real_getgid();
    unsigned need = ((mode & R_OK) ? 4 : 0) | ((mode & W_OK) ? 2 : 0) | ((mode & X_OK) ? 1 : 0);
    unsigned bits;
    if (uid == 0) {
        /* root: R/W always, X iff any exec bit set */
        bits = (need & 6) | (((st.st_mode & 0111) != 0) ? 1 : 0);
        return ((bits & need) == need) ? 0 : (errno = EACCES, -1);
    }
    if (st.st_uid == uid) bits = st.st_mode >> 6;
    else if (st.st_gid == gid) bits = st.st_mode >> 3;
    else {
        bits = st.st_mode;
        if (sp_real_getgroups) {
            gid_t tabs[64];
            int n = sp_real_getgroups(64, tabs);
            for (int i = 0; i < n && n > 0; i++)
                if (tabs[i] == st.st_gid) { bits = st.st_mode >> 3; break; }
        }
    }
    return ((bits & need) == need) ? 0 : (errno = EACCES, -1);
}

int access(const char *path, int mode) {
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("access", path, p);
    return sp_emulate_access_impl(AT_FDCWD, p, mode, 0, 0);
}

int eaccess(const char *path, int mode) {
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("eaccess", path, p);
    return sp_emulate_access_impl(AT_FDCWD, p, mode, 0, 1);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("faccessat", path, p);
    return sp_emulate_access_impl(dirfd, p, mode, flags, flags & AT_EACCESS);
}

/* glibc also exports its internal nocancel-style open symbols; libc modules
 * that bypass the public PLT (notably libnss_dns reading /etc/resolv.conf,
 * and the nsswitch engine reading /etc/nsswitch.conf) call THOSE. Without
 * interception the DNS resolver falls back to nameserver 127.0.0.1 — i.e.,
 * Android-loopback stays silent and every apt getter EAI_AGAINs. */
int __open64_nocancel(const char *path, int flags, ...) {
    static int (*SP_REAL(__open64_nocancel))(const char *, int, ...) = NULL;
    SP_RESOLVE(__open64_nocancel);
    if (!SP_REAL(__open64_nocancel)) SP_REAL(__open64_nocancel) = dlsym(RTLD_NEXT, "open64"); /* musl fallback */
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("__open64_nocancel", path, p);
    return SP_REAL(__open64_nocancel)(p, flags, mode);
}

int __openat64_nocancel(int dirfd, const char *path, int flags, ...) {
    static int (*SP_REAL(__openat64_nocancel))(int, const char *, int, ...) = NULL;
    SP_RESOLVE(__openat64_nocancel);
    if (!SP_REAL(__openat64_nocancel)) SP_REAL(__openat64_nocancel) = dlsym(RTLD_NEXT, "openat64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("__openat64_nocancel", path, p);
    return SP_REAL(__openat64_nocancel)(dirfd, p, flags, mode);
}

/* glibc's actual filepath reach-in for resolver configs uses the public-ish
 * __open64 / __openat64 channels in most builds. */
/* ---------------- stub DNS client + getaddrinfo family ----------------
 * glibc's resolver (nss_dns) reads /etc/resolv.conf through *libc-internal*
 * opens we cannot interpose (verified: LD_DEBUG=bindings shows NO symbol
 * involvement); guest apt/python/curl then falls back to nameserver
 * 127.0.0.1 (host loopback, silent) => EAI_AGAIN everywhere.
 * musl's resolver reads through *exported* fopen — already works.
 * This section intercepts the public resolution entry points for glibc,
 * parses the guest resolv.conf through OUR translated paths, and performs
 * a real stub DNS exchange — answers are live, no fabrication.
 */
#undef AF_INET
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static char *sp_dns_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

#define SP_AI_BRAND 0x7370726f  /* 'spro' — marks addrinfo chains we own */

/* read up to 4 nameserver IPs from the guest's resolv.conf */
static int sp_dns_servers(struct in_addr out[4]) {
    char pb[SP_PATH_MAX];
    const char *path = sp_translate_x("/etc/resolv.conf", pb);
    FILE *f = fopen(path, "r");   /* wrapper -> translated */
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (n < 4 && fgets(line, sizeof(line), f)) {
        char ip[64];
        /* sscanf/atoi redirect to __isoc23_* on glibc≥2.40 headers,
         * which then fails at RUNTIME in musl guests (undefined symbols).
         * Parse by hand (resolv.conf 'nameserver IP[:port]' is simple). */
        char *nsp = line;
        while (*nsp == ' ' || *nsp == '\t') nsp++;
        ip[0] = 0;
        if (strncmp(nsp, "nameserver", 10) == 0) {
            nsp += 10;
            while (*nsp == ' ' || *nsp == '\t') nsp++;
            size_t ipl = strcspn(nsp, " \t\n");
            if (ipl && ipl < sizeof ip) { memcpy(ip, nsp, ipl); ip[ipl] = 0; }
        }
        if (ip[0])
            if (inet_aton(ip, &out[n]) != 0) n++;
    }
    fclose(f);
    if (n == 0) {
        inet_aton("8.8.8.8", &out[0]);
        inet_aton("8.8.4.4", &out[1]);
        n = 2;
    }
    return n;
}

struct sp_dns_hdr { uint16_t id, flags, qd, an, ns, ar; };
#define SP_DNS_TXT_LEN 512

static int sp_dns_encode(char *dst, const char *host, uint16_t qtype, uint16_t id) {
    struct sp_dns_hdr *h = (struct sp_dns_hdr *)dst;
    memset(dst, 0, SP_DNS_TXT_LEN);
    h->id = htons(id);
    h->flags = htons(0x0100); /* RD */
    h->qd = htons(1);
    char *w = dst + 12;
    const char *p = host, *lv = p;
    for (;;) {
        if (*p == '.' || *p == 0) {
            size_t l = (size_t)(p - lv);
            if (l == 0) break;
            if (l > 63) return -1;
            *w++ = (char)l; memcpy(w, lv, l); w += l;
            lv = p + 1;
        }
        if (*p == 0) break;
        p++;
    }
    *w++ = 0;
    uint16_t *qt = (uint16_t *)w;
    *qt++ = htons(qtype);
    *qt++ = htons(1); /* IN */
    return (int)((char *)qt - dst);
}

/* crude answer walker: skip CNAME chains, return first A or AAAA rdata */
static int sp_dns_extract(const char *bufv, ssize_t nread, int want_v6,
                          struct in_addr a4[8], int *na4,
                          struct in6_addr a6[8], int *na6) {
    const unsigned char *buf = (const unsigned char *)bufv;
    if (nread < 12) return -1;
    struct sp_dns_hdr *h = (struct sp_dns_hdr *)bufv;
    if (ntohs(h->flags) & 3) return -2; /* RCODE */
    int qd = ntohs(h->qd), an = ntohs(h->an);
    size_t off = 12;
    /* skip question section */
    for (int i = 0; i < qd; i++) {
        while (off < (size_t)nread) {
            uint8_t l = buf[off];
            if (l == 0) { off++; break; }
            if ((l & 0xC0) == 0xC0) { off += 2; break; }
            off += 1 + l;
        }
        off += 4;
    }
    int got = 0;
    for (int i = 0; i < an && off + 12 <= (size_t)nread; i++) {
        while (off < (size_t)nread) {
            uint8_t l = buf[off];
            if (l == 0) { off++; break; }
            if ((l & 0xC0) == 0xC0) { off += 2; break; }
            off += 1 + l;
        }
        if (off + 10 > (size_t)nread) break;
        uint16_t type = (buf[off] << 8) | buf[off+1];
        uint16_t cls  = (buf[off+2] << 8) | buf[off+3];
        uint16_t rdlen= (buf[off+8] << 8) | buf[off+9];
        off += 10;
        if (cls != 1 || off + rdlen > (size_t)nread) { off += rdlen; continue; }
        if (type == 1 && rdlen == 4 && !want_v6) {
            if (*na4 < 8) a4[(*na4)++] = *(struct in_addr *)&buf[off];
            got++;
        } else if (type == 28 && rdlen == 16) {
            if (*na6 < 8) a6[(*na6)++] = *(struct in6_addr *)&buf[off];
            got++;
        }
        off += rdlen;
    }
    return got;
}

/* one stubborn question to the servers, alternating A/AAAA */
static int sp_dns_lookup(const char *host, int want_v6,
                         struct in_addr a4[8], int *na4,
                         struct in6_addr a6[8], int *na6) {
    *na4 = 0; *na6 = 0;
    struct in_addr srv[4];
    int nsc = sp_dns_servers(srv);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    static uint16_t qid = 4242;
    qid = (uint16_t)(qid * 31 + 7);
    int rc = -1;
    for (int ai = 0; ai < nsc && rc < 0; ai++) {
        char q[SP_DNS_TXT_LEN], ans[4096];
        int qlen = sp_dns_encode(q, host, want_v6 ? 28 : 1, qid);
        if (qlen < 0) continue;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET; sa.sin_port = htons(53); sa.sin_addr = srv[ai];
        if (sendto(s, q, (size_t)qlen, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) continue;
        ssize_t r = recvfrom(s, ans, sizeof(ans), 0, NULL, NULL);
        if (r < 12) continue;
        rc = sp_dns_extract(ans, r, want_v6, a4, na4, a6, na6);
        if (rc == -2) break;      /* NXDOMAIN/name error: no use asking others */
        if (rc == 0) continue;    /* empty answer: try the next server anyway */
    }
    close(s);
    return (rc < 0) ? -1 : (*na4 + *na6 > 0 ? 0 : -1);
}

static int sp_dns_host_ok(const char *s) {
    return s && *s && strspn(s, ".-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == strlen(s);
}

static short sp_dns_port(const char *s) {
    static const struct { const char *n; short p; } tb[] = {
        {"http",80},{"http-alt",8080},{"https",443},{"ftp",21},{"ssh",22},
        {"smtp",25},{"domain",53},{"dns",53},{"ntp",123},{"pop3",110},
        {"imap",143},{"imaps",993},{"pop3s",995},{"submission",587},{NULL,0}
    };
    if (!s || !*s) return 0;
    if (s[0] >= '0' && s[0] <= '9') { short v = 0; while (*s >= '0' && *s <= '9') v = (short)(v * 10 + (*s++ - '0')); return v; }
    for (int i = 0; tb[i].n; i++) if (!strcmp(tb[i].n, s)) return tb[i].p;
    return 0;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res);
#undef getaddrinfo
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res) {
    static int (*SP_REAL(getaddrinfo))(const char *, const char *, const struct addrinfo *, struct addrinfo **) = NULL;
    SP_RESOLVE(getaddrinfo);
    if (!node || !sp_dns_host_ok(node)) return SP_REAL(getaddrinfo)(node, service, hints, res);
    if (node[0] >= '0' && node[0] <= '9') return SP_REAL(getaddrinfo)(node, service, hints, res); /* numeric */
    if (hints && hints->ai_family == AF_UNIX) return SP_REAL(getaddrinfo)(node, service, hints, res);
    struct in_addr a4[8]; struct in6_addr a6[8]; int na4 = 0, na6 = 0;
    int fam = hints ? hints->ai_family : AF_UNSPEC;
    if (fam == AF_INET) {
        if (sp_dns_lookup(node, 0, a4, &na4, a6, &na6) != 0) return EAI_AGAIN;
    } else if (fam == AF_INET6) {
        struct in_addr tmp4[8]; int tn4;
        if (sp_dns_lookup(node, 1, tmp4, &tn4, a6, &na6) != 0) return EAI_AGAIN;
    } else {
        /* AF_UNSPEC: A records ONLY. Emitting AAAA makes apt/curl attempt
         * v6 on Android networks that lack any v6 route (RST=113): apt's
         * http method then FAILS that URL mid-transaction instead of
         * falling back across families reliably. Android v6-usable LANs
         * were never achieved by proot's resolver either — parity. */
        if (sp_dns_lookup(node, 0, a4, &na4, a6, &na6) != 0) return EAI_AGAIN;
        if (na4 == 0) return EAI_AGAIN;
    }
    short port = sp_dns_port(service);
    struct addrinfo *head = NULL, **tail = &head;
    int count = 0;
    /* RFC 3484-bleed: emit A records BEFORE AAAA for AF_UNSPEC — many
     * Android networks have broken IPv6 (no route/113), python/curl/apt all
     * stop at the first connection failure without family alternatives. */
    for (int i = 0; i < na4 && count < 8; i++) {
        struct addrinfo *ai = calloc(1, sizeof(*ai));
        struct sockaddr_in *sin = calloc(1, sizeof(*sin));
        sin->sin_family = AF_INET; sin->sin_port = htons(port); sin->sin_addr = a4[i];
        ai->ai_flags = SP_AI_BRAND;
        ai->ai_family = AF_INET;
        ai->ai_socktype = hints ? hints->ai_socktype : 0;
        ai->ai_protocol = hints ? hints->ai_protocol : 0;
        ai->ai_addrlen = sizeof(*sin);
        ai->ai_addr = (struct sockaddr *)sin;
        ai->ai_canonname = sp_dns_strdup(node);
        *tail = ai; tail = &ai->ai_next; count++;
    }
    for (int i = 0; i < na6 && count < 16; i++) {
        struct addrinfo *ai = calloc(1, sizeof(*ai));
        struct sockaddr_in6 *sin6 = calloc(1, sizeof(*sin6));
        sin6->sin6_family = AF_INET6; sin6->sin6_port = htons(port); sin6->sin6_addr = a6[i];
        ai->ai_flags = SP_AI_BRAND;
        ai->ai_family = AF_INET6;
        ai->ai_socktype = hints ? hints->ai_socktype : 0;
        ai->ai_protocol = hints ? hints->ai_protocol : 0;
        ai->ai_addrlen = sizeof(*sin6);
        ai->ai_addr = (struct sockaddr *)sin6;
        ai->ai_canonname = sp_dns_strdup(node);
        *tail = ai; tail = &ai->ai_next; count++;
    }
    *res = head;
    return head ? 0 : EAI_AGAIN;
}

#define SP_AI_BRAND 0x7370726f  /* 'spro' */
void freeaddrinfo(struct addrinfo *res);
#undef freeaddrinfo
void freeaddrinfo(struct addrinfo *res) {
    static void (*SP_REAL(freeaddrinfo))(struct addrinfo *) = NULL;
    SP_RESOLVE(freeaddrinfo);
    /* libc chains may be contiguous slabs — NEVER free per-node. Ours carry
     * the brand flag; free branded nodes only, delegate tails to glibc. */
    struct addrinfo *own = res;
    while (own && (int)own->ai_flags == SP_AI_BRAND) {
        struct addrinfo *n = own->ai_next;
        if (own->ai_addr) free(own->ai_addr);
        if (own->ai_canonname) free(own->ai_canonname);
        free(own);
        own = n;
    }
    if (own && SP_REAL(freeaddrinfo)) SP_REAL(freeaddrinfo)(own);
}

struct hostent *gethostbyname(const char *name);
#undef gethostbyname
struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static char *aliases[1] = { NULL };
    static char *addrs[10];
    static struct in_addr buf4[8];
    struct in_addr a4[8]; struct in6_addr a6[8]; int na4, na6;
    if (sp_dns_lookup(name, 0, a4, &na4, a6, &na6) != 0) return NULL;
    for (int i = 0; i < na4 && i < 8; i++) { buf4[i] = a4[i]; addrs[i] = (char *)&buf4[i]; }
    addrs[na4 < 8 ? na4 : 8] = NULL;
    he.h_name = (char *)name; he.h_aliases = aliases;
    he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = addrs;
    return &he;
}

/* tempfile family: glibc's mkstemp uses internal __open calls — wraps the
 * public template entry points and creates the file under the translated
 * path. The caller's template string is left untouched (contains the guest
 * spelling); callers unlink/rename through the wrappers anyway, so all
 * model-visible semantics stay consistent. */
int mkstemp(char *tmpl) {
    static int (*SP_REAL(mkstemp))(char *) = NULL;
    SP_RESOLVE(mkstemp);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(tmpl, x);
    char big[SP_PATH_MAX];
    char *use = tmpl;
    if (p != tmpl) {
        strncpy(big, p, sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        use = big;
    }
    SP_TRACE("mkstemp", tmpl, use);
    int fd = SP_REAL(mkstemp)(use);
    /* glibc contract: substitute trailing XXXXXX IN THE CALLER'S BUFFER.
     * `use` is the (possibly longer, translated) stack template the real
     * library substituted its suffix into; its tail 6 chars are the shared
     * semantic — mirror them back into the caller's guest-spelled buffer.
     * All later opens/unlinks through any wrapper on the guest spelling
     * reach the same real file. */
    if (fd >= 0) {
        size_t tl = strlen(tmpl), ul = strlen(use);
        if (ul >= 6 && tl >= 6) memcpy(tmpl + tl - 6, use + ul - 6, 6);
    }
    return fd;
}

int mkostemp(char *tmpl, int flags) {
    static int (*SP_REAL(mkostemp))(char *, int) = NULL;
    SP_RESOLVE(mkostemp);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(tmpl, x);
    char big[SP_PATH_MAX];
    char *use = tmpl;
    if (p != tmpl) {
        strncpy(big, p, sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        use = big;
    }
    SP_TRACE("mkostemp", tmpl, use);
    int fd = SP_REAL(mkostemp)(use, flags);
    if (fd >= 0) {
        size_t tl = strlen(tmpl), ul = strlen(use);
        if (ul >= 6 && tl >= 6) memcpy(tmpl + tl - 6, use + ul - 6, 6);
    }
    return fd;
}

/* glibc _FILE_OFFSET_BITS=64 program image aliases: the SAME contract
 * (substitute XXXXXX in caller buffer, tail-6 mirror back after the real
 * call writes the substituted suffix into the translated `big`). git, gcc
 * and most of dpkg's toolchain bind to the 64 spellings directly — without
 * these wrappers the template lands in glibc UNtranslated (observed on
 * device: `git clone` -> "/tmp/HW/.git/tXXXXXX: No such file or dir"). */
int mkstemp64(char *tmpl) {
    static int (*SP_REAL(mkstemp64))(char *) = NULL;
    SP_RESOLVE(mkstemp64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(tmpl, x);
    char big[SP_PATH_MAX];
    char *use = tmpl;
    if (p != tmpl) {
        strncpy(big, p, sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        use = big;
    }
    SP_TRACE("mkstemp64", tmpl, use);
    int fd = SP_REAL(mkstemp64)(use);
    if (fd >= 0) {
        size_t tl = strlen(tmpl), ul = strlen(use);
        if (ul >= 6 && tl >= 6) memcpy(tmpl + tl - 6, use + ul - 6, 6);
    }
    return fd;
}
int mkostemp64(char *tmpl, int flags) {
    static int (*SP_REAL(mkostemp64))(char *, int) = NULL;
    SP_RESOLVE(mkostemp64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(tmpl, x);
    char big[SP_PATH_MAX];
    char *use = tmpl;
    if (p != tmpl) {
        strncpy(big, p, sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        use = big;
    }
    SP_TRACE("mkostemp64", tmpl, use);
    int fd = SP_REAL(mkostemp64)(use, flags);
    if (fd >= 0) {
        size_t tl = strlen(tmpl), ul = strlen(use);
        if (ul >= 6 && tl >= 6) memcpy(tmpl + tl - 6, use + ul - 6, 6);
    }
    return fd;
}
char *mkdtemp64(char *tmpl) {
    static char *(*SP_REAL(mkdtemp64))(char *) = NULL;
    static char *(*SP_REAL(mkdtemp))(char *) = NULL;
    SP_RESOLVE(mkdtemp64);
    if (!SP_REAL(mkdtemp64)) { SP_RESOLVE(mkdtemp); }
    char *(*realdt)(char *) = SP_REAL(mkdtemp64) ? SP_REAL(mkdtemp64) : SP_REAL(mkdtemp);
    if (!realdt) { errno = ENOSYS; return NULL; }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(tmpl, x);
    char big[SP_PATH_MAX];
    char *use = tmpl;
    if (p != tmpl) {
        strncpy(big, p, sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        use = big;
    }
    SP_TRACE("mkdtemp64", tmpl, use);
    char *rd = realdt(use);
    if (rd) {
        size_t tl = strlen(tmpl), ul = strlen(use);
        if (ul >= 6 && tl >= 6) memcpy(tmpl + tl - 6, use + ul - 6, 6);
        return tmpl;
    }
    return NULL;
}

/* mutation paths (apt/dpkg needs): chmod/chown/link/truncate families */
int chmod(const char *path, mode_t mode) {
    static int (*SP_REAL(chmod))(const char *, mode_t) = NULL;
    SP_RESOLVE(chmod);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("chmod", path, p);
    int rc = SP_REAL(chmod)(p, mode);
    sp_trace_line("TRM chmod pid=%d '%s' -> '%s' mode=%04o rc=%d errno=%d\n", (int)getpid(),
                    path ? path : "(null)", p, (unsigned)mode & 07777, rc, rc < 0 ? errno : 0);
    return rc;
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    static int (*SP_REAL(fchmodat))(int, const char *, mode_t, int) = NULL;
    SP_RESOLVE(fchmodat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(fchmodat)(dirfd, path, mode, flags);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_NOFOLLOW) ? sp_translate_l(path, x) : sp_translate_x(path, x);
    SP_TRACE("fchmodat", path, p);
    sp_trace_line("TRM fchmodat pid=%d '%s' -> '%s' mode=%04o flags=%x\n", (int)getpid(),
                    path ? path : "(null)", p, (unsigned)mode & 07777, flags);
    return SP_REAL(fchmodat)(dirfd, p, mode, flags);
}

/* ---- fakeroot identity family (ADR-0011/M3.1 parity with proot -0) ----
 * apt's privilege-drop chain (setgroups/setresgid/setresuid/seteuid...)
 * must BELIEVE it succeeded under -0. Android TRAPs the set*id syscalls
 * for untrusted apps (supervisor swallows SIGSYS: 143-152,159 -> 0), and
 * the non-trapped remainder (seteuid=148 & friends) are answered here at
 * the PLT so the guest never sees a real kernel EPERM. Without -0 every
 * call passes through untouched. */
static int sp_fakeroot_on(void) {
    static int fr = -1;
    if (fr < 0) fr = getenv("SPROUT_FAKEROOT") != NULL;
    return fr;
}

/* ---- fakeroot ownership spoof (proot --root-id parity) ---------------
 * Under -0 the guest must see ITSELF-own files as root-owned, otherwise
 * tools that compare st_uid against getuid() trip over a mismatch
 * (observed on device: git refuses any repo via 'dubious ownership').
 * Rule out of proot: files whose REAL owner is the host-application
 * uid/gid read back as 0:0 under fakeroot; anything else keeps kernel
 * truth. */
static void sp_spoof_uid_gid(uid_t *u, gid_t *g) {
    if (!sp_fakeroot_on()) return;
    static uid_t he = (uid_t)-1;
    static gid_t hg = (gid_t)-1;
    if (he == (uid_t)-1) {
        if (!sp_real_geteuid) sp_real_geteuid = (uid_t (*)(void))sp_sym("geteuid");
        if (!sp_real_getegid) sp_real_getegid = (gid_t (*)(void))sp_sym("getegid");
        he = sp_real_geteuid ? sp_real_geteuid() : 0;
        hg = sp_real_getegid ? sp_real_getegid() : 0;
    }
    if (u && *u == he) *u = g_fake_uid;
    if (g && *g == hg) *g = g_fake_gid;
}

/* proot link2symlink-parity registry: hardlinks emulated as symlinks/copies
 * leave nlink==1, which shadow's lock protocol (useradd: /etc/passwd.<pid>
 * -> /etc/passwd.lock demands nlink==2) reads as "lock file already used".
 * proot keeps an in-memory registry of symlinked-as-hardlink paths and
 * reports nlink=2 via its stat wrappers; mirror it, process-local exec-live
 * (lock+check happen inside one tool lifetime). */
#define SP_HREG_MAX 256
static char sp_hreg_paths[SP_HREG_MAX][SP_PATH_MAX];
static int  sp_hreg_n = 0;
static int sp_hreg_hit(const char *gpath) {
    if (!gpath) return 0;
    for (int i = 0; i < sp_hreg_n; i++)
        if (strcmp(sp_hreg_paths[i], gpath) == 0) return 1;
    return 0;
}
static void sp_hreg_note(const char *gpath) {
    if (!gpath || !*gpath || *gpath != '/' || sp_hreg_n >= SP_HREG_MAX) return;
    if (sp_hreg_hit(gpath)) return;
    snprintf(sp_hreg_paths[sp_hreg_n++], SP_PATH_MAX, "%s", gpath);
}

/* state backing the fake answers; the get* wrappers below report
 * whatever the most recent fake set*id call promised. */
uid_t  g_fake_uid = 0;
gid_t  g_fake_gid = 0;
gid_t  g_fake_groups[64];
int    g_fake_ngroups = 0;
int setuid(uid_t uid) { static int (*SP_REAL(setuid))(uid_t)=NULL; SP_RESOLVE(setuid); if (sp_fakeroot_on()) { g_fake_uid = uid; return 0; } return SP_REAL(setuid)(uid); }
int seteuid(uid_t uid) { static int (*SP_REAL(seteuid))(uid_t)=NULL; SP_RESOLVE(seteuid); if (sp_fakeroot_on()) { g_fake_uid = uid; return 0; } return SP_REAL(seteuid)(uid); }
int setgid(gid_t gid) { static int (*SP_REAL(setgid))(gid_t)=NULL; SP_RESOLVE(setgid); if (sp_fakeroot_on()) { g_fake_gid = gid; g_fake_groups[0] = gid; g_fake_ngroups = 1; return 0; } return SP_REAL(setgid)(gid); }
int setegid(gid_t gid) { static int (*SP_REAL(setegid))(gid_t)=NULL; SP_RESOLVE(setegid); if (sp_fakeroot_on()) { g_fake_gid = gid; return 0; } return SP_REAL(setegid)(gid); }
int setreuid(uid_t ruid, uid_t euid) { static int (*SP_REAL(setreuid))(uid_t,uid_t)=NULL; SP_RESOLVE(setreuid); if (sp_fakeroot_on()) { if (euid != (uid_t)-1) g_fake_uid = euid; return 0; } return SP_REAL(setreuid)(ruid,euid); }
int setregid(gid_t rgid, gid_t egid) { static int (*SP_REAL(setregid))(gid_t,gid_t)=NULL; SP_RESOLVE(setregid); if (sp_fakeroot_on()) { if (egid != (gid_t)-1) g_fake_gid = egid; return 0; } return SP_REAL(setregid)(rgid,egid); }
int setresuid(uid_t ruid, uid_t euid, uid_t suid) { static int (*SP_REAL(setresuid))(uid_t,uid_t,uid_t)=NULL; SP_RESOLVE(setresuid); if (sp_fakeroot_on()) { if (euid != (uid_t)-1) g_fake_uid = euid; return 0; } return SP_REAL(setresuid)(ruid,euid,suid); }
int setresgid(gid_t rgid, gid_t egid, gid_t sgid) { static int (*SP_REAL(setresgid))(gid_t,gid_t,gid_t)=NULL; SP_RESOLVE(setresgid); if (sp_fakeroot_on()) { if (egid != (gid_t)-1) g_fake_gid = egid; return 0; } return SP_REAL(setresgid)(rgid,egid,sgid); }
int setgroups(size_t n, const gid_t *g) { static int (*SP_REAL(setgroups))(size_t,const gid_t*)=NULL; SP_RESOLVE(setgroups); if (sp_fakeroot_on()) { unsigned c = (n > 64) ? 64 : n; for (unsigned k = 0; k < c; k++) g_fake_groups[k] = g[k]; g_fake_ngroups = (int)c; return 0; } return SP_REAL(setgroups)(n,g); }
int initgroups(const char *u, gid_t g) { static int (*SP_REAL(initgroups))(const char*,gid_t)=NULL; SP_RESOLVE(initgroups); if (sp_fakeroot_on()) { (void)u; g_fake_groups[0] = g; g_fake_ngroups = 1; return 0; } return SP_REAL(initgroups)(u,g); }
/* apt DropPrivsOrDie verifies with getgroups() that no foreign
 * supplementary groups remain after the drop; under -0 the truthful
 * fake is the root profile: zero supplementary groups. */
int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid) {
    static int (*SP_REAL(getresuid))(uid_t*,uid_t*,uid_t*)=NULL; SP_RESOLVE(getresuid);
    if (sp_fakeroot_on()) { *ruid = g_fake_uid; *euid = g_fake_uid; *suid = g_fake_uid; return 0; }
    return SP_REAL(getresuid)(ruid,euid,suid);
}
int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid) {
    static int (*SP_REAL(getresgid))(gid_t*,gid_t*,gid_t*)=NULL; SP_RESOLVE(getresgid);
    if (sp_fakeroot_on()) { *rgid = g_fake_gid; *egid = g_fake_gid; *sgid = g_fake_gid; return 0; }
    return SP_REAL(getresgid)(rgid,egid,sgid);
}
int getgroups(int n, gid_t *list) {
    static int (*SP_REAL(getgroups))(int,gid_t*)=NULL; SP_RESOLVE(getgroups);
    if (sp_fakeroot_on()) {
        if (n <= 0) return g_fake_ngroups;
        int c = (g_fake_ngroups < n) ? g_fake_ngroups : n;
        for (int k = 0; k < c; k++) list[k] = g_fake_groups[k];
        return c;
    }
    return SP_REAL(getgroups)(n,list);
}
int chown(const char *path, uid_t uid, gid_t gid) {
    static int (*SP_REAL(chown))(const char *, uid_t, gid_t) = NULL;
    static int fake_root = -1;
    if (fake_root < 0) fake_root = getenv("SPROUT_FAKEROOT") != NULL;
    SP_RESOLVE(chown);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("chown", path, p);
    /* proot -0 parity: under fake-root the guest happily chown()s anything
     * to root; the host keeps ownership (uid of the Android app) */
    if (fake_root) return 0;
    return SP_REAL(chown)(p, uid, gid);
}
int lchown(const char *path, uid_t uid, gid_t gid) {
    static int (*SP_REAL(lchown))(const char *, uid_t, gid_t) = NULL;
    static int fake_root2 = -1;
    if (fake_root2 < 0) fake_root2 = getenv("SPROUT_FAKEROOT") != NULL;
    SP_RESOLVE(lchown);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("lchown", path, p);
    if (fake_root2) return 0;
    return SP_REAL(lchown)(p, uid, gid);
}
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) {
    static int (*SP_REAL(fchownat))(int, const char *, uid_t, gid_t, int) = NULL;
    static int fake_root3 = -1;
    if (fake_root3 < 0) fake_root3 = getenv("SPROUT_FAKEROOT") != NULL;
    SP_RESOLVE(fchownat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(fchownat)(dirfd, path, uid, gid, flags);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_NOFOLLOW) ? sp_translate_l(path, x) : sp_translate_x(path, x);
    SP_TRACE("fchownat", path, p);
    if (fake_root3) return 0;
    return SP_REAL(fchownat)(dirfd, p, uid, gid, flags);
}
int fchown(int fd, uid_t uid, gid_t gid) {
    static int (*SP_REAL(fchown))(int, uid_t, gid_t) = NULL;
    static int fake_root4 = -1;
    if (fake_root4 < 0) fake_root4 = getenv("SPROUT_FAKEROOT") != NULL;
    SP_RESOLVE(fchown);
    if (fake_root4) return 0;
    return SP_REAL(fchown)(fd, uid, gid);
}

/* Ephemeral-journal hardlink hazard (SELinux EPERM on hardlinks under
 * /data/data/.../files): callers like git do link(tmp -> target)+unlink(tmp);
 * a SYMLINK fallback dangles the moment tmp is unlinked (observed: git
 * "not a valid object"). proot's --link2symlink story only covers the
 * persistent-backup case anyway. Robust fallback: MATERIALIZE the target
 * as a COPY of the source (content + mode), then the unlink-journal is
 * harmless. Only when copying genuinely fails do we degrade to symlink. */
/* proot-shape .l2s fallback (preferred over copy): content is RENAMED to
 * a hidden $ROOT/.l2s file and both src+dst become symlinks to it. Wins
 * where copy loses: apps that keep writing archive content through the
 * tmp's fd AFTER link() (glibc locale-archive builder), and the classic
 * unlink-journal (git objects) — unlink(tmp) then only kills a symlink,
 * the dst symlink still reads the moved content. */
static int sp_link_fallback_l2s(const char *p1, const char *p2) {
    if (!g_cfg.rootfs_len) return -1;
    static unsigned long l2s_n = 0;
    char hid[SP_PATH_MAX], tmp[SP_PATH_MAX];
    const char *base = strrchr(p1, '/');
    base = base ? base + 1 : p1;
    struct stat lst;
    if (lstat(p1, &lst) != 0) return -1;
    if (S_ISLNK(lst.st_mode)) {
        ssize_t rn = readlink(p1, tmp, sizeof(tmp) - 1);
        if (rn <= 0) return -1;
        tmp[rn] = 0;
        if (strstr(tmp, "/.l2s/.l2s.")) {
            snprintf(hid, sizeof hid, "%s", tmp);
            goto have_hidden;
        }
    }
    if (snprintf(hid, sizeof hid, "%s/.l2s/.l2s.%s.%lx%lx", g_cfg.rootfs, base,
                 (unsigned long)getpid(), (unsigned long)++l2s_n) >= (int)sizeof hid)
        return -1; /* long-path safety: silent truncation below would rename to the WRONG file */
    if (snprintf(tmp, sizeof tmp, "%s/.l2s", g_cfg.rootfs) >= (int)sizeof tmp)
        return -1;
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    if (rename(p1, hid) != 0) return -1;
    if (symlink(hid, p1) != 0) {
        rename(hid, p1);
        return -1;
    }
have_hidden:
    unlink(p2);
    if (symlink(hid, p2) != 0) return -1;
    return 0;
}

static int sp_link_fallback_copy(const char *p1, const char *p2) {
    int si = open(p1, O_RDONLY);
    if (si < 0) return -1;
    struct stat sst;
    if (fstat(si, &sst) != 0) { close(si); errno = EIO; return -1; }
    int di = open(p2, O_WRONLY | O_CREAT | O_EXCL, sst.st_mode & 07777);
    if (di < 0) { close(si); return -1; }
    char buf[65536];
    ssize_t n;
    while ((n = read(si, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(di, buf + off, (size_t)(n - off));
            if (w < 0) { close(si); close(di); unlink(p2); return -1; }
            off += w;
        }
    }
    fsync(di);
    close(si);
    int rc = close(di);
    if (n < 0 || rc != 0) { errno = EIO; unlink(p2); return -1; }
    return 0;
}

int link(const char *oldpath, const char *newpath) {
    static int (*SP_REAL(link))(const char *, const char *) = NULL;
    static ssize_t (*SP_REAL(symlink))(const char *, const char *) = NULL;
    SP_RESOLVE(link);
    char x1[SP_PATH_MAX], x2[SP_PATH_MAX];
    const char *p1 = sp_translate_x(oldpath, x1);
    const char *p2 = sp_translate_l(newpath, x2);
    SP_TRACE("link", oldpath, p1);
    SP_TRACE("link->", newpath, p2);
    int rc = SP_REAL(link)(p1, p2);
    /* SELinux denies hardlinks on /data/data/.../files: fallback = copy
     * (journal-safe, see helper), symlink only as last resort. */
    if (rc != 0 && (errno == EPERM || errno == EACCES) && getenv("SPROUT_LINK2SYMLINK")) {
        if (sp_link_fallback_l2s(p1, p2) == 0) rc = 0;
        else {
            int rc2 = sp_link_fallback_copy(p1, p2);
            if (rc2 == 0) rc = 0;
            else {
                if (!SP_REAL(symlink)) SP_REAL(symlink) = dlsym(RTLD_NEXT, "symlink");
                if (SP_REAL(symlink)) rc = SP_REAL(symlink)(p1, p2);
            }
        }
    }
    if (rc == 0) { sp_hreg_note(newpath); sp_hreg_note(oldpath); } /* shadow nlink==2 lock quote ref: proot registry */
    return rc;
}
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    static int (*SP_REAL(linkat))(int, const char *, int, const char *, int) = NULL;
    static int (*SP_REAL(symlinkat))(const char *, int, const char *) = NULL;
    SP_RESOLVE(linkat);
    if ((olddirfd != -100 /*AT_FDCWD*/ && oldpath && oldpath[0] != '/') ||
        (newdirfd != -100 /*AT_FDCWD*/ && newpath && newpath[0] != '/')) {
        int rc = SP_REAL(linkat)(olddirfd, oldpath, newdirfd, newpath, flags);
        /* SELinux still laughs at hardlinks even inside our db dir:
         * journal-safe copy, mirroring the notify/governed fallbacks. */
        if (rc != 0 && (errno == EPERM || errno == EACCES) && getenv("SPROUT_LINK2SYMLINK")) {
            char ab[SP_PATH_MAX], nb[SP_PATH_MAX];
            ab[0] = nb[0] = 0;
            if (oldpath[0] != '/') {
                char l1[128];
                snprintf(l1, sizeof l1, "/proc/self/fd/%d", olddirfd);
                ssize_t n = readlink(l1, ab, sizeof(ab) - 1);
                if (n > 0) { ab[n] = 0; strncat(ab, "/", sizeof(ab) - strlen(ab) - 1); strncat(ab, oldpath, sizeof(ab) - strlen(ab) - 1); }
            } else snprintf(ab, sizeof ab, "%s", oldpath);
            if (newpath[0] != '/') {
                char l2[128];
                snprintf(l2, sizeof l2, "/proc/self/fd/%d", newdirfd);
                ssize_t n = readlink(l2, nb, sizeof(nb) - 1);
                if (n > 0) { nb[n] = 0; strncat(nb, "/", sizeof(nb) - strlen(nb) - 1); strncat(nb, newpath, sizeof(nb) - strlen(nb) - 1); }
            } else snprintf(nb, sizeof nb, "%s", newpath);
            if (ab[0] && nb[0] &&
                (sp_link_fallback_l2s(ab, nb) == 0 || sp_link_fallback_copy(ab, nb) == 0))
                rc = 0;
        }
        return rc;
    }
    char x1[SP_PATH_MAX], x2[SP_PATH_MAX];
    const char *p1 = sp_translate_x(oldpath, x1);
    const char *p2 = sp_translate_l(newpath, x2);
    SP_TRACE("linkat", oldpath, p1);
    SP_TRACE("linkat->", newpath, p2);
    int rc = SP_REAL(linkat)(olddirfd, p1, newdirfd, p2, flags);
    if (rc != 0 && (errno == EPERM || errno == EACCES) && getenv("SPROUT_LINK2SYMLINK")) {
        if (sp_link_fallback_l2s(p1, p2) == 0) rc = 0;
        else {
            int rc2 = sp_link_fallback_copy(p1, p2);
            if (rc2 == 0) rc = 0;
            else {
                if (!SP_REAL(symlinkat)) SP_REAL(symlinkat) = dlsym(RTLD_NEXT, "symlinkat");
                if (SP_REAL(symlinkat)) rc = SP_REAL(symlinkat)(p1, AT_FDCWD, p2);
            }
        }
    }
    if (rc == 0) { sp_hreg_note(newpath); sp_hreg_note(oldpath); }
    return rc;
}
int truncate(const char *path, off_t length) {
    static int (*SP_REAL(truncate))(const char *, off_t) = NULL;
    SP_RESOLVE(truncate);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("truncate", path, p);
    return SP_REAL(truncate)(p, length);
}
int utimes(const char *path, const struct timeval times[2]) {
    static int (*SP_REAL(utimes))(const char *, const struct timeval *) = NULL;
    SP_RESOLVE(utimes);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("utimes", path, p);
    return SP_REAL(utimes)(p, times);
}
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    static int (*SP_REAL(utimensat))(int, const char *, const struct timespec *, int) = NULL;
    SP_RESOLVE(utimensat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(utimensat)(dirfd, path, times, flags);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_NOFOLLOW) ? sp_translate_l(path, x) : sp_translate_x(path, x);
    SP_TRACE("utimensat", path, p);
    return SP_REAL(utimensat)(dirfd, p, times, flags);
}

/* BSD timeval APIs. dpkg's tar postprocessing ALSO imports lutimes()
 * (existing utimes wrapper above covers only the follow-target form);
 * glibc implements them via internal utimensat() calls, which bypass all
 * PLT interception — same internal-syscall blind spot as ifstream/fopen. */
int lutimes(const char *path, const struct timeval tv[2]) {
    static int (*SP_REAL(lutimes))(const char *, const struct timeval *) = NULL;
    SP_RESOLVE(lutimes);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("lutimes", path, p);
    return SP_REAL(lutimes)(p, tv);
}

/* filesystem-space probes (apt's 'determine free space' step) */
#include <sys/statvfs.h>
int statvfs(const char *path, struct statvfs *buf) {
    static int (*SP_REAL(statvfs))(const char *, struct statvfs *) = NULL;
    SP_RESOLVE(statvfs);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("statvfs", path, p);
    return SP_REAL(statvfs)(p, buf);
}
int statvfs64(const char *path, struct statvfs64 *buf) {
    static int (*SP_REAL(statvfs64))(const char *, struct statvfs64 *) = NULL;
    SP_RESOLVE(statvfs64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("statvfs64", path, p);
    return SP_REAL(statvfs64)(p, buf);
}

/* statfs/statfs64 — mozilla's nsLocalFile uses STATFS=statfs on LINUX
 * (nsLocalFileUnix.cpp selects statvfs only when !defined(LINUX)), while
 * coreutils df(1) rides statvfs — hence the false-parity probe: df(1)
 * worked, about:support's diskSpaceAvailable silently broke against the
 * untranslated guest path and returned NS_ERROR_FAILURE (user report,
 * about:support all-blank, 2026-08-13).
 * aarch64 glibc: statfs64 = the LFS alias; both exported. */
#include <sys/vfs.h>
int statfs(const char *path, struct statfs *buf) {
    static int (*SP_REAL(statfs))(const char *, struct statfs *) = NULL;
    SP_RESOLVE(statfs);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("statfs", path, p);
    return SP_REAL(statfs)(p, buf);
}
int statfs64(const char *path, struct statfs64 *buf) {
    static int (*SP_REAL(statfs64))(const char *, struct statfs64 *) = NULL;
    SP_RESOLVE(statfs64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("statfs64", path, p);
    return SP_REAL(statfs64)(p, buf);
}

/* xattr family (path-taking members) — python os.listxattr(follow_symlinks=False)
 * goes through llistxattr, meson's shutil.copy2/copystat dies with ENOENT on
 * translated guest paths that absolutely exist in the guest ns (message
 * identically misleading: 'FileNotFoundError [...] /root/mesa/bin/drm-shim.py'
 * after a SUCCESSFUL copy2 content step, mesa setup fail 2026-08-13). The
 * The fxattr variants take fd arguments whose kernel meaning is already
 * host-real: they pass through untouched, untranslated. */
#include <sys/xattr.h>
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    static int (*SP_REAL(setxattr))(const char *, const char *, const void *, size_t, int) = NULL;
    SP_RESOLVE(setxattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("setxattr", path, p);
    return SP_REAL(setxattr)(p, name, value, size, flags);
}
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    static int (*SP_REAL(lsetxattr))(const char *, const char *, const void *, size_t, int) = NULL;
    SP_RESOLVE(lsetxattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("lsetxattr", path, p);
    return SP_REAL(lsetxattr)(p, name, value, size, flags);
}
ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
    static ssize_t (*SP_REAL(getxattr))(const char *, const char *, void *, size_t) = NULL;
    SP_RESOLVE(getxattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("getxattr", path, p);
    return SP_REAL(getxattr)(p, name, value, size);
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
    static ssize_t (*SP_REAL(lgetxattr))(const char *, const char *, void *, size_t) = NULL;
    SP_RESOLVE(lgetxattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("lgetxattr", path, p);
    return SP_REAL(lgetxattr)(p, name, value, size);
}
ssize_t listxattr(const char *path, char *list, size_t size) {
    static ssize_t (*SP_REAL(listxattr))(const char *, char *, size_t) = NULL;
    SP_RESOLVE(listxattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("listxattr", path, p);
    return SP_REAL(listxattr)(p, list, size);
}
ssize_t llistxattr(const char *path, char *list, size_t size) {
    static ssize_t (*SP_REAL(llistxattr))(const char *, char *, size_t) = NULL;
    SP_RESOLVE(llistxattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("llistxattr", path, p);
    return SP_REAL(llistxattr)(p, list, size);
}
int removexattr(const char *path, const char *name) {
    static int (*SP_REAL(removexattr))(const char *, const char *) = NULL;
    SP_RESOLVE(removexattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("removexattr", path, p);
    return SP_REAL(removexattr)(p, name);
}
int lremovexattr(const char *path, const char *name) {
    static int (*SP_REAL(lremovexattr))(const char *, const char *) = NULL;
    SP_RESOLVE(lremovexattr);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("lremovexattr", path, p);
    return SP_REAL(lremovexattr)(p, name);
}

/* stat-at family: GNU coreutils cp/c(LT)-h directives use fstatat; a missing
 * wrapper makes the SOURCE lookup fail with ENOENT on the host tree */
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    static int (*SP_REAL(fstatat))(int, const char *, struct stat *, int) = NULL;
    SP_RESOLVE(fstatat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(fstatat)(dirfd, path, st, flags);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_NOFOLLOW) ? sp_translate_l(path, x) : sp_translate_x(path, x);
    SP_TRACE("fstatat", path, p);
    int rc = SP_REAL(fstatat)(dirfd, p, st, flags);
    if (rc == 0) { sp_spoof_uid_gid(&st->st_uid, &st->st_gid); if (sp_hreg_hit(path) && st->st_nlink == 1) st->st_nlink = 2; }
    return rc;
}
int newfstatat(int dirfd, const char *path, struct stat *st, int flags) {
    static int (*SP_REAL(newfstatat))(int, const char *, struct stat *, int) = NULL;
    SP_RESOLVE(newfstatat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(newfstatat)(dirfd, path, st, flags);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_NOFOLLOW) ? sp_translate_l(path, x) : sp_translate_x(path, x);
    SP_TRACE("newfstatat", path, p);
    int rc = SP_REAL(newfstatat)(dirfd, p, st, flags);
    if (rc == 0) { sp_spoof_uid_gid(&st->st_uid, &st->st_gid); if (sp_hreg_hit(path) && st->st_nlink == 1) st->st_nlink = 2; }
    return rc;
}

char *mkdtemp(char *tmpl) {
    static char *(*SP_REAL(mkdtemp))(char *) = NULL;
    SP_RESOLVE(mkdtemp);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(tmpl, x);
    char big[SP_PATH_MAX];
    char *use = tmpl;
    if (p != tmpl) {
        strncpy(big, p, sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        use = big;
    }
    char *r = SP_REAL(mkdtemp)(use);
    if (!r) return NULL;
    if (use != tmpl) {
        size_t tl = strlen(tmpl), ul = strlen(use);
        if (ul >= 6 && tl >= 6) memcpy(tmpl + tl - 6, use + ul - 6, 6);
        /* the caller continues with the guest spelling; wrappers translate */
        return tmpl;
    }
    return r;
}

#include <stdio.h>
#include <stdbool.h>
FILE *tmpfile(void) {
    static FILE *(*SP_REAL(tmpfile))(void) = NULL;
    SP_RESOLVE(tmpfile);
    return SP_REAL(tmpfile)();   /* glibc manages O_TMPFILE in rootfs/tmp */
}

struct hostent *gethostbyname2(const char *name, int af);
#undef gethostbyname2
struct hostent *gethostbyname2(const char *name, int af) {
    if (af == AF_INET) return gethostbyname(name);
    return NULL; /* IPv6 callers fall through to getaddrinfo */
}

int __open64(const char *path, int flags, ...) {
    static int (*SP_REAL(__open64))(const char *, int, ...) = NULL;
    SP_RESOLVE(__open64);
    if (!SP_REAL(__open64)) SP_REAL(__open64) = dlsym(RTLD_NEXT, "open64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("__open64", path, p);
    return SP_REAL(__open64)(p, flags, mode);
}

int __openat64(int dirfd, const char *path, int flags, ...) {
    static int (*SP_REAL(__openat64))(int, const char *, int, ...) = NULL;
    SP_RESOLVE(__openat64);
    if (!SP_REAL(__openat64)) SP_REAL(__openat64) = dlsym(RTLD_NEXT, "openat64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("__openat64", path, p);
    return SP_REAL(__openat64)(dirfd, p, flags, mode);
}

int faccessat2(int dirfd, const char *path, int mode, int flags) {
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("faccessat2", path, p);
    return sp_emulate_access_impl(dirfd, p, mode, flags, flags & AT_EACCESS);
}

/* ---- setfsuid/setfsgid (203/204) identity emulation -------------------
 * ncurses `_nc_safe_fopen()` probes fsuid by calling setfsuid(getuid())
 * unconditionally (Linux has no getfsuid(2)); Android blocks BOTH raw
 * syscalls for untrusted apps — without emulation every readline-using
 * guest dies at interactive shell startup (verified via strace -k:
 * _nc_safe_fopen on every terminfo read).
 *
 * Rootless truth table:
 *   setfsuid(uid) where uid == current uid/euid  → the kernel computes
 *     this as a no-op for unprivileged callers AND returns the prior
 *     fsuid (== uid, since fsuid==uid throughout a rootless guest);
 *   setfsuid(other) → kernel answer for an unprivileged caller is EPERM.
 * Emulating exactly that — nothing else — keeps the contract truthful. */
#include <sys/fsuid.h>
int setfsuid(uid_t fsuid) {
    sp_resolve_access_emul();
    if (sp_real_getuid && sp_real_geteuid && (fsuid == sp_real_getuid() || fsuid == sp_real_geteuid()))
        return (int)fsuid;              /* prior fsuid == uid in a rootless guest */
    errno = EPERM; return -1;           /* kernel's honest answer when switching */
}
int setfsgid(gid_t fsgid) {
    sp_resolve_access_emul();
    if (sp_real_getgid && sp_real_getegid && (fsgid == sp_real_getgid() || fsgid == sp_real_getegid()))
        return (int)fsgid;
    errno = EPERM; return -1;
}

int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *buf) {
    static int (*SP_REAL(statx))(int, const char *, int, unsigned int, struct statx *) = NULL;
    SP_RESOLVE(statx);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(statx)(dirfd, path, flags, mask, buf);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_FOLLOW) ? sp_translate_x(path, x) : sp_translate_l(path, x);
    SP_TRACE("statx", path, p);
    int rc = SP_REAL(statx)(dirfd, p, flags, mask, buf);
    if (rc == 0) { sp_spoof_uid_gid(&buf->stx_uid, &buf->stx_gid); if (sp_hreg_hit(path) && buf->stx_nlink == 1) buf->stx_nlink = 2; }
    return rc;
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    static int (*SP_REAL(mkdirat))(int, const char *, mode_t) = NULL;
    SP_RESOLVE(mkdirat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(mkdirat)(dirfd, path, mode);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("mkdirat", path, p);
    return SP_REAL(mkdirat)(dirfd, p, mode);
}

int unlinkat(int dirfd, const char *path, int flags) {
    static int (*SP_REAL(unlinkat))(int, const char *, int) = NULL;
    SP_RESOLVE(unlinkat);
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(unlinkat)(dirfd, path, flags);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("unlinkat", path, p);
    return SP_REAL(unlinkat)(dirfd, p, flags);
}

/* answer /proc/self/exe (and the pid-spelled twin) with SPROUT_EXE;
 * see sp_stamp_exe above for the loader-chain fallout being mended. */
static int sp_self_exe_answer(const char *path, char *buf, size_t bufsiz) {
    if (!path) return -1;
    char m[96];
    int ok = !strcmp(path, "/proc/self/exe");
    if (!ok) {
        snprintf(m, sizeof(m), "/proc/%ld/exe", (long)getpid());
        ok = !strcmp(path, m);
    }
    if (!ok) return -1;
    const char *e = getenv("SPROUT_EXE");
    if (!e || !*e) return -1;
    size_t l = strlen(e);
    if (l > bufsiz) l = bufsiz;
    memcpy(buf, e, l);
    return (int)l;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*SP_REAL(readlinkat))(int, const char *, char *, size_t) = NULL;
    SP_RESOLVE(readlinkat);
    {
        int se = sp_self_exe_answer(path, buf, bufsiz);
        if (se > 0) return se;
    }
    if (dirfd != -100 /*AT_FDCWD*/ && path && path[0] != '/')
        return SP_REAL(readlinkat)(dirfd, path, buf, bufsiz);
    if (bufsiz == 0) return SP_REAL(readlinkat)(dirfd, path, buf, bufsiz);
    char x[SP_PATH_MAX];
    char target[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    ssize_t n = SP_REAL(readlinkat)(dirfd, p, target, SP_PATH_MAX - 1);
    if (n <= 0) return n;
    target[n] = '\0';
    n = (ssize_t)sp_reverse(&g_cfg, target, target, sizeof(target));
    if ((size_t)n > bufsiz) n = (ssize_t)bufsiz;
    memcpy(buf, target, (size_t)n);
    SP_TRACE("readlinkat", path, target);
    return n;
}

/* glibc's FORTIFY re-entry of readlink is a distinct PLT symbol and slips
 * past the plain readlink() wrapper; firefox-esr's stub comes via this
 * __readlink_chk path, so it needs the same self-exe answer. */
ssize_t __readlink_chk(const char *path, char *buf, size_t len, size_t max_len) {
    static ssize_t (*SP_REAL(__readlink_chk))(const char *, char *, size_t, size_t) = NULL;
    {
        int se = sp_self_exe_answer(path, buf, len);
        if (se > 0) return se;
    }
    SP_RESOLVE(__readlink_chk);
    return SP_REAL(__readlink_chk)(path, buf, len, max_len);
}

int chdir(const char *path) {
    static int (*SP_REAL(chdir))(const char *) = NULL;
    SP_RESOLVE(chdir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("chdir", path, p);
    return SP_REAL(chdir)(p);
}

/* stdio family: glibc's fopen/ifstream do files via *internal* non-cancellable
 * svc calls by the FILE* plumbing, so they never go through our open/open64
 * PLT wrappers (apt's cputable read was the first proof). Wrapping the FILE
 * entry points translates the path BEFORE glibc's internal open. */
FILE *fopen(const char *path, const char *mode) {
    static FILE *(*SP_REAL(fopen))(const char *, const char *) = NULL;
    SP_RESOLVE(fopen);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("fopen", path, p);
    return SP_REAL(fopen)(p, mode);
}

FILE *fopen64(const char *path, const char *mode) {
    static FILE *(*SP_REAL(fopen64))(const char *, const char *) = NULL;
    SP_RESOLVE(fopen64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("fopen64", path, p);
    return SP_REAL(fopen64)(p, mode);
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    static FILE *(*SP_REAL(freopen))(const char *, const char *, FILE *) = NULL;
    SP_RESOLVE(freopen);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("freopen", path, p);
    return SP_REAL(freopen)(p, mode, stream);
}

FILE *freopen64(const char *path, const char *mode, FILE *stream) {
    static FILE *(*SP_REAL(freopen64))(const char *, const char *, FILE *) = NULL;
    SP_RESOLVE(freopen64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("freopen64", path, p);
    return SP_REAL(freopen64)(p, mode, stream);
}

char *getcwd(char *buf, size_t size) {
    static char *(*SP_REAL(getcwd))(char *, size_t) = NULL;
    SP_RESOLVE(getcwd);
    char tmp[SP_PATH_MAX];
    char *r = SP_REAL(getcwd)(tmp, sizeof(tmp));
    if (!r) return NULL;
    char out[SP_PATH_MAX];
    size_t n = sp_reverse(&g_cfg, tmp, out, sizeof(out));
    n++; /* include NUL */
    if (buf) {
        if (size < n) { errno = ERANGE; return NULL; }
        memcpy(buf, out, n);
        return buf;
    }
    /* buf == NULL: glibc allocates; mimic: malloc of max(size, n+1) */
    size_t want = size > n ? size : n;
    char *m = malloc(want);
    if (!m) return NULL;
    memcpy(m, out, n);
    return m;
}

/* ---- AF_UNIX pathname translation (ADR-0010) ------------------------- */
/* X11/Wayland/VirGL/virpipe/ssh-agent all speak over pathname UNIX sockets.
 * proot translates sun_path; without this the guest cannot reach host or
 * bound-dir sockets at all. Rules:
 *  - pathname sockets ONLY (sun_path[1] != '\0' where the first byte is
 *    the conventional "abstract" marker? NO — abstract sockets have
 *    sun_path[0] == '\0'; pathnames sun_path[0] != '\0')
 *  - bind table first, then rootfs (identical order to sp_translate)
 *  - translated path must fit sockaddr_un/sun_path (108 incl. NUL); else
 *    passthrough faithfully (kernel EFAULT rather than lie)
 *  - NO symlink chase on sockets (bind CREATES the pathname; chase is
 *    file-semantics, not socket-semantics)
 *  Reverse direction (getsockname/getpeername/recvfrom) strips the
 *  rootfs/bind prefix to present guest spelling.
 */
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <errno.h>

/* returns replaced length, or 0 for passthrough */
static socklen_t sp_addr_fwd_unix(const struct sockaddr *addr, socklen_t len,
                                  struct sockaddr_un *out) {
    if (!addr || addr->sa_family != AF_UNIX) return 0;
    size_t off = offsetof(struct sockaddr_un, sun_path);
    if (len <= (socklen_t)(off + 1)) return 0;
    const struct sockaddr_un *u = (const struct sockaddr_un *)addr;
    if (u->sun_path[0] == '\0') return 0; /* abstract namespace: kernel-only */
    size_t plen = strnlen(u->sun_path, len - off);
    if (plen == 0) return 0;
    char x[SP_PATH_MAX];
    if (!sp_translate(&g_cfg, u->sun_path, x)) return 0;
    size_t xl = strlen(x);
    if (off + xl + 1 > sizeof(out->sun_path) + off) return 0; /* > 108 */
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    memcpy(out->sun_path, x, xl + 1);
    return (socklen_t)(off + xl + 1);
}

/* reverse-rootfs after kernel fills addr; in-place shrink when possible */
static void sp_addr_rev_unix(struct sockaddr *addr, socklen_t *len) {
    if (!addr || !len || addr->sa_family != AF_UNIX) return;
    size_t off = offsetof(struct sockaddr_un, sun_path);
    if (*len <= (socklen_t)(off + 1)) return;
    struct sockaddr_un *u = (struct sockaddr_un *)addr;
    if (u->sun_path[0] == '\0') return;
    char out[SP_PATH_MAX];
    size_t n = sp_reverse(&g_cfg, u->sun_path, out, sizeof(out));
    if (!n) return;              /* kernel path not under our rootfs/binds */
    if (off + n + 1 > (size_t)*len) return;
    memcpy(u->sun_path, out, n + 1);
    *len = (socklen_t)(off + n + 1);
}

/* ADR-0019: -k kernel-release parity. The real syscall still runs (the
 * kernel's sysname/nodename/machine comportment is unchanged); we only
 * substitute the uts.release field when the caller opted in with -k.
 * Static cache of the env value keeps the hot path per-call free. */
#include <sys/utsname.h>
int uname(struct utsname *b) {
    static int (*SP_REAL(uname))(struct utsname *) = NULL;
    SP_RESOLVE(uname);
    int rc = SP_REAL(uname)(b);
    if (rc != 0) return rc;
    static char rel[65];
    static int init_ = -1;
    if (init_ < 0) {
        init_ = 0;
        const char *e = getenv("SPROUT_KERNEL_RELEASE");
        if (e && *e) {
            strncpy(rel, e, sizeof(rel) - 1);
            rel[sizeof(rel) - 1] = '\0';
            init_ = 1;
        }
    }
    if (init_ == 1) {
        strncpy(b->release, rel, sizeof(b->release) - 1);
        b->release[sizeof(b->release) - 1] = '\0';
    }
    return rc;
}

/* ADR-0019: --ashmem-memfd parity. Android kernels before CONFIG_MEMFD_
 * CREATE=1 era need the ashmem fallback; GKI kernels accept the real
 * syscall anyway, so this wrapper is a strict superset: native first,
 * ENOSYS/ENODEV/EINVAL -> /dev/ashmem when the flag is set, carrying the
 * tracking fd in a tiny ring so fstat's st_size can be simulated
 * (ashmem's fstat reports 0; guest code checks it meaningfully without
 * our patch). FDs tracked are only THIS process's view (fd numbers are
 * process-scoped anyway). */
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define SV_ASHMEMIOC   0x77
#define SV_ASHMEM_SET_NAME   _IOW(SV_ASHMEMIOC, 1, char [256])
#define SV_ASHMEM_SET_SIZE   _IOW(SV_ASHMEMIOC, 3, size_t)

static int sv_ashmem_fds[16];
static int sv_ashmem_pos;
static void sv_ashmem_track(int fd) {
    sv_ashmem_fds[sv_ashmem_pos % 16] = fd;
    sv_ashmem_pos++;
}
static int sv_ashmem_is_tracked(int fd) {
    for (int i = 0; i < 16; i++) if (sv_ashmem_fds[i] == fd) return 1;
    return 0;
}

/* Android's app seccomp policy kills processes that call mount(2)&co with
 * SIGSYS (SIGSYS is the default action for those nrs, not EPERM). Guest
 * x86 emulators (FEX) try `mount()` while probing their RootFS VFS setup;
 * translate that family into a plain EPERM failure so callers fall back
 * to their no-mount code paths instead of getting reaped. */
int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    (void)source; (void)target; (void)fstype; (void)flags; (void)data;
    errno = EPERM;
    return -1;
}
int umount(const char *target) {
    (void)target;
    errno = EPERM;
    return -1;
}
int umount2(const char *target, int flags) {
    (void)target; (void)flags;
    errno = EPERM;
    return -1;
}
/* Same Android seccomp family as mount(): swap, acct, reboot/kexec and
 * friends are SIGSYS-killed by the host filter. Give each an EPERM answer
 * so any guest probing them degrades gracefully. */
int swapon(const char *path, int flags) {
    (void)path; (void)flags;
    errno = EPERM;
    return -1;
}
int swapoff(const char *path) {
    (void)path;
    errno = EPERM;
    return -1;
}
int acct(const char *filename) {
    (void)filename;
    errno = EPERM;
    return -1;
}

int pivot_root(const char *new_root, const char *put_old) {
    (void)new_root; (void)put_old;
    errno = EPERM;
    return -1;
}

int memfd_create(const char *name, unsigned int flags) {
    static int (*SP_REAL(memfd_create))(const char *, unsigned int) = NULL;
    SP_RESOLVE(memfd_create);
    int fd = -1;
    if (SP_REAL(memfd_create)) fd = SP_REAL(memfd_create)(name, flags);
    if (fd >= 0) return fd;
    int saved = errno;
    static int afd_flag = -1;
    if (afd_flag < 0)
        afd_flag = getenv("SPROUT_ASHMEM_MEMFD") ? 1 : 0;
    if (!afd_flag || (saved != ENOSYS && saved != ENODEV && saved != EINVAL)) {
        errno = saved;
        return -1;
    }
    static int (*SP_REAL(open))(const char *, int, ...) = NULL;
    SP_RESOLVE(open);
    int afd = SP_REAL(open)("/dev/ashmem", O_RDWR | O_CLOEXEC, 0600);
    if (afd < 0) { errno = saved; return -1; }
    char nm[256];
    snprintf(nm, sizeof(nm), "%s", name && *name ? name : "memfd");
    (void)ioctl(afd, SV_ASHMEM_SET_NAME, nm);
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    (void)ioctl(afd, SV_ASHMEM_SET_SIZE, pg);
    sv_ashmem_track(afd);
    return afd;
}

/* --ashmem-memfd fstat simulation: tracked ashmem fds report st_size=0;
 * the correct value is lseek-end. Used from the fstat-family wrappers.
 * NOTE the stored offset is RESTORED so the caller's fd position isn't
 * observable through the wrapper. */
static void sv_ashmem_fstat_fixup(int fd, off_t *size_out) {
    static int afd_flag = -1;
    if (afd_flag < 0)
        afd_flag = getenv("SPROUT_ASHMEM_MEMFD") ? 1 : 0;
    if (!afd_flag || !sv_ashmem_is_tracked(fd)) return;
    if (*size_out != 0) return;
    static off_t (*SP_REAL(lseek))(int, off_t, int) = NULL;
    SP_RESOLVE(lseek);
    off_t cur = SP_REAL(lseek)(fd, 0, SEEK_CUR);
    off_t end = SP_REAL(lseek)(fd, 0, SEEK_END);
    if (end > 0) *size_out = end;
    if (cur >= 0) (void)SP_REAL(lseek)(fd, cur, SEEK_SET);
}

#include <netinet/in.h>
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*SP_REAL(bind))(int, const struct sockaddr *, socklen_t) = NULL;
    SP_RESOLVE(bind);
    struct sockaddr_un x;
    socklen_t xl = sp_addr_fwd_unix(addr, len, &x);
    if (xl) return SP_REAL(bind)(fd, (const struct sockaddr *)&x, xl);
    /* ADR-0019 -p, proot port-mapping parity: privileged ports (<1024)
     * are remapped to 1024+port. Android denies CAP_NET_BIND_SERVICE to
     * app uids, so guest servers (lighttpd on :80, sshd on :22,
     * dnsmasq on :53) die on bind(2) — mapping them transparently keeps
     * the process model working. Connect/sendto stay untouched: the
     * mapping exists to start services, not to lie about client intent.
     * Only when SPROUT_PORTMAP=1 is set (explicit opt-in of v1). */
    static int pmap = -1;
    if (pmap < 0) pmap = getenv("SPROUT_PORTMAP") ? 1 : 0;
    if (pmap && addr) {
        if (addr->sa_family == AF_INET) {
            struct sockaddr_in in4;
            memcpy(&in4, addr, sizeof(in4));
            unsigned port = ntohs(in4.sin_port);
            if (port > 0 && port < 1024) {
                in4.sin_port = htons((uint16_t)(1024 + port));
                return SP_REAL(bind)(fd, (const struct sockaddr *)&in4,
                                     sizeof(in4));
            }
        } else if (addr->sa_family == AF_INET6) {
            struct sockaddr_in6 in6;
            memcpy(&in6, addr, sizeof(in6));
            unsigned port = ntohs(in6.sin6_port);
            if (port > 0 && port < 1024) {
                in6.sin6_port = htons((uint16_t)(1024 + port));
                return SP_REAL(bind)(fd, (const struct sockaddr *)&in6,
                                     sizeof(in6));
            }
        }
    }
    return SP_REAL(bind)(fd, addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*SP_REAL(connect))(int, const struct sockaddr *, socklen_t) = NULL;
    SP_RESOLVE(connect);
    struct sockaddr_un x;
    socklen_t xl = sp_addr_fwd_unix(addr, len, &x);
    if (xl) return SP_REAL(connect)(fd, (const struct sockaddr *)&x, xl);
    return SP_REAL(connect)(fd, addr, len);
}

ssize_t sendto(int fd, const void *bufv, size_t n, int flags,
               const struct sockaddr *addr, socklen_t len) {
    static ssize_t (*SP_REAL(sendto))(int, const void *, size_t, int,
                                      const struct sockaddr *, socklen_t) = NULL;
    SP_RESOLVE(sendto);
    struct sockaddr_un x;
    socklen_t xl = sp_addr_fwd_unix(addr, len, &x);
    if (xl) return SP_REAL(sendto)(fd, bufv, n, flags, (const struct sockaddr *)&x, xl);
    return SP_REAL(sendto)(fd, bufv, n, flags, addr, len);
}

/* SCM_CREDENTIALS truth: under fake-root the guest believes uid/gid=0 and
 * claims them in ucred ancillary data; the kernel rejects impersonation of
 * a uid/gid it never had with EPERM (GDBus clients: "Error sending
 * credentials: Error sending message: Operation not permitted" — hit by
 * dbus/glib inside XFCE, 2026-08-13). pid is untouched (no pid-ns lie),
 * uid/gid are swapped for the REAL ids, then the guest buffer is restored. */
static void sp_cmsg_creds_truth(struct msghdr *m,
                                struct ucred old[4], struct ucred *pos[4],
                                size_t *n) {
    *n = 0;
    if (!sp_fakeroot_on() || !m->msg_control ||
        m->msg_controllen < (socklen_t)CMSG_LEN(sizeof(struct ucred)))
        return;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m);
         c && *n < 4 && c->cmsg_len >= CMSG_LEN(0) &&
         (char *)c + c->cmsg_len <= (char *)m->msg_control + m->msg_controllen;
         c = CMSG_NXTHDR(m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS &&
            c->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
            struct ucred *uc = (struct ucred *)CMSG_DATA(c);
            old[*n] = *uc;
            pos[*n] = uc;
            uc->uid = (uid_t)syscall(SYS_getuid);
            uc->gid = (gid_t)syscall(SYS_getgid);
            if (uc->pid == 0 || uc->pid != (pid_t)syscall(SYS_getpid))
                uc->pid = (pid_t)syscall(SYS_getpid);
            (*n)++;
        }
    }
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    static ssize_t (*SP_REAL(sendmsg))(int, const struct msghdr *, int) = NULL;
    SP_RESOLVE(sendmsg);
    if (!msg) return SP_REAL(sendmsg)(fd, msg, flags);
    struct sockaddr_un x;
    struct msghdr m;
    const struct msghdr *to_send = msg;
    struct ucred crold[4];
    struct ucred *crpos[4];
    size_t ncr = 0;
    if (msg->msg_name) {
        socklen_t xl = sp_addr_fwd_unix((const struct sockaddr *)msg->msg_name,
                                        (socklen_t)msg->msg_namelen, &x);
        if (xl) {
            m = *msg;
            m.msg_name = &x;
            m.msg_namelen = xl;
            to_send = &m;
        }
    }
    /* cmsg editing happens on the GUEST'S buffer (either msg->msg_control
     * or the same pointer copied into `m`); restore it after the send. */
    if (to_send->msg_control)
        sp_cmsg_creds_truth((struct msghdr *)to_send, crold, crpos, &ncr);
    ssize_t r = SP_REAL(sendmsg)(fd, to_send, flags);
    while (ncr) { ncr--; *crpos[ncr] = crold[ncr]; }
    return r;
}

ssize_t recvfrom(int fd, void *bufv, size_t n, int flags,
                 struct sockaddr *addr, socklen_t *len) {
    static ssize_t (*SP_REAL(recvfrom))(int, void *, size_t, int,
                                        struct sockaddr *, socklen_t *) = NULL;
    SP_RESOLVE(recvfrom);
    ssize_t r = SP_REAL(recvfrom)(fd, bufv, n, flags, addr, len);
    if (r >= 0 && addr && len) sp_addr_rev_unix(addr, len);
    return r;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    static ssize_t (*SP_REAL(recvmsg))(int, struct msghdr *, int) = NULL;
    SP_RESOLVE(recvmsg);
    ssize_t r = SP_REAL(recvmsg)(fd, msg, flags);
    if (r >= 0 && msg && msg->msg_name) {
        socklen_t l = (socklen_t)msg->msg_namelen;
        sp_addr_rev_unix((struct sockaddr *)msg->msg_name, &l);
        msg->msg_namelen = l;
    }
    return r;
}

/* (ADR-0006+) Android's app-policy TRAPs legacy accept(2) — glibc emits
 * exactly that svc — but accept4(2) is allowed. Pivot the symbol call
 * site: every glibc accept(fd,addr,len) goes through accept4(fd,addr,len,0)
 * here so the guest never trips the trap. (Can't fix statics here — the
 * stub emulates the same pivot in-guest.) */
int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    static int (*SP_REAL(accept4))(int, struct sockaddr *, socklen_t *, int) = NULL;
    SP_RESOLVE(accept4);
    return SP_REAL(accept4)(fd, addr, len, 0);
}

/* proot-distro parity for NETLINK_AUDIT: untrusted_app audit sockets are
 * SELinux-denied with EACCES; shadow's useradd/adduser/usermod family
 * aborts ("Cannot open audit interface") unless the audit netlink reads
 * as UNSUPPORTED. proot-distro reports EPROTONOSUPPORT for the identical
 * call on the same kernel (verified live, proot=93 vs sprout=13 before
 * this wrapper). Minimal-lie errno remap, audit protocol only. */
int socket(int domain, int type, int protocol) {
    static int (*SP_REAL(socket))(int, int, int) = NULL;
    SP_RESOLVE(socket);
    int fd = SP_REAL(socket)(domain, type, protocol);
    if (fd < 0 && domain == AF_NETLINK && protocol == 9 /*NETLINK_AUDIT*/
        && errno == EACCES) {
        errno = EPROTONOSUPPORT;
    }
    return fd;
}

/* proot-compat: fake SO_PEERCRED results the way proot does — the guest
 * believes it is uid=0 (SPROUT_FAKEROOT) but the kernel still reports the
 * real Android uid in ucred for AF_UNIX peers; servers with peer-AACl checks
 * (tmux >= 3.5) deny clients whose ucred uid != their own. Zero uid/gid
 * when fake-root is active; keep the real pid. */
struct sp_ucred { pid_t pid; uid_t uid; gid_t gid; };
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    static int (*SP_REAL(getsockopt))(int, int, int, void *, socklen_t *) = NULL;
    SP_RESOLVE(getsockopt);
    int r = SP_REAL(getsockopt)(fd, level, optname, optval, optlen);
    if (r == 0 && level == SOL_SOCKET && optname == SO_PEERCRED && optval != NULL
        && optlen != NULL && *optlen >= (socklen_t)sizeof(struct sp_ucred)
        && sp_fakeroot_on()) {
        struct sp_ucred *u = (struct sp_ucred *)optval;
        u->uid = g_fake_uid;
        u->gid = g_fake_gid;
    }
    return r;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    static int (*SP_REAL(getsockname))(int, struct sockaddr *, socklen_t *) = NULL;
    SP_RESOLVE(getsockname);
    int r = SP_REAL(getsockname)(fd, addr, len);
    if (!r && addr && len) sp_addr_rev_unix(addr, len);
    return r;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    static int (*SP_REAL(getpeername))(int, struct sockaddr *, socklen_t *) = NULL;
    SP_RESOLVE(getpeername);
    int r = SP_REAL(getpeername)(fd, addr, len);
    if (!r && addr && len) sp_addr_rev_unix(addr, len);
    return r;
}


char *realpath(const char *path, char *resolved) {
    static char *(*SP_REAL(realpath))(const char *, char *) = NULL;
    SP_RESOLVE(realpath);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    char tmp[SP_PATH_MAX];
    char *r = SP_REAL(realpath)(p, tmp);
    if (!r) return NULL;
    char out[SP_PATH_MAX];
    sp_reverse(&g_cfg, tmp, out, sizeof(out));
    if (!resolved) {
        resolved = sp_dns_strdup(out);
    } else {
        strcpy(resolved, out);
    }
    return resolved;
}

/* stat-family */
int stat(const char *path, struct stat *st) {
    static int (*SP_REAL(stat))(const char *, struct stat *) = NULL;
    SP_RESOLVE(stat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("stat", path, p);
    int rc = SP_REAL(stat)(p, st);
    if (rc == 0) { sp_spoof_uid_gid(&st->st_uid, &st->st_gid); if (sp_hreg_hit(path) && st->st_nlink == 1) st->st_nlink = 2; }
    return rc;
}

#if defined(__GLIBC__)
/* The *64 transitional family (fstat64/fstatat64/stat64/lstat64) is a
 * glibc ABI: musl does not provide struct stat64 nor these symbols, so
 * guard the whole block. */

/* fstat64: no path to translate, only the ownership spoof contract. */
int fstat64(int fd, struct stat64 *st) {
    static int (*SP_REAL(fstat64))(int, struct stat64 *) = NULL;
    SP_RESOLVE(fstat64);
    int rc = SP_REAL(fstat64)(fd, st);
    if (rc == 0) {
        sp_spoof_uid_gid(&((struct stat *)st)->st_uid, &((struct stat *)st)->st_gid);
        sv_ashmem_fstat_fixup(fd, (off_t *)&st->st_size);
    }
    return rc;
}

/* fstat: glibc exports this symbol even on 64-bit (no path to translate,
 * nobody wrapped it until --ashmem-memfd: the presence of this symbol is
 * what lets the simulated st_size surface for ashmem fds). */
int fstat(int fd, struct stat *st) {
    static int (*SP_REAL(fstat))(int, struct stat *) = NULL;
    SP_RESOLVE(fstat);
    int rc = SP_REAL(fstat)(fd, st);
    if (rc == 0) {
        sp_spoof_uid_gid(&st->st_uid, &st->st_gid);
        sv_ashmem_fstat_fixup(fd, &st->st_size);
    }
    return rc;
}
int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags) {
    static int (*SP_REAL(fstatat64))(int, const char *, struct stat64 *, int) = NULL;
    SP_RESOLVE(fstatat64);
    char x[SP_PATH_MAX];
    const char *p = (flags & AT_SYMLINK_NOFOLLOW) ? sp_translate_l(path, x) : sp_translate_x(path, x);
    SP_TRACE("fstatat64", path, p);
    int rc = SP_REAL(fstatat64)(dirfd, p, st, flags);
    if (rc == 0) { sp_spoof_uid_gid(&((struct stat *)st)->st_uid, &((struct stat *)st)->st_gid); if (sp_hreg_hit(path) && ((struct stat *)st)->st_nlink == 1) ((struct stat *)st)->st_nlink = 2; }
    return rc;
}

int stat64(const char *path, struct stat64 *st) {
    static int (*SP_REAL(stat64))(const char *, struct stat64 *) = NULL;
    SP_RESOLVE(stat64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("stat64", path, p);
    int rc = SP_REAL(stat64)(p, st);
    if (rc == 0) { sp_spoof_uid_gid(&((struct stat *)st)->st_uid, &((struct stat *)st)->st_gid); if (sp_hreg_hit(path) && ((struct stat *)st)->st_nlink == 1) ((struct stat *)st)->st_nlink = 2; }
    return rc;
}

int lstat(const char *path, struct stat *st) {
    static int (*SP_REAL(lstat))(const char *, struct stat *) = NULL;
    SP_RESOLVE(lstat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("lstat", path, p);
    int rc = SP_REAL(lstat)(p, st);
    if (rc == 0) { sp_spoof_uid_gid(&st->st_uid, &st->st_gid); if (sp_hreg_hit(path) && st->st_nlink == 1) st->st_nlink = 2; }
    return rc;
}

int lstat64(const char *path, struct stat64 *st) {
    static int (*SP_REAL(lstat64))(const char *, struct stat64 *) = NULL;
    SP_RESOLVE(lstat64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("lstat64", path, p);
    int rc = SP_REAL(lstat64)(p, st);
    if (rc == 0) { sp_spoof_uid_gid(&((struct stat *)st)->st_uid, &((struct stat *)st)->st_gid); if (sp_hreg_hit(path) && ((struct stat *)st)->st_nlink == 1) ((struct stat *)st)->st_nlink = 2; }
    return rc;
}
#endif /* __GLIBC__ */


ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*SP_REAL(readlink))(const char *, char *, size_t) = NULL;
    SP_RESOLVE(readlink);
    if (bufsiz == 0) return SP_REAL(readlink)(path, buf, bufsiz);
    {
        /* /proc/self/exe => SPROUT_EXE (see sp_self_exe_answer) */
        int se = sp_self_exe_answer(path, buf, bufsiz);
        if (se > 0) return se;
    }
    char x[SP_PATH_MAX];
    char target[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    /* Reserve one byte: readlink allows n == bufsiz, which would leave no
     * room for the NUL the reverse-translation step needs. */
    ssize_t n = SP_REAL(readlink)(p, target, SP_PATH_MAX - 1);
    if (n <= 0) return n;
    target[n] = '\0';
    n = (ssize_t)sp_reverse(&g_cfg, target, target, sizeof(target));
    if ((size_t)n > bufsiz) n = (ssize_t)bufsiz;
    memcpy(buf, target, (size_t)n);
    SP_TRACE("readlink", path, target);
    return n;
}

/* directory iteration */
DIR *opendir(const char *name) {
    static DIR *(*SP_REAL(opendir))(const char *) = NULL;
    SP_RESOLVE(opendir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(name, x);
    SP_TRACE("opendir", name, p);
    return SP_REAL(opendir)(p);
}

/* removal/creation/mutation */
int unlink(const char *path) {
    static int (*SP_REAL(unlink))(const char *) = NULL;
    SP_RESOLVE(unlink);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("unlink", path, p);
    return SP_REAL(unlink)(p);
}

int rmdir(const char *path) {
    static int (*SP_REAL(rmdir))(const char *) = NULL;
    SP_RESOLVE(rmdir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("rmdir", path, p);
    return SP_REAL(rmdir)(p);
}

int mkdir(const char *path, mode_t mode) {
    static int (*SP_REAL(mkdir))(const char *, mode_t) = NULL;
    SP_RESOLVE(mkdir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_l(path, x);
    SP_TRACE("mkdir", path, p);
    return SP_REAL(mkdir)(p, mode);
}

int rename(const char *oldpath, const char *newpath) {
    static int (*SP_REAL(rename))(const char *, const char *) = NULL;
    SP_RESOLVE(rename);
    char xo[SP_PATH_MAX], xn[SP_PATH_MAX];
    /* both sides are name-operations: move the LINK itself, never its target */
    const char *po = sp_translate_l(oldpath, xo);
    const char *pn = sp_translate_l(newpath, xn);
    SP_TRACE("rename", oldpath, po);
    return SP_REAL(rename)(po, pn);
}
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    static int (*SP_REAL(renameat))(int, const char *, int, const char *) = NULL;
    SP_RESOLVE(renameat);
    if ((olddirfd != -100 /*AT_FDCWD*/ && oldpath && oldpath[0] != '/') ||
        (newdirfd != -100 /*AT_FDCWD*/ && newpath && newpath[0] != '/'))
        return SP_REAL(renameat)(olddirfd, oldpath, newdirfd, newpath);
    char xo[SP_PATH_MAX], xn[SP_PATH_MAX];
    const char *po = sp_translate_l(oldpath, xo);
    const char *pn = sp_translate_l(newpath, xn);
    SP_TRACE("renameat", oldpath, po);
    return SP_REAL(renameat)(olddirfd, po, newdirfd, pn);
}
int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags) {
    static int (*SP_REAL(renameat2))(int, const char *, int, const char *, unsigned int) = NULL;
    SP_RESOLVE(renameat2);
    if ((olddirfd != -100 /*AT_FDCWD*/ && oldpath && oldpath[0] != '/') ||
        (newdirfd != -100 /*AT_FDCWD*/ && newpath && newpath[0] != '/'))
        return SP_REAL(renameat2)(olddirfd, oldpath, newdirfd, newpath, flags);
    char xo[SP_PATH_MAX], xn[SP_PATH_MAX];
    const char *po = sp_translate_l(oldpath, xo);
    const char *pn = sp_translate_l(newpath, xn);
    SP_TRACE("renameat2", oldpath, po);
    return SP_REAL(renameat2)(olddirfd, po, newdirfd, pn, flags);
}

int symlink(const char *target, const char *linkpath) {
    static int (*SP_REAL(symlink))(const char *, const char *) = NULL;
    SP_RESOLVE(symlink);
    char x[SP_PATH_MAX];
    const char *lp = sp_translate_l(linkpath, x);
    /* target is guest-spelled by design; the reverse path is what readlink
     * hands back, so both directions stay consistent. */
    SP_TRACE("symlink", linkpath, lp);
    return SP_REAL(symlink)(target, lp);
}
int symlinkat(const char *target, int newdirfd, const char *linkpath) {
    static int (*SP_REAL(symlinkat))(const char *, int, const char *) = NULL;
    SP_RESOLVE(symlinkat);
    if (newdirfd != -100 /*AT_FDCWD*/ && linkpath && linkpath[0] != '/')
        return SP_REAL(symlinkat)(target, newdirfd, linkpath);
    char x[SP_PATH_MAX];
    const char *lp = sp_translate_l(linkpath, x);
    SP_TRACE("symlinkat", linkpath, lp);
    return SP_REAL(symlinkat)(target, newdirfd, lp);
}

/* ------------------------------------------------------------------ */

/* execve chaining: rewriting exec targets through the guest loader    */
/* ------------------------------------------------------------------ */

/* Busybox-style appliance layouts make every tool an absolute symlink to
 * /bin/busybox (or similar). On the host those absolute targets don't
 * resolve; chase absolute symlinks back through the guest translation
 * (relative ones are fine, the kernel resolves them on host). Mutates
 * `host` in place, up to 8 hops. */
static void sp_resolve_absolute_symlink(char host[SP_PATH_MAX]) {
    char target[SP_PATH_MAX], dir[SP_PATH_MAX], tmp[SP_PATH_MAX];
    for (int hop = 0; hop < 8; hop++) {
        struct stat st;
        if (lstat(host, &st) != 0 || !S_ISLNK(st.st_mode)) return;
        ssize_t n = readlink(host, target, sizeof(target) - 1);
        if (n < 0) return;
        target[n] = '\0';
        if (target[0] == '/') {
            if (!sp_translate(&g_cfg, target, tmp)) return;
            snprintf(host, SP_PATH_MAX, "%s", tmp);
        } else {
            snprintf(dir, sizeof(dir), "%s", host);
            char *sl = strrchr(dir, '/');
            if (!sl) return;
            *sl = '\0';
            int w = snprintf(host, SP_PATH_MAX, "%s/%s", dir, target);
            if (w < 0 || w >= SP_PATH_MAX) return; /* truncated join = wrong path; leave host as-is */
        }
    }
}

/* Return codes from sp_classify_host */
#define SP_ELF_DYNAMIC 1
#define SP_ELF_STATIC  2
#define SP_SCRIPT      3
#define SP_NOT_ELF     0

/* Minimal ELF64 inspection: classify host path as dynamic ELF (writes the
 * PT_INTERP string into interp) / static ELF / script. Only reads header + a
 * few hundred bytes. */
/* opt (shebang trailing arg, e.g. "sh" in "#!/bin/busybox sh") may be NULL
 * for callers that don't chain scripts. */
static int sp_classify_host(const char *host, char interp[SP_PATH_MAX], char opt[SP_PATH_MAX]) {
    if (opt) opt[0] = '\0';
    FILE *f = fopen(host, "rb");
    if (!f) return SP_NOT_ELF;
    unsigned char head[256];
    size_t n = fread(head, 1, sizeof(head), f);
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        /* script: copy the interpreter word into interp, trailing arg into opt */
        size_t i = 2;
        while (i < n && (head[i] == ' ' || head[i] == '\t')) i++;
        size_t j = 0;
        while (i < n && j < SP_PATH_MAX - 1 && head[i] != ' ' && head[i] != '\t'
               && head[i] != '\n' && head[i] != '\r') {
            interp[j++] = (char)head[i++];
        }
        interp[j] = '\0';
        if (opt) {
            /* Linux-style: everything after the interp word up to newline
             * becomes ONE argv token for the interpreter. */
            while (i < n && (head[i] == ' ' || head[i] == '\t')) i++;
            size_t k = 0;
            while (i < n && k < SP_PATH_MAX - 1 && head[i] != '\n' && head[i] != '\r') {
                opt[k++] = (char)head[i++];
            }
            while (k > 0 && (opt[k - 1] == ' ' || opt[k - 1] == '\t')) k--;
            opt[k] = '\0';
        }
        fclose(f);
        return SP_SCRIPT;
    }
    if (n < 64 || head[0] != 0x7f || memcmp(head + 1, "ELF", 3) != 0 || head[4] != 2) {
        fclose(f);
        return SP_NOT_ELF;
    }
    unsigned int e_phoff = (unsigned int)(*(unsigned long long *)(head + 32));
    unsigned short e_phentsize = *(unsigned short *)(head + 54);
    unsigned short e_phnum = *(unsigned short *)(head + 56);
    if (e_phentsize < 56 || e_phnum == 0 || e_phnum > 64) {
        fclose(f);
        return SP_NOT_ELF;
    }
    interp[0] = '\0';
    for (unsigned int i = 0; i < e_phnum; i++) {
        if (fseek(f, (long)(e_phoff + i * e_phentsize), SEEK_SET) != 0) break;
        unsigned char ph[56];
        if (fread(ph, 1, 56, f) != 56) break;
        unsigned int p_type = *(unsigned int *)ph;
        if (p_type != 3) continue; /* PT_INTERP */
        unsigned long long p_offset = *(unsigned long long *)(ph + 8);
        unsigned long long p_filesz = *(unsigned long long *)(ph + 32);
        if (p_filesz == 0 || p_filesz >= SP_PATH_MAX) break;
        if (fseek(f, (long)p_offset, SEEK_SET) != 0) break;
        if (fread(interp, 1, (size_t)p_filesz, f) != (size_t)p_filesz) break;
        interp[p_filesz - 1] = '\0';
        fclose(f);
        return interp[0] ? SP_ELF_DYNAMIC : SP_NOT_ELF;
    }
    fclose(f);
    return SP_ELF_STATIC;
}

/* Resolve `name` against guest PATH (absolute or relative), writing the
 * resulting GUEST-absolute candidate into out. Returns 0 on success,
 * -1 when nothing matched. */
static int sp_guest_path_search(const char *name, char out[SP_PATH_MAX]) {
    if (strchr(name, '/') != NULL) {
        /* already path-qualified: must still exist (shebang search, exec
         * PATH bypass). Mirroring the PATH-search branch's access(X_OK). */
        if (strlen(name) >= SP_PATH_MAX) return -1;
        char hostabs[SP_PATH_MAX];
        const char *ha = sp_translate_x(name, hostabs);
        if (access(ha, X_OK) != 0) return -1;
        strcpy(out, name);
        return 0;
    }
    const char *path = getenv("PATH");
    const char *def = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path ? path : def);
    for (char *dir = strtok(buf, ":"); dir; dir = strtok(NULL, ":")) {
        char cand[SP_PATH_MAX];
        int w = snprintf(cand, sizeof(cand), "%s/%s", *dir ? dir : ".", name);
        if (w < 0 || (size_t)w >= sizeof(cand)) continue;
        char hostcand[SP_PATH_MAX];
        const char *hc = sp_translate_x(cand, hostcand);
        if (access(hc, X_OK) == 0) {
            strcpy(out, cand);
            return 0;
        }
    }
    return -1;
}

/* firefox-class LD_LIBRARY_PATH users (lib*naive in LD_PRELOAD, $ORIGIN
 * DT_NEEDED chains): glibc's ld.so gives the `--library-path` CMDLINE arg
 * full precedence over the env's LD_LIBRARY_PATH, and our loader chain
 * always passes the system list on the cmdline — so a guest that
 * legitimately sets LD_LIBRARY_PATH for its children (firefox-esr: the
 * stub pushes libmozsandbox.so into its e10s childrens' LD_PRELOAD and
 * expects $exedir to be searched, user log 2026-08-13) lost those dirs.
 * Merge guest-supplied entries BEFORE our system fallback. Static buffer
 * is deliberate: chains run under vfork-shared frames (ADR-0014), malloc
 * is forbidden here. */
static const char *sp_chain_libpath(char *envp[]) {
    const char *sys = getenv("SPROUT_LIBRARY_PATH");
    const char *cust = NULL;
    for (int i = 0; envp && envp[i]; i++)
        if (!strncmp(envp[i], "LD_LIBRARY_PATH=", 16)) { cust = envp[i] + 16; break; }
    if (!cust || !*cust || !sys || !*sys) return NULL;
    if (!strcmp(cust, sys)) return NULL;      /* already the system list */
    /* deep chains re-merge (merged-in-cust + sys tail duplicated): fine,
     * duplicates only cost a repeat directory walk in ld.so; dropping the
     * merge would LOSE the dirs after the 2nd exec generation. */
    static char merged[SP_PATH_MAX * 4];
    int w = snprintf(merged, sizeof(merged), "%s:%s", cust, sys);
    if (w < 0 || (size_t)w >= sizeof(merged)) return NULL;
    /* env-visible row: grandchildren that re-exec through the chain see
     * the same merged list; the next generation re-merges harmlessly. */
    static char lp_entry[SP_PATH_MAX * 4 + 32];
    w = snprintf(lp_entry, sizeof(lp_entry), "LD_LIBRARY_PATH=%s", merged);
    if (w > 0 && (size_t)w < sizeof(lp_entry)) {
        for (int i = 0; envp[i]; i++)
            if (!strncmp(envp[i], "LD_LIBRARY_PATH=", 16)) { envp[i] = lp_entry; break; }
    }
    return merged;
}

/* Build the loader-chain argv for a dynamic guest program:
 *   loader --argv0 <orig> --inhibit-cache --library-path <lp> <hostprog> [args...]
 * into into allocated memory; caller frees with sp_free_argv. */
/* vfork-safety (ADR-0014): posix_spawn()'d executors (debian dash and
 * friends) reach us as clone(CLONE_VM|CLONE_VFORK) — the chain-building
 * code runs in a shared address space, so malloc() from here would
 * scribble the RUNNING PARENT's glibc arena (observed on-device as the
 * parent's next spawn tripping glibc's sysmalloc assertion). All chain
 * structures are therefore built in CALLER STACK memory, never the heap. */
/* 256 was the original cap; autotools/rpm/docker-style guests regularly
 * exec commands after glob expansion with hundreds of args (the argv-build
 * EIO bug report: configure's grep got EIO 'errno=5' once argv arg-count
 * exceeded vmax). 4096 pointers = 32 KiB of caller stack — safe pre-exec
 * (glibc thread stack >= 8 MiB) and preserves ADR-0014 vfork-safety
 * (still caller-stack memory, never heap). Configurability via env is
 * left out deliberately: bigger static stack is strictly better here. */
#define SP_CHAIN_MAX_ARGS 4096
static int sp_build_loader_argv(char **v, size_t vmax,
                                const char *host_prog, char *const argv[],
                                int extra, int *outc, const char *lp_override) {
    int argc = 0;
    while (argv[argc]) argc++;
    const char *loader = getenv("SPROUT_LOADER");
    const char *lp = lp_override ? lp_override : getenv("SPROUT_LIBRARY_PATH");
    const char *libc_kind = getenv("SPROUT_LIBC"); /* "musl" or "glibc" (default) */
    if (!loader) {
        fprintf(stderr, "[sprout] argv-build fail: SPROUT_LOADER unset (argc=%d)\n", argc);
        return -2; /* -2: missing loader — different from cap overflow */
    }
    int musl = libc_kind && strcmp(libc_kind, "musl") == 0;
    int i = 0;
    v[i++] = (char *)loader;
    v[i++] = "--argv0";
    v[i++] = argv[0] ? argv[0] : (char *)host_prog;
    if (!musl) v[i++] = "--inhibit-cache";
    v[i++] = "--library-path";
    v[i++] = (char *)(lp ? lp : "");
    v[i++] = (char *)host_prog;
    if (vmax < (size_t)(i + argc)) {
        fprintf(stderr, "[sprout] argv-build fail: argc=%d vneed=%d vmax=%zu\n",
                argc, i + argc, vmax);
        return -1; /* cap overflow */
    }
    for (int k = 1; k < argc; k++) v[i++] = argv[k];
    v[i] = NULL;
    if (outc) *outc = i;
    return 0;
}

/* Execute path/argv/envp under the guest loader when dynamic; script
 * shebangs resolve their interpreter first. depth guards recursion. */
/* Resolve the REAL execve once for chain tails (we cannot call our own
 * exported symbol — self-recursion). Direct declaration would collide with
 * ours, so keep it under a private name and use a union (POSIX-sanctioned
 * dlsym conversion). */
static int sp_real_execve(const char *path, char *const argv[], char *const envp[]) {
    static int (*f)(const char *, char *const *, char *const *) = NULL;
    if (!f) {
        union { void *p; __typeof__(f) q; } u;
        u.p = dlsym(RTLD_NEXT, "execve");
        f = u.q;
    }
    return f(path, argv, envp);
}

static int sp_chain_fail(const char *path, int depth, int err, const char *why) {
    char eb[192];
    int el = snprintf(eb, sizeof eb,
                      "[sprout] chain-fail depth=%d path='%s' why=%s errno=%d (%s) loader=%s\n",
                      depth, path ? path : "?", why, err, strerror(err),
                      getenv("SPROUT_LOADER") ? getenv("SPROUT_LOADER") : "(null)");
    if (el > (int)sizeof eb) el = (int)sizeof eb;
    (void)!write(2, eb, (size_t)el);
    /* ALSO the flight recorder: spawn children get their stderr dup2'ed
     * into the parent's pipe (apk does exactly this for script-trigger
     * runs), which swallowed every chain-fail line and hid the apk-127
     * root cause for days. */
    const char *logf = getenv("SPROUT_TRACELOG");
    if (logf && *logf) {
        int fd = (int)syscall(SP_SYS_openat, AT_FDCWD, logf,
                              O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (fd >= 0) {
            (void)!write(fd, eb, (size_t)el);
            extern char **environ;
            char kb[2048]; int kn = 0;
            kn = snprintf(kb, sizeof(kb), "TRENV pid=%d", (int)getpid());
            for (int i = 0; environ && environ[i] && i < 40 && kn < (int)sizeof(kb) - 80; i++) {
                if (strncmp(environ[i], "SPROUT", 6) && strncmp(environ[i], "LD_", 3)) continue;
                kn += snprintf(kb + kn, sizeof(kb) - (size_t)kn, " |%.60s", environ[i]);
            }
            kb[kn++] = '\n';
            (void)!write(fd, kb, (size_t)kn);
            close(fd);
        }
    }
    errno = err;
    return -1;
}

/* apk (without --preserve-env) spawns script-triggers with env =
 * exactly one PATH entry — LD_PRELOAD and every SPROUT_* key get
 * stripped. The spawn-child then boots WITHOUT the interposer, and the
 * script's inner execve('/bin/busybox') becomes raw-kernel: ENOENT,
 * sh reports 127. proot is immune (ptrace translates at the syscall
 * boundary, env-independent). Mirror that immunity at the chain level:
 * when the caller's envp is missing a key the PARENT process carries,
 * re-inject it. Only LD_/SPROUT_ prefixed keys are copied (the caller's deliberate
 * env like APK_SCRIPT='install' stays authoritative). */
extern char **environ;
static char **sp_chain_env(char *const envp[]) {
    static char *e2[512];
    const char *vital[] = {"LD_", "SPROUT_"};
    int n = 0;
    if (envp) {
        for (; envp[n] && n < 400; n++) e2[n] = envp[n];
    }
    for (int vi = 0; vi < 2; vi++) {
        const char *pfx = vital[vi];
        size_t pl = strlen(pfx);
        for (int i = 0; environ[i] && n < 500; i++) {
            if (strncmp(environ[i], pfx, pl) != 0) continue;
            const char *eq = strchr(environ[i], '=');
            if (!eq) continue;
            size_t kl = (size_t)(eq - environ[i]);
            int have = 0;
            for (int j = 0; j < n; j++) {
                if (strncmp(e2[j], environ[i], kl) == 0 && e2[j][kl] == '=') { have = 1; break; }
            }
            if (have) {
                /* LD_PRELOAD MERGE CLAUSE: it is the ONE vital key whose
                 * guest-supplied value must COMBINE with ours instead of
                 * shadowing it. A guest LD_PRELOAD=libX.so (firefox-esr's
                 * libmozsandbox for its e10s children, user log 2026-08-13)
                 * replacing the interposer silently DISABLES exec-chaining
                 * for that process's descendants: the next exec goes to the
                 * RAW glibc execve and /proc/self/exe & friends leak loader
                 * paths again. Keep our entry FIRST (interception lives
                 * there), the guest's DSO appended after; skip re-merge
                 * when the interposer is already visible (deep chains). */
                if (kl == 10 && 0 == strncmp(environ[i], "LD_PRELOAD", kl)) {
                    for (int j = 0; j < n; j++) {
                        if (strncmp(e2[j], environ[i], kl) != 0 || e2[j][kl] != '=') continue;
                        if (strstr(e2[j], "libsprout-core")) continue;
                        static char mp[SP_PATH_MAX * 4 + 32];
                        int mw = snprintf(mp, sizeof(mp), "LD_PRELOAD=%s:%s", eq + 1, e2[j] + kl + 1);
                        if (mw > 0 && (size_t)mw < sizeof(mp)) e2[j] = mp;
                    }
                }
            } else e2[n++] = environ[i];
        }
    }
    e2[n] = NULL;
    {
        const char *logf = getenv("SPROUT_TRACELOG");
        if (logf && *logf) {
            int fd = (int)syscall(SP_SYS_openat, AT_FDCWD, logf, O_WRONLY | O_APPEND | O_CREAT, 0600);
            if (fd >= 0) {
                char kb[1500]; int kn = 0;
                int spr=0, ld=0, en=0; for (int i=0; environ && environ[i]; i++) { en++; if (!strncmp(environ[i],"SPROUT",6)) spr++; if (!strncmp(environ[i],"LD_",3)) ld++; }
                kn = snprintf(kb, sizeof(kb), "TRINJ pid=%d n=%d env=%d spr=%d ld=%d", (int)getpid(), n, en, spr, ld);
                for (int j = 0; j < n && kn < (int)sizeof(kb) - 100; j++)
                    if (!strncmp(e2[j], "SPROUT", 6) || !strncmp(e2[j], "LD_", 3))
                        kn += snprintf(kb + kn, sizeof(kb) - (size_t)kn, " |%.40s", e2[j]);
                kb[kn++] = '\n';
                (void)!write(fd, kb, (size_t)kn);
                close(fd);
            }
        }
    }
    return e2;
}

/* /proc/self/exe truth (Firefox/Gecko-class bug): under the loader chain,
 * the kernel maps /proc/self/exe to OUR sanitized ld.so, so anything that
 * computes its own install dir via readlink(/proc/self/exe) — Mozilla's
 * stub linker looking for $exedir/libxul.so being the canonical case
 * ("Couldn't load XPCOM" under sprout, user report 2026-08-13) — searches
 * ~/.cache/sprout and finds nothing. plan.rs and this chain stamp
 * SPROUT_EXE=<guest-abs spelling of the current image> into the env and
 * the readlinkat() interposer below replies with it when asked for the
 * self-exe symlink. proot never needed this because it exec()s the guest
 * binary directly; our chain exec()s a launcher. */
static void sp_stamp_exe(char **env, const char *gabs) {
    if (!gabs || !*gabs) return;
    static char exe_entry[SP_PATH_MAX + 12];
    snprintf(exe_entry, sizeof(exe_entry), "SPROUT_EXE=%s", gabs);
    int n = 0;
    for (; env[n]; n++)
        if (!strncmp(env[n], "SPROUT_EXE=", 11)) { env[n] = exe_entry; return; }
    if (n < 510) { env[n] = exe_entry; env[n + 1] = NULL; }
}

/* ------- ADR-0018: userspace binfmt adapter -------
 * Kernel binfmt_misc is unusable rootless (CAP_SYS_ADMIN + /proc/sys
 * writes are SELinux-blocked), so foreign-arch ELFs (x86_64, i386) are
 * sniffed at THE exec-gate here in the preload lane and rewritten to an
 * emulator (default /usr/local/bin/box64) via argv manipulation, the same
 * interception spine the shebang-chain already uses.
 * Config: SPROUT_BINFMT_X86_64 / SPROUT_BINFMT_I386 (emulator guest path),
 * SPROUT_BINFMT_ALWAYS=1 for proot -q parity (wrap every native exec).
 * Costs: ONE extra 20-byte read per exec on top of the (already required)
 * classify open; zero per-syscall. The supervisor lane CANNOT consume a
 * foreign-arch image: tracee execve classification is arch-tied, so the
 * preload gate is the only interception point — documented in ADR-0018. */
#define EM_386     3
#define EM_X86_64  62
#define EM_AARCH64 183

static int sp_execve_chain(const char *path, char *const argv[], char *const envp[], int depth);

static int sp_binfmt_always(void) {
    static int v = -1;
    if (v < 0)
        v = (getenv("SPROUT_BINFMT_ALWAYS") && getenv("SPROUT_BINFMT_ALWAYS")[0] == '1') ? 1 : 0;
    return v;
}

/* Sniff ELF identity from the already-translated host path. Fills e_class
 * + e_machine when it IS an ELF (any arch/class). Returns 1 when ELF, 0
 * otherwise (scripts keep the regular chain, which re-enters the gate for
 * their interpreter). */
static int sp_elf_meta(const char *host, unsigned char *e_class_out, unsigned short *e_machine_out) {
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    unsigned char head[20];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (n < sizeof(head) || head[0] != 0x7f || head[1] != 'E' || head[2] != 'L' || head[3] != 'F')
        return 0;
    *e_class_out  = head[4];  /* 1=32 2=64 */
    *e_machine_out = (unsigned short)(head[18] | ((unsigned short)head[19] << 8));
    return 1;
}

/* Rewrites the incoming exec to an emulator when the target is a foreign
 * (or ALWAYS-wrapped native) ELF. Returns: 1 = recursion issued (caller
 * propagates rc), 0 = not relevant — proceed with the regular chain;
 * -1 = foreign ELF but no usable emulator (caller emits chain_fail with
 * ENOEXEC). */
static int sp_binfmt_maybe_exec(const char *guest_path, const char *host_abs,
                                char *const argv[], char *const merged_envp[], int depth) {
    unsigned char e_class = 0;
    unsigned short e_machine = 0;
    int is_elf = sp_elf_meta(host_abs, &e_class, &e_machine);
    if (!is_elf) return 0; /* scripts etc. — ordinary chain */

    const char *emu = NULL, *libenv = NULL, *libdef = NULL;
    if (e_class == 2 && e_machine == EM_X86_64) {
        emu = getenv("SPROUT_BINFMT_X86_64");
        if (!emu || !*emu) emu = "/usr/local/bin/box64";
        libenv = "BOX64_LD_LIBRARY_PATH";
        libdef = "/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/x86_64-linux-gnu";
    } else if (e_class == 1 && e_machine == EM_386) {
        emu = getenv("SPROUT_BINFMT_I386");
        if ((!emu || !*emu) && getenv("SPROUT_BINFMT_X86_64") && *getenv("SPROUT_BINFMT_X86_64"))
            emu = getenv("SPROUT_BINFMT_X86_64"); /* box64's integrated box32 */
        if (!emu || !*emu) emu = "/usr/local/bin/box64";
        libenv = "BOX32_LD_LIBRARY_PATH";
        libdef = "/usr/lib/i386-linux-gnu:/lib/i386-linux-gnu:/usr/lib32";
    } else if (e_machine == EM_AARCH64) {
        if (!sp_binfmt_always()) return 0;
        emu = getenv("SPROUT_BINFMT_X86_64");
        if (!emu || !*emu) emu = "/usr/local/bin/box64";
        libenv = "BOX64_LD_LIBRARY_PATH";
        libdef = "/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/x86_64-linux-gnu";
    } else {
        return 0; /* other architectures: native-path failure (unchanged) */
    }

    /* never wrap the emulator ITSELF (self-recursion guard: with ALWAYS=1
     * box64's own exec resurfaces here) */
    {
        char ex[SP_PATH_MAX];
        const char *eraw = sp_translate_x(emu, ex);
        char ehost[SP_PATH_MAX];
        snprintf(ehost, sizeof(ehost), "%s", eraw);
        sp_resolve_absolute_symlink(ehost);
        if (strcmp(ehost, host_abs) == 0) return 0;
        if (access(ehost, X_OK) != 0) {
            fprintf(stderr,
                "sprout: binfmt: no emulator for EI_CLASS=%u e_machine=%u target %s "
                "(set SPROUT_BINFMT_X86_64/SPROUT_BINFMT_I386 or install box64)\n",
                (unsigned)e_class, (unsigned)e_machine, guest_path);
            return -1;
        }
    }

    /* argv: [emu, guest_path, argv[1], argv[2], ... NULL] — guest_path stays
     * guest-spelled: box64's own open() rides the preload translation. */
    int ac = 0;
    while (argv[ac]) ac++;
    char **nv = malloc(((size_t)ac + 2) * sizeof(char *));
    if (!nv) { errno = ENOMEM; return -1; }
    nv[0] = (char *)emu;
    nv[1] = (char *)guest_path;
    for (int i = 1; argv[i]; i++) nv[1 + i] = argv[i];
    nv[1 + ac] = NULL;

    /* env: append the loader library path default iff absent (copy of the
     * already-chain-merged envp; recursion re-merges our chain vars, that's
     * cheap and safe). */
    int ec = 0;
    while (merged_envp[ec]) ec++;
    char **nev = malloc(((size_t)ec + 3) * sizeof(char *));
    if (!nev) { free(nv); errno = ENOMEM; return -1; }
    int have_lib = 0, have_pre = 0;
    size_t le = strlen(libenv);
    for (int i = 0; i < ec; i++) {
        nev[i] = merged_envp[i];
        if (!have_lib && strncmp(nev[i], libenv, le) == 0 && nev[i][le] == '=') have_lib = 1;
        if (!have_pre && strncmp(nev[i], "BOX64_LD_PRELOAD=", 17) == 0) have_pre = 1;
    }
    /* ADR-0018 sysvipc shim: box64's PLT resolution is the ONLY lane the
     * emulated guest's SysV libc calls traverse — preload interception via
     * BOX64_LD_PRELOAD works for BOTH 64-bit and box32 children. The shim
     * runs entirely in the emulated guest (x86 or i386 ELF); its kernel-ABI
     * calls (int80/syscall) are translated by box64. */
    if (!have_pre && e_machine == EM_X86_64 && !getenv("SPROUT_SYSVIPC_OFF")
        && access("/usr/lib/sprout-sysvipc/x86_64/libsprout-sysvipc.so", R_OK) == 0) {
        nev[ec] = malloc(strlen("BOX64_LD_PRELOAD=/usr/lib/sprout-sysvipc/x86_64/libsprout-sysvipc.so") + 1);
        if (!nev[ec]) { free(nev); free(nv); errno = ENOMEM; return -1; }
        sprintf(nev[ec], "BOX64_LD_PRELOAD=/usr/lib/sprout-sysvipc/x86_64/libsprout-sysvipc.so");
        ec++;
    }
    if (!have_pre && e_machine == EM_386 && !getenv("SPROUT_SYSVIPC_OFF")
        && access("/usr/lib/sprout-sysvipc/i386/libsprout-sysvipc.so", R_OK) == 0) {
        nev[ec] = malloc(strlen("BOX64_LD_PRELOAD=/usr/lib/sprout-sysvipc/i386/libsprout-sysvipc.so") + 1);
        if (!nev[ec]) { free(nev); free(nv); errno = ENOMEM; return -1; }
        sprintf(nev[ec], "BOX64_LD_PRELOAD=/usr/lib/sprout-sysvipc/i386/libsprout-sysvipc.so");
        ec++;
    }
    if (!have_lib) {
        nev[ec] = malloc(le + 1 + strlen(libdef) + 1);
        if (!nev[ec]) { free(nev); free(nv); errno = ENOMEM; return -1; }
        sprintf(nev[ec], "%s=%s", libenv, libdef);
        ec++;
    }
    nev[ec] = NULL;

    int rc = sp_execve_chain(emu, nv, nev, depth + 1);
    /* only reached on exec FAILURE through the recursion (prints already) */
    free(nev);
    free(nv);
    (void)rc;
    return 1;
}

static int sp_execve_chain(const char *path, char *const argv[], char *const envp[], int depth) {
    if (depth > 4) return sp_chain_fail(path, depth, ELOOP, "depth");
    envp = (char *const *)sp_chain_env(envp);
    {
        char gabs[SP_PATH_MAX];
        if (path[0] == '/') {
            snprintf(gabs, sizeof(gabs), "%s", path);
        } else {
            char cw[SP_PATH_MAX], gcw[SP_PATH_MAX];
            ssize_t cn = readlink("/proc/self/cwd", cw, sizeof(cw) - 1);
            if (cn <= 0 || (size_t)cn >= sizeof(cw)) snprintf(cw, sizeof(cw), "/");
            else cw[cn] = '\0';
            sp_reverse(&g_cfg, cw, gcw, sizeof(gcw));
            snprintf(gabs, sizeof(gabs), "%s/%s", gcw, path);
        }
        /* SP_SCRIPT recurses with the interpreter; the recursion deep-stamps
         * the interpreter's own gabs — matching kernel semantics where
         * /proc/self/exe of a script is the INTERPRETER binary. */
        sp_stamp_exe((char **)envp, gabs);
    }
    char x[SP_PATH_MAX];
    const char *host_raw = sp_translate_x(path, x);
    char hx[SP_PATH_MAX];
    snprintf(hx, sizeof(hx), "%s", host_raw);
    sp_resolve_absolute_symlink(hx);
    const char *host = hx;

    {
        int brc = sp_binfmt_maybe_exec(path, host, argv, envp, depth);
        if (brc < 0) return sp_chain_fail(path, depth, ENOEXEC, "binfmt:no-emu");
        if (brc > 0) return -1; /* recursion already ran + reported */
    }

    char interp[SP_PATH_MAX], sopt[SP_PATH_MAX];
    int cls = sp_classify_host(host, interp, sopt);
    if (g_cfg.debug)
        fprintf(stderr, "[sprout] execve('%s') host='%s' class=%d\n", path, host, cls);
    sp_trace_exec(path, argv, cls);
    switch (cls) {
    case SP_ELF_DYNAMIC: {
        char *vstack[SP_CHAIN_MAX_ARGS + 8];
        int b = sp_build_loader_argv(vstack, SP_CHAIN_MAX_ARGS + 8, host, argv, 0, NULL,
                                     sp_chain_libpath(envp));
        if (b == -2) return sp_chain_fail(path, depth, EIO, "argv-build:no-loader");
        if (b != 0) return sp_chain_fail(path, depth, E2BIG, "argv-build:cap");
        {
            /* flight-recorder: what the loader exec is about to run.
             * apk's script pipes swallow stderr; only the tracelog sees. */
            const char *logf = getenv("SPROUT_TRACELOG");
            if (logf && *logf) {
                int fd = (int)syscall(SP_SYS_openat, AT_FDCWD, logf, O_WRONLY | O_APPEND | O_CREAT, 0600);
                if (fd >= 0) {
                    char buf[4096]; int n = 0;
                    n = snprintf(buf + n, sizeof(buf) - (size_t)n,
                                 "TREXEC pid=%d loader='%s' b=%d", (int)getpid(),
                                 getenv("SPROUT_LOADER") ? getenv("SPROUT_LOADER") : "(null)", b);
                    for (int i = 0; vstack[i] && i < 12 && n < (int)sizeof(buf) - 96; i++)
                        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " |%s", vstack[i]);
                    buf[n++] = '\n';
                    (void)!write(fd, buf, (size_t)n);
                    close(fd);
                }
            }
        }
        int rc = sp_real_execve(getenv("SPROUT_LOADER"), vstack, envp);
        if (rc < 0) return sp_chain_fail(path, depth, errno, "loader-execve");
        return rc;
    }
    case SP_SCRIPT: {
        /* script: interpret the shebang's interpreter via recursion, then
         * append script path (guest spelling) + remaining argv */
        char ires[SP_PATH_MAX];
        if (sp_guest_path_search(interp, ires) != 0) { errno = ENOENT; return -1; }
        /* build argv: [interp, opt?, script, argv+1...]
         * (Linux: the whole shebang tail after the interp word is ONE
         * token; apk's busybox trigger '#!/bin/busybox sh' proves it —
         * without the arg, busybox tried to run the script PATH as an
         * APPLET and exited 127. proot-equivalent chain omits nothing. */
        int argc = 0;
        while (argv[argc]) argc++;
        int has_opt = sopt[0] != '\0';
        if (argc + 2 + has_opt > SP_CHAIN_MAX_ARGS + 8) { errno = ENOMEM; return -1; }
        char *vchain[SP_CHAIN_MAX_ARGS + 8];
        vchain[0] = ires;
        int slot = 1;
        if (has_opt) vchain[slot++] = sopt;
        vchain[slot] = (char *)path;
        for (int k = 1; k < argc; k++) vchain[slot + k] = argv[k];
        vchain[slot + argc] = NULL;
        return sp_execve_chain(ires, vchain, envp, depth + 1);
    }
    case SP_ELF_STATIC: {
        /* Already under a ptrace supervisor (shadow mode): just exec the
         * static image; the supervisor's exec-stop reclassifies it into
         * full translation. Must NOT spawn a nested sprout-ptrace. */
        if (getenv("SPROUT_SUPERVISED"))
            return sp_real_execve(host, argv, envp);
        /* static guest inside a dynamic process: raw syscalls bypass the
         * interposer, so the supervisor (ptrace) must own the process.
         * sprout-cli hands the supervisor's path via SPROUT_PTRACE; it is
         * a HOST binary — the bionic loader would try to link the guest
         * glibc LD_PRELOAD it inherits, so LD_* is stripped before exec. */
        const char *px = getenv("SPROUT_PTRACE");
        if (!px) { errno = ENOENT; return -1; }
        int argc = 0;
        while (argv[argc]) argc++;
        char **v = malloc((size_t)(argc + 3) * sizeof(char *));
        if (!v) { errno = ENOMEM; return -1; }
        v[0] = (char *)px;
        v[1] = "--";
        v[2] = (char *)host;
        for (int k = 1; k < argc; k++) v[k + 2] = argv[k];
        v[argc + 2] = NULL;
        /* env minus LD_PRELOAD/LD_LIBRARY_PATH (host-unsafe for bionic) */
        int ec = 0;
        while (envp[ec]) ec++;
        char **e2 = malloc((size_t)(ec + 1) * sizeof(char *));
        if (!e2) { free(v); errno = ENOMEM; return -1; }
        int w = 0;
        for (int k = 0; k < ec; k++) {
            if (strncmp(envp[k], "LD_PRELOAD=", 11) == 0) continue;
            if (strncmp(envp[k], "LD_LIBRARY_PATH=", 16) == 0) continue;
            e2[w++] = envp[k];
        }
        e2[w] = NULL;
        int rc = sp_real_execve(px, v, e2);
        free(e2); free(v);
        return rc;
    }
    default: {
        /* glibc-parity errno: sp_classify_host() collapses ENOENT and
         * plain-data into SP_NOT_ELF, but callers MUST see fopen's real
         * errno for a missing/unreadable path (ENOENT, EACCES...) — with
         * a blanket ENOEXEC, glib's g_spawn PATH walker takes its
         * script-fallback branch ("execve returned ENOEXEC => try
         * /bin/sh <cand>") on candidate #1 (/usr/local/sbin/<name>),
         * so EVERY command living in a later PATH dir dies as dash's
         * "/bin/sh: 0: cannot open ..." — observed as xfce4-session
         * spawning zero components (2026-08-13). */
        FILE *pe = fopen(host, "rb");
        if (!pe) return -1;             /* errno already set by fopen */
        fclose(pe);
        errno = ENOEXEC;                /* genuine data file */
        return -1;
    }
    }
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    /* glibc binaries execve() never returns on success; the C interposer
     * branches into the guest loader chain or hands through for host-auto
     * cases (only for paths EXPLICITLY excluded from translation? none
     * today: everything goes through the chain when dynamic). */
    return sp_execve_chain(path, argv, envp, 0);
}

/* variadic exec family. glibc implements execl/execlp/execle via INSIDE-libc
 * calls to execve/execvp: PLT intercepts on execve/execvp never trigger for
 * them — DPKG's subprocess code uses execlp() for exactly the dpkg-split
 * reassembly step (found via objdump import tables). */
int execl(const char *path, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    char *a[128];
    int i = 0;
    if (arg) { a[i++] = (char *)arg; char *s; while ((s = va_arg(ap, char *)) && i < 127) a[i++] = s; }
    va_end(ap);
    a[i] = NULL;
    return sp_execve_chain(path, (char *const *)a, environ, 0);
}

int execle(const char *path, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    char *a[128];
    int i = 0;
    if (arg) { a[i++] = (char *)arg; char *s; while ((s = va_arg(ap, char *)) && i < 127) a[i++] = s; }
    char **envp = va_arg(ap, char **);
    va_end(ap);
    a[i] = NULL;
    return sp_execve_chain(path, (char *const *)a, envp ? (char *const *)envp : environ, 0);
}

int execlp(const char *file, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    char *a[128];
    int i = 0;
    if (arg) { a[i++] = (char *)arg; char *s; while ((s = va_arg(ap, char *)) && i < 127) a[i++] = s; }
    va_end(ap);
    a[i] = NULL;
    char cand[SP_PATH_MAX];
    if (sp_guest_path_search(file, cand) == 0)
        return sp_execve_chain(cand, (char *const *)a, environ, 0);
    errno = ENOENT;
    return -1;
}

int execv(const char *path, char *const argv[]) {
    return sp_execve_chain(path, argv, environ, 0);
}

int execvp(const char *path, char *const argv[]) {
    char cand[SP_PATH_MAX];
    if (sp_guest_path_search(path, cand) != 0) { errno = ENOENT; return -1; }
    return sp_execve_chain(cand, argv, environ, 0);
}

int execvpe(const char *path, char *const argv[], char *const envp[]) {
    char cand[SP_PATH_MAX];
    if (sp_guest_path_search(path, cand) != 0) { errno = ENOENT; return -1; }
    return sp_execve_chain(cand, argv, envp, 0);
}

int fexecve(int fd, char *const argv[], char *const envp[]) {
    /* Map fd → host path; if it sits inside the guest rootfs we can chain
     * normally, otherwise the host kernel must deal with it */
    char link[64], host[SP_PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, host, sizeof(host) - 1);
    if (n <= 0) { errno = ENOENT; return -1; }
    host[n] = '\0';
    char interp[SP_PATH_MAX];
    switch (sp_classify_host(host, interp, NULL)) {
    case SP_ELF_DYNAMIC: {
        char *vstack[SP_CHAIN_MAX_ARGS + 8];
        int b = sp_build_loader_argv(vstack, SP_CHAIN_MAX_ARGS + 8, host, argv, 0, NULL, NULL);
        if (b == -2) { errno = EIO; return -1; }
        if (b != 0) { errno = E2BIG; return -1; }
        return sp_real_execve(getenv("SPROUT_LOADER"), vstack, envp);
    }
    default:
        errno = ENOEXEC;
        return -1;
    }
}

/* glibc's system() bypasses our execve (it calls __execve internally),
 * so we implement it with the public fork/exec pair instead. */
int system(const char *command) {
    if (!command) {
        /* presence test: /bin/sh in guest */
        char x[SP_PATH_MAX];
        const char *p = sp_translate(&g_cfg, "/bin/sh", x) ? x : "/bin/sh";
        return access(p, X_OK) == 0;
    }
    /* keep it simple: fork + exec /bin/sh -c command via the shared chain.
     * (execl is unwrapped: glibc's private exec call would bypass our
     * translation — earlier apt Post-Invoke ran HOST /bin/sh + glibc chain
     * env, yielding 'bad ELF magic' when the loader met a linker script.) */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *const argv[] = {"sh", "-c", (char *)command, NULL};
        sp_execve_chain("/bin/sh", argv, environ, 0);
        { char eb[160]; int el = snprintf(eb, sizeof eb, "[sprout] system() chain exec failed errno=%d (%s)\n", errno, strerror(errno)); if (el > (int)sizeof eb) el = (int)sizeof eb; (void)!write(2, eb, (size_t)el); }
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return st;
}

/* glibc's popen()/pclose() pair suffers exactly the system() disease:
 * popen forks + execve's "/bin/sh -c cmd" via glibc's INTERNAL syscall
 * — the PLT-interposed execve chain NEVER RUNS, so the child lands on the
 * HOST /bin/sh (BIONIC mksh on Android Termux!) with a guest glibc
 * LD_LIBRARY_PATH around it. The bionic loader then walks its own default
 * dirs (system/odm/vendor lib64 — our notify lane translates those to
 * nonexistent $B/... paths) and prints:
 *   CANNOT LINK EXECUTABLE "sh": library "libc.so" not found
 * Observed: ONE such line per startxfce4 boot via xrdb -merge's cpp
 * preprocessing (xrdb imports popen, user log 2026-08-13-11h). Fix: own
 * the pair — popen becomes pipe2+fork+sp_execve_chain(/bin/sh) exactly
 * like system(); pclose pair-tracks pid by fd and waits manually.
 * (pid-map slots are static: chains under vfork-shared frames forbid
 * heap in spawn paths — ADR-0014.) */
#define SP_POPEN_MAX 24
static struct { int fd; pid_t pid; } sp_popen_map[SP_POPEN_MAX];
static int sp_popen_initd = 0;
static void sp_popen_reg(int fd, pid_t pid) {
    if (!sp_popen_initd) {
        for (int i = 0; i < SP_POPEN_MAX; i++) sp_popen_map[i].fd = -1;
        sp_popen_initd = 1;
    }
    for (int i = 0; i < SP_POPEN_MAX; i++)
        if (sp_popen_map[i].fd == -1 || sp_popen_map[i].fd == fd) {
            sp_popen_map[i].fd = fd; sp_popen_map[i].pid = pid; return;
        }
}
static pid_t sp_popen_lookup(int fd) {
    if (!sp_popen_initd) return -1;
    for (int i = 0; i < SP_POPEN_MAX; i++)
        if (sp_popen_map[i].fd == fd) { pid_t p = sp_popen_map[i].pid; sp_popen_map[i].fd = -1; sp_popen_map[i].pid = -1; return p; }
    return -1;
}

FILE *popen(const char *command, const char *type) {
    static FILE *(*SP_REAL(popen))(const char *, const char *) = NULL;
    SP_RESOLVE(popen);
    if (!command || (type[0] != 'r' && type[0] != 'w')) { errno = EINVAL; return NULL; }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return NULL;
    /* child ends on the READ side for 'w', WRITE side for 'r' */
    int child_fd = (type[0] == 'r') ? 1 : 0;
    int parent_fd = (type[0] == 'r') ? 0 : 1;
    FILE *fp = fdopen(fds[parent_fd], type);
    if (!fp) { close(fds[0]); close(fds[1]); return NULL; }
    pid_t pid = fork();
    if (pid == -1) { int e = errno; fclose(fp); close(fds[child_fd]); errno = e; return NULL; }
    if (pid == 0) {
        close(fds[parent_fd]);
        if (dup2(fds[child_fd], child_fd == 1 ? 1 : 0) == -1) _exit(127);
        close(fds[child_fd]);
        char *const argv[] = {"sh", "-c", (char *)command, NULL};
        sp_execve_chain("/bin/sh", argv, environ, 0);
        _exit(127);
    }
    close(fds[child_fd]);
    sp_popen_reg(fileno(fp), pid);
    return fp;
}

int pclose(FILE *stream) {
    static int (*SP_REAL(pclose))(FILE *) = NULL;
    SP_RESOLVE(pclose);
    if (!stream) { errno = EINVAL; return -1; }
    pid_t pid = sp_popen_lookup(fileno(stream));
    if (pid == -1) return SP_REAL(pclose)(stream);   /* not one of ours */
    fclose(stream);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return WTERMSIG(st);
    errno = ECHILD; return -1;
}

/* ------------------------------------------------------------------ */
/* posix_spawn/posix_spawnp: glibc's fast path calls __execvpe          */
/* internally, which bypasses PLT interposition entirely. We implement   */
/* the spawn contract as fork + setup + our own exec chain. POSIX        */
/* explicitly permits a fork/exec implementation.                        */
/* ------------------------------------------------------------------ */
#include <spawn.h>

/* libc-internal file_actions ABI. glibc: array of structs + __used count
 * (frozen since 2.2). musl: doubly-linked struct fdop list, newest at
 * head, executed tail→head (insertion order) — exactly what musl's own
 * child does. Introspected here exactly like proot/strace do. */
#if defined(__GLIBC__)
struct sp_spawn_action {
    enum {
        sp_spawn_do_close,
        sp_spawn_do_dup2,
        sp_spawn_do_open,
        sp_spawn_do_chdir,
        sp_spawn_do_fchdir
    } tag;
    union {
        struct { int fd; } close_action;
        struct { int fd; int newfd; } dup2_action;
        struct { int fd; char *path; int oflag; mode_t mode; } open_action;
        struct { char *path; } chdir_action;
        struct { int fd; } fchdir_action;
    } action;
};

static int sp_apply_file_actions(const posix_spawn_file_actions_t *fa) {
    if (!fa || !fa->__actions) return 0;
    const struct sp_spawn_action *acts =
        (const struct sp_spawn_action *)fa->__actions;
    for (int i = 0; i < fa->__used; i++) {
        const struct sp_spawn_action *a = &acts[i];
        switch (a->tag) {
        case sp_spawn_do_close:
            close(a->action.close_action.fd);
            break;
        case sp_spawn_do_dup2:
            if (dup2(a->action.dup2_action.fd, a->action.dup2_action.newfd) < 0)
                return errno;
            break;
        case sp_spawn_do_open: {
            int fd = open(a->action.open_action.path, a->action.open_action.oflag,
                          a->action.open_action.mode);
            if (fd < 0) return errno;
            if (fd != a->action.open_action.fd) {
                if (dup2(fd, a->action.open_action.fd) < 0) return errno;
                close(fd);
            }
            break;
        }
        case sp_spawn_do_chdir:
            if (chdir(a->action.chdir_action.path) < 0) return errno;
            break;
        case sp_spawn_do_fchdir:
            if (fchdir(a->action.fchdir_action.fd) < 0) return errno;
            break;
        default:
            break;
        }
    }
    return 0;
}

#define SP_ATTR_SIGMASK(a) ((a)->__ss)
#define SP_ATTR_SIGDEF(a)  ((a)->__sd)

#else /* __GLIBC__ not defined → musl */

/* musl 1.2.x internal ABI (from src/process/fdop.h): */
#define SP_FDOP_CLOSE  1
#define SP_FDOP_DUP2   2
#define SP_FDOP_OPEN   3
#define SP_FDOP_CHDIR  4
#define SP_FDOP_FCHDIR 5
struct sp_musl_fdop {
    struct sp_musl_fdop *next, *prev;
    int cmd, fd, srcfd, oflag;
    mode_t mode;
    char path[]; /* flexible array (only OPEN carries one) */
};

static int sp_apply_file_actions(const posix_spawn_file_actions_t *fa) {
    if (!fa || !fa->__actions) return 0;
    /* newest action is at the head; musl executes tail→head (insertion
     * order) — same traversal its own child does. */
    const struct sp_musl_fdop *op = fa->__actions;
    for (; op && op->next; op = op->next) {}
    for (; op; op = op->prev) {
        switch (op->cmd) {
        case SP_FDOP_CLOSE:
            close(op->fd);
            break;
        case SP_FDOP_DUP2:
            if (dup2(op->srcfd, op->fd) < 0) return errno;
            break;
        case SP_FDOP_OPEN: {
            int fd = open(op->path, op->oflag, op->mode);
            if (fd < 0) return errno;
            if (fd != op->fd) {
                if (dup2(fd, op->fd) < 0) return errno;
                close(fd);
            }
            break;
        }
        case SP_FDOP_CHDIR:
            if (chdir(op->path) < 0) return errno;
            break;
        case SP_FDOP_FCHDIR:
            if (fchdir(op->fd) < 0) return errno;
            break;
        default:
            return EIO;
        }
    }
    return 0;
}

#define SP_ATTR_SIGMASK(a) ((a)->__mask)
#define SP_ATTR_SIGDEF(a)  ((a)->__def)
#endif /* __GLIBC__ */

static int sp_spawn_impl(pid_t *restrict pid, const char *path,
                         const posix_spawn_file_actions_t *fa,
                         const posix_spawnattr_t *attrp,
                         char *const argv[], char *const envp[], int use_path) {
    short flags = attrp ? attrp->__flags : 0;

    pid_t child = fork();
    if (child < 0) return errno;
    if (child == 0) {
        /* --- child side of spawn --- */
        if (flags & POSIX_SPAWN_RESETIDS) { setgid(getgid()); setuid(getuid()); }
        if (flags & POSIX_SPAWN_SETPGROUP) setpgid(0, attrp->__pgrp);
        if (flags & POSIX_SPAWN_SETSIGMASK) {
            sigset_t m; memcpy(&m, &SP_ATTR_SIGMASK(attrp), sizeof(m));
            sigprocmask(SIG_SETMASK, &m, NULL);
        }
        if (flags & POSIX_SPAWN_SETSIGDEF) {
            struct sigaction dfl; memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            for (int s = 1; s < _NSIG; s++)
                if (sigismember(&SP_ATTR_SIGDEF(attrp), s)) sigaction(s, &dfl, NULL);
        }
        int err = sp_apply_file_actions(fa);
        if (err) { errno = err; _exit(127); }
        if (use_path) execvp(path, argv);
        else execve(path, argv, envp);
        { char eb[160]; int el = snprintf(eb, sizeof eb, "[sprout] spawn-child exec('%s') failed errno=%d (%s)\n", path ? path : "?", errno, strerror(errno)); if (el > (int)sizeof eb) el = (int)sizeof eb; (void)!write(2, eb, (size_t)el); }
        _exit(127);
    }
    if (pid) *pid = child;
    return 0;
}

int posix_spawn(pid_t *restrict pid, const char *restrict path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *restrict attrp,
                char *const argv[restrict], char *const envp[restrict]) {
    return sp_spawn_impl(pid, path, fa, attrp, argv, envp, 0);
}

int posix_spawnp(pid_t *restrict pid, const char *restrict file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *restrict attrp,
                 char *const argv[restrict], char *const envp[restrict]) {
    return sp_spawn_impl(pid, file, fa, attrp, argv, envp, 1);
}

/* uid/gid spoofing for -0 / --root-id.
 * Stateful model (apt DropPrivsOrDie verifies its own drop):
 * the id a fake-set*id call PROMISED is what the get* calls then
 * report (per-process; fresh execs start at the root profile).
 * The state itself is defined next to the set* wrappers above. */
uid_t getuid(void)  { return g_cfg.fakeroot ? g_fake_uid : geteuid(); }
uid_t geteuid(void) {
    static uid_t (*SP_REAL(geteuid))(void) = NULL;
    SP_RESOLVE(geteuid);
    return g_cfg.fakeroot ? g_fake_uid : SP_REAL(geteuid)();
}
gid_t getgid(void)  { return g_cfg.fakeroot ? g_fake_gid : getegid(); }
gid_t getegid(void) {
    static gid_t (*SP_REAL(getegid))(void) = NULL;
    SP_RESOLVE(getegid);
    return g_cfg.fakeroot ? g_fake_gid : SP_REAL(getegid)();
}

/* dlopen entry interception. glibc's ld.so opens libraries via ITS OWN
 * private __open* routines — a PLT interposer can never see those. What we
 * CAN intercept is the exported dlopen() entry point: translate the path,
 * hand ld.so an already-host-absolute spelling, and the internal open then
 * works. Dependency resolution of the dl'd object works because the running
 * ld.so is the guest's own initialized with LD_LIBRARY_PATH = translated,
 * --inhibit-cache (plan.rs sp_launch_env). Only ABSOLUTE paths engaging the
 * rootfs translate; bare soname lookups pass straight through. */
#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif
void *dlopen(const char *file, int mode) {
    static void *(*SP_REAL(dlopen))(const char *, int) = NULL;
    SP_RESOLVE(dlopen);
    if (!file || strchr(file, '/') == NULL) return SP_REAL(dlopen)(file, mode);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(file, x);
    SP_TRACE("dlopen", file, p);
    return SP_REAL(dlopen)(p, mode);
}

/* ADR-0020: sysv-shm emulation (termux libandroid-shmem protocol). */
#include "sprout_shm.inc"

#endif /* SPROUT_INTERPOSE */
