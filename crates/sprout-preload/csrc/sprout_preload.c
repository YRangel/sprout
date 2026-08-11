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
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#define SP_RESOLVE(name)                                                   \
    do {                                                                   \
        if (!SP_REAL(name)) SP_REAL(name) = dlsym(RTLD_NEXT, #name);       \
    } while (0)

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
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
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
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
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
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("openat", path, p);
    return SP_REAL(openat)(dirfd, p, flags, mode);
}

/* stat-family */
int stat(const char *path, struct stat *st) {
    static int (*SP_REAL(stat))(const char *, struct stat *) = NULL;
    SP_RESOLVE(stat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("stat", path, p);
    return SP_REAL(stat)(p, st);
}

int stat64(const char *path, struct stat64 *st) {
    static int (*SP_REAL(stat64))(const char *, struct stat64 *) = NULL;
    SP_RESOLVE(stat64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("stat64", path, p);
    return SP_REAL(stat64)(p, st);
}

int lstat(const char *path, struct stat *st) {
    static int (*SP_REAL(lstat))(const char *, struct stat *) = NULL;
    SP_RESOLVE(lstat);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("lstat", path, p);
    return SP_REAL(lstat)(p, st);
}

int lstat64(const char *path, struct stat64 *st) {
    static int (*SP_REAL(lstat64))(const char *, struct stat64 *) = NULL;
    SP_RESOLVE(lstat64);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("lstat64", path, p);
    return SP_REAL(lstat64)(p, st);
}

int access(const char *path, int mode) {
    static int (*SP_REAL(access))(const char *, int) = NULL;
    SP_RESOLVE(access);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("access", path, p);
    return SP_REAL(access)(p, mode);
}

int readlink(const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*SP_REAL(readlink))(const char *, char *, size_t) = NULL;
    SP_RESOLVE(readlink);
    if (bufsiz == 0) return SP_REAL(readlink)(path, buf, bufsiz);
    char x[SP_PATH_MAX];
    char target[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
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
    const char *p = sp_translate(&g_cfg, name, x) ? x : name;
    SP_TRACE("opendir", name, p);
    return SP_REAL(opendir)(p);
}

/* removal/creation/mutation */
int unlink(const char *path) {
    static int (*SP_REAL(unlink))(const char *) = NULL;
    SP_RESOLVE(unlink);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("unlink", path, p);
    return SP_REAL(unlink)(p);
}

int rmdir(const char *path) {
    static int (*SP_REAL(rmdir))(const char *) = NULL;
    SP_RESOLVE(rmdir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("rmdir", path, p);
    return SP_REAL(rmdir)(p);
}

int mkdir(const char *path, mode_t mode) {
    static int (*SP_REAL(mkdir))(const char *, mode_t) = NULL;
    SP_RESOLVE(mkdir);
    char x[SP_PATH_MAX];
    const char *p = sp_translate(&g_cfg, path, x) ? x : path;
    SP_TRACE("mkdir", path, p);
    return SP_REAL(mkdir)(p, mode);
}

int rename(const char *oldpath, const char *newpath) {
    static int (*SP_REAL(rename))(const char *, const char *) = NULL;
    SP_RESOLVE(rename);
    char xo[SP_PATH_MAX], xn[SP_PATH_MAX];
    const char *po = sp_translate(&g_cfg, oldpath, xo) ? xo : oldpath;
    const char *pn = sp_translate(&g_cfg, newpath, xn) ? xn : newpath;
    SP_TRACE("rename", oldpath, po);
    return SP_REAL(rename)(po, pn);
}

int symlink(const char *target, const char *linkpath) {
    static int (*SP_REAL(symlink))(const char *, const char *) = NULL;
    SP_RESOLVE(symlink);
    char x[SP_PATH_MAX];
    const char *lp = sp_translate(&g_cfg, linkpath, x) ? x : linkpath;
    /* target is guest-spelled by design; the reverse path is what readlink
     * hands back, so both directions stay consistent. */
    SP_TRACE("symlink", linkpath, lp);
    return SP_REAL(symlink)(target, lp);
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
