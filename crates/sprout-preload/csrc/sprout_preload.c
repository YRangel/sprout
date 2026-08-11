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

    /* passthrough: default kernel pseudo-fs; override via env */
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

    /* Never translate host pseudo-filesystem mountpoints. */
    for (int i = 0; i < cfg->npassthrough; i++) {
        if (path_within(cfg->passthrough[i].prefix, cfg->passthrough[i].len, path))
            return 0;
    }

    for (int i = 0; i < cfg->nbinds; i++) {
        const sp_bind_t *b = &cfg->binds[i];
        if (!path_within(b->guest, b->guest_len, path)) continue;
        size_t rest = strlen(path + b->guest_len);
        if (b->host_len + rest + 1 > SP_PATH_MAX) return 0;
        memcpy(out, b->host, b->host_len);
        memcpy(out + b->host_len, path + b->guest_len, rest + 1);
        return 1;
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

__attribute__((constructor)) static void sprout_init(void) {
    sp_config_load(&g_cfg);
}

#define SP_TRACE(name, orig, trans)                                        \
    do {                                                                   \
        if (g_cfg.debug) {                                                 \
            fprintf(stderr, "[sprout] %s(\"%s\") -> \"%s\"\n", name,        \
                    orig ? orig : "(null)", trans);                        \
        }                                                                  \
    } while (0)

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

/* Translate + absolute-symlink chase. Alpine lays every applet out as an
 * absolute symlink to /bin/busybox; the host kernel would resolve those
 * targets on the HOST (missing). Only when the translation moved the path
 * do we chase (host passthrough paths are the kernel's business).
 * RTLD_NEXT syscalls keep us free of interposer recursion. */
static int (*sp_real_lstat)(const char *, struct stat *) = NULL;
static ssize_t (*sp_real_readlink)(const char *, char *, size_t) = NULL;
static const char *sp_translate_x(const char *path, char buf[SP_PATH_MAX]) {
    const char *out = sp_translate(&g_cfg, path, buf) ? buf : path;
    if (out != buf) return out;
    static int l2s_off = -1;
    if (l2s_off < 0) l2s_off = getenv("SPROUT_DISABLE_L2S") ? 1 : 0;
    if (l2s_off) return out;
    char dir[SP_PATH_MAX], tmp[SP_PATH_MAX], lnk[SP_PATH_MAX];
    for (int hop = 0; hop < 8; hop++) {
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
            snprintf(tmp, sizeof(tmp), "%s/%s", dir, lnk);
            snprintf(buf, SP_PATH_MAX, "%s", tmp);
        }
    }
    return buf;
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

int openat(int dirfd, const char *path, int flags, ...) {
    static int (*SP_REAL(openat))(int, const char *, int, ...) = NULL;
    SP_RESOLVE(openat);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t); va_end(ap);
    }
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
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("openat64", path, p);
    return SP_REAL(openat64)(dirfd, p, flags, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    static int (*SP_REAL(faccessat))(int, const char *, int, int) = NULL;
    SP_RESOLVE(faccessat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("faccessat", path, p);
    return SP_REAL(faccessat)(dirfd, p, mode, flags);
}

int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *buf) {
    static int (*SP_REAL(statx))(int, const char *, int, unsigned int, struct statx *) = NULL;
    SP_RESOLVE(statx);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("statx", path, p);
    return SP_REAL(statx)(dirfd, p, flags, mask, buf);
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    static int (*SP_REAL(mkdirat))(int, const char *, mode_t) = NULL;
    SP_RESOLVE(mkdirat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("mkdirat", path, p);
    return SP_REAL(mkdirat)(dirfd, p, mode);
}

int unlinkat(int dirfd, const char *path, int flags) {
    static int (*SP_REAL(unlinkat))(int, const char *, int) = NULL;
    SP_RESOLVE(unlinkat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("unlinkat", path, p);
    return SP_REAL(unlinkat)(dirfd, p, flags);
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*SP_REAL(readlinkat))(int, const char *, char *, size_t) = NULL;
    SP_RESOLVE(readlinkat);
    if (bufsiz == 0) return SP_REAL(readlinkat)(dirfd, path, buf, bufsiz);
    char x[SP_PATH_MAX];
    char target[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    ssize_t n = SP_REAL(readlinkat)(dirfd, p, target, SP_PATH_MAX - 1);
    if (n <= 0) return n;
    target[n] = '\0';
    n = (ssize_t)sp_reverse(&g_cfg, target, target, sizeof(target));
    if ((size_t)n > bufsiz) n = (ssize_t)bufsiz;
    memcpy(buf, target, (size_t)n);
    SP_TRACE("readlinkat", path, target);
    return n;
}

int chdir(const char *path) {
    static int (*SP_REAL(chdir))(const char *) = NULL;
    SP_RESOLVE(chdir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("chdir", path, p);
    return SP_REAL(chdir)(p);
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

static char *sp_strdup(const char *out) {
    size_t n = strlen(out) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, out, n);
    return p;
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
        resolved = sp_strdup(out);
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
    return SP_REAL(stat)(p, st);
}

int stat64(const char *path, struct stat64 *st) {
    static int (*SP_REAL(stat64))(const char *, struct stat64 *) = NULL;
    SP_RESOLVE(stat64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("stat64", path, p);
    return SP_REAL(stat64)(p, st);
}

int lstat(const char *path, struct stat *st) {
    static int (*SP_REAL(lstat))(const char *, struct stat *) = NULL;
    SP_RESOLVE(lstat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("lstat", path, p);
    return SP_REAL(lstat)(p, st);
}

int lstat64(const char *path, struct stat64 *st) {
    static int (*SP_REAL(lstat64))(const char *, struct stat64 *) = NULL;
    SP_RESOLVE(lstat64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("lstat64", path, p);
    return SP_REAL(lstat64)(p, st);
}

int access(const char *path, int mode) {
    static int (*SP_REAL(access))(const char *, int) = NULL;
    SP_RESOLVE(access);
    char x[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
    SP_TRACE("access", path, p);
    return SP_REAL(access)(p, mode);
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*SP_REAL(readlink))(const char *, char *, size_t) = NULL;
    SP_RESOLVE(readlink);
    if (bufsiz == 0) return SP_REAL(readlink)(path, buf, bufsiz);
    char x[SP_PATH_MAX];
    char target[SP_PATH_MAX];
    const char *p = sp_translate_x(path, x);
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
    const char *p = sp_translate_x(path, x);
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
    const char *p = sp_translate_x(path, x);
    SP_TRACE("mkdir", path, p);
    return SP_REAL(mkdir)(p, mode);
}

int rename(const char *oldpath, const char *newpath) {
    static int (*SP_REAL(rename))(const char *, const char *) = NULL;
    SP_RESOLVE(rename);
    char xo[SP_PATH_MAX], xn[SP_PATH_MAX];
    const char *po = sp_translate_x(oldpath, xo);
    const char *pn = sp_translate_x(newpath, xn);
    SP_TRACE("rename", oldpath, po);
    return SP_REAL(rename)(po, pn);
}

int symlink(const char *target, const char *linkpath) {
    static int (*SP_REAL(symlink))(const char *, const char *) = NULL;
    SP_RESOLVE(symlink);
    char x[SP_PATH_MAX];
    const char *lp = sp_translate_x(linkpath, x);
    /* target is guest-spelled by design; the reverse path is what readlink
     * hands back, so both directions stay consistent. */
    SP_TRACE("symlink", linkpath, lp);
    return SP_REAL(symlink)(target, lp);
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
            snprintf(host, SP_PATH_MAX, "%s/%s", dir, target);
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
static int sp_classify_host(const char *host, char interp[SP_PATH_MAX]) {
    FILE *f = fopen(host, "rb");
    if (!f) return SP_NOT_ELF;
    unsigned char head[256];
    size_t n = fread(head, 1, sizeof(head), f);
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        /* script: copy the interpreter word into interp */
        size_t i = 2;
        while (i < n && (head[i] == ' ' || head[i] == '\t')) i++;
        size_t j = 0;
        while (i < n && j < SP_PATH_MAX - 1 && head[i] != ' ' && head[i] != '\t'
               && head[i] != '\n' && head[i] != '\r') {
            interp[j++] = (char)head[i++];
        }
        interp[j] = '\0';
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
        /* already path-qualified */
        if (strlen(name) >= SP_PATH_MAX) return -1;
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

/* Build the loader-chain argv for a dynamic guest program:
 *   loader --argv0 <orig> --inhibit-cache --library-path <lp> <hostprog> [args...]
 * into into allocated memory; caller frees with sp_free_argv. */
static char **sp_build_loader_argv(const char *host_prog, char *const argv[],
                                   int extra, int *outc) {
    int argc = 0;
    while (argv[argc]) argc++;
    const char *loader = getenv("SPROUT_LOADER");
    const char *lp = getenv("SPROUT_LIBRARY_PATH");
    const char *libc_kind = getenv("SPROUT_LIBC"); /* "musl" or "glibc" (default) */
    if (!loader) return NULL;
    int musl = libc_kind && strcmp(libc_kind, "musl") == 0;
    /* musl ldso has no cache: --inhibit-cache is a glibc-only flag and
     * unknown options would make it bail out. */
    int fixed = (6 + extra) - (musl ? 1 : 0);
    char **v = malloc((size_t)(fixed + argc) * sizeof(char *));
    if (!v) return NULL;
    int i = 0;
    v[i++] = (char *)loader;
    v[i++] = "--argv0";
    v[i++] = argv[0] ? argv[0] : (char *)host_prog;
    if (!musl) v[i++] = "--inhibit-cache";
    v[i++] = "--library-path";
    v[i++] = (char *)(lp ? lp : "");
    v[i++] = (char *)host_prog;
    for (int k = 1; k < argc; k++) v[i++] = argv[k];
    v[i] = NULL;
    if (outc) *outc = i;
    return v;
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

static int sp_execve_chain(const char *path, char *const argv[], char *const envp[], int depth) {
    if (depth > 4) { errno = ELOOP; return -1; }
    char x[SP_PATH_MAX];
    const char *host_raw = sp_translate_x(path, x);
    char hx[SP_PATH_MAX];
    snprintf(hx, sizeof(hx), "%s", host_raw);
    sp_resolve_absolute_symlink(hx);
    const char *host = hx;

    char interp[SP_PATH_MAX];
    int cls = sp_classify_host(host, interp);
    if (g_cfg.debug)
        fprintf(stderr, "[sprout] execve('%s') host='%s' class=%d\n", path, host, cls);
    switch (cls) {
    case SP_ELF_DYNAMIC: {
        char **v = sp_build_loader_argv(host, argv, 0, NULL);
        if (!v) { errno = EIO; return -1; }
        int rc = sp_real_execve(getenv("SPROUT_LOADER"), v, envp);
        free(v);
        return rc;
    }
    case SP_SCRIPT: {
        /* script: interpret the shebang's interpreter via recursion, then
         * append script path (guest spelling) + remaining argv */
        char ires[SP_PATH_MAX];
        if (sp_guest_path_search(interp, ires) != 0) { errno = ENOENT; return -1; }
        /* build argv: [interp, script, argv+1...] */
        int argc = 0;
        while (argv[argc]) argc++;
        char **v = malloc((size_t)(argc + 2) * sizeof(char *));
        if (!v) { errno = ENOMEM; return -1; }
        v[0] = ires;
        v[1] = (char *)path;
        for (int k = 1; k < argc; k++) v[1 + k] = argv[k];
        v[1 + argc] = NULL;
        int rc = sp_execve_chain(ires, v, envp, depth + 1);
        free(v);
        return rc;
    }
    case SP_ELF_STATIC:
        /* static guest: no libc to interpose, raw syscalls bypass us;
         * full support = v0.3 supervisor. */
        errno = EINVAL;
        if (g_cfg.debug)
            fprintf(stderr, "[sprout] execve: static ELF '%s' needs the supervisor (v0.3)\n", path);
        return -1;
    default:
        errno = ENOEXEC;
        return -1;
    }
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    /* glibc binaries execve() never returns on success; the C interposer
     * branches into the guest loader chain or hands through for host-auto
     * cases (only for paths EXPLICITLY excluded from translation? none
     * today: everything goes through the chain when dynamic). */
    return sp_execve_chain(path, argv, envp, 0);
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
    switch (sp_classify_host(host, interp)) {
    case SP_ELF_DYNAMIC: {
        char **v = sp_build_loader_argv(host, argv, 0, NULL);
        if (!v) { errno = EIO; return -1; }
        int rc = sp_real_execve(getenv("SPROUT_LOADER"), v, envp);
        free(v);
        return rc;
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
    /* keep it simple: fork + exec /bin/sh -c command */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return st;
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

/* uid/gid spoofing for -0 / --root-id */
uid_t getuid(void)  { return g_cfg.fakeroot ? 0 : geteuid(); }
uid_t geteuid(void) {
    static uid_t (*SP_REAL(geteuid))(void) = NULL;
    SP_RESOLVE(geteuid);
    return g_cfg.fakeroot ? 0 : SP_REAL(geteuid)();
}
gid_t getgid(void)  { return g_cfg.fakeroot ? 0 : getegid(); }
gid_t getegid(void) {
    static gid_t (*SP_REAL(getegid))(void) = NULL;
    SP_RESOLVE(getegid);
    return g_cfg.fakeroot ? 0 : SP_REAL(getegid)();
}

#endif /* SPROUT_INTERPOSE */
