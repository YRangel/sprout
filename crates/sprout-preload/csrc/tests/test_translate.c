/*
 * test_translate.c — host-run unit tests for the pure translation core.
 *
 * Compiled without SPROUT_INTERPOSE so no libc symbols are overridden in
 * the test process itself; only sp_translate()/sp_reverse()/sp_config are
 * exercised. Exit status is the number of failed assertions.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "../sprout_preload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(desc, cond)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, desc);\
        }                                                       \
    } while (0)

#define EXPECT_T(cfg, in, want)                                 \
    do {                                                        \
        char out[SP_PATH_MAX];                                  \
        int r = sp_translate((cfg), (in), out);                 \
        if (!r || strcmp(out, (want)) != 0) {                   \
            failures++;                                         \
            printf("FAIL translate(\"%s\") -> %s\"%s\", want \"%s\"\n", \
                   (in), r ? "" : "(none) ", r ? out : "", (want)); \
        }                                                       \
    } while (0)

#define EXPECT_NONE(cfg, in)                                    \
    do {                                                        \
        char out[SP_PATH_MAX];                                  \
        int r = sp_translate((cfg), (in), out);                 \
        if (r) {                                                \
            failures++;                                         \
            printf("FAIL translate(\"%s\") translated to \"%s\", want passthrough\n", \
                   (in) ? (in) : "(null)", out);                                  \
        }                                                       \
    } while (0)

#define EXPECT_REV(cfg, in, want)                               \
    do {                                                        \
        char out[SP_PATH_MAX];                                  \
        sp_reverse((cfg), (in), out, sizeof(out));              \
        if (strcmp(out, (want)) != 0) {                         \
            failures++;                                         \
            printf("FAIL reverse(\"%s\") -> \"%s\", want \"%s\"\n", \
                   (in), out, (want));                          \
        }                                                       \
    } while (0)

static int main_ancestor_bind(void);
static sp_config_t make_cfg(void) {
    sp_config_t c;
    memset(&c, 0, sizeof(c));
    strcpy(c.rootfs, "/data/local/rootfs");
    c.rootfs_len = strlen(c.rootfs);
    /* default passthroughs (matches sp_config_load) */
    static const char *pt[] = { "/proc", "/sys", "/dev" };
    for (int i = 0; i < 3; i++) {
        c.passthrough[c.npassthrough].prefix = pt[i];
        c.passthrough[c.npassthrough].len = strlen(pt[i]);
        c.npassthrough++;
    }

    strcpy(c.binds[0].host, "/sdcard");
    c.binds[0].host_len = strlen(c.binds[0].host);
    strcpy(c.binds[0].guest, "/mnt/sdcard");
    c.binds[0].guest_len = strlen(c.binds[0].guest);

    strcpy(c.binds[1].host, "/data/local/home-u1");
    c.binds[1].host_len = strlen(c.binds[1].host);
    strcpy(c.binds[1].guest, "/home/u1");
    c.binds[1].guest_len = strlen(c.binds[1].guest);

    /* longest-guest-first, as sp_config_load() produces */
    c.nbinds = 2;
    return c;
}

int main(void) {
    sp_config_t cfg = make_cfg();

    /* plain prefixing */
    EXPECT_T(&cfg, "/usr/bin/node", "/data/local/rootfs/usr/bin/node");
    EXPECT_T(&cfg, "/", "/data/local/rootfs/");

    /* bindings beat rootfs */
    EXPECT_T(&cfg, "/home/u1/code", "/data/local/home-u1/code");
    EXPECT_T(&cfg, "/mnt/sdcard/dl/x.zip", "/sdcard/dl/x.zip");

    /* relative & already-translated pass through */
    EXPECT_NONE(&cfg, "etc/passwd");
    EXPECT_NONE(&cfg, "./rel/path");
    EXPECT_NONE(&cfg, "/data/local/rootfs/usr/bin/node");
    EXPECT_NONE(&cfg, NULL);

    /* segment-boundary safety: a path that merely *starts with* the
     * rootfs string is NOT considered already-translated */
    EXPECT_T(&cfg, "/data/local/rootfs2/x", "/data/local/rootfs/data/local/rootfs2/x");

    /* reverse path (readlink results) */
    EXPECT_REV(&cfg, "/data/local/rootfs/usr/bin/node", "/usr/bin/node");
    EXPECT_REV(&cfg, "/data/local/home-u1/x", "/home/u1/x");
    EXPECT_REV(&cfg, "/proc/cpuinfo", "/proc/cpuinfo");

    /* passthrough: default kernel pseudo-fs must NEVER be translated */
    EXPECT_NONE(&cfg, "/proc/cpuinfo");
    EXPECT_NONE(&cfg, "/dev/urandom");
    EXPECT_NONE(&cfg, "/sys/class");
    /* but unset-as-default bind-ish prefixes DO translate normally */
    EXPECT_T(&cfg, "/apex/x", "/data/local/rootfs/apex/x");
    EXPECT_T(&cfg, "/system/app", "/data/local/rootfs/system/app");

    /* no rootfs: everything passes through */
    sp_config_t empty;
    memset(&empty, 0, sizeof(empty));
    EXPECT_NONE(&empty, "/usr/bin/node");

    /* config cloning safety: loading twice doesn't accumulate junk */
    sp_config_t a, b;
    setenv("SPROUT_ROOTFS", "/r", 1);
    setenv("SPROUT_BIND", "/h=/g", 1);
    unsetenv("SPROUT_DEBUG");
    sp_config_load(&a);
    sp_config_load(&b);
    CHECK("idempotent load", a.nbinds == b.nbinds && a.nbinds == 1);
    CHECK("rootfs len", a.rootfs_len == 2);

    if (main_ancestor_bind() != 0) {
        failures++;
        printf("FAIL ancestor_bind suite\n");
    }
    if (failures == 0) {
        puts("test_translate: all OK");
        return 0;
    }
    printf("test_translate: %d failure(s)\n", failures);
    return failures;
}

/* regression: a bind whose HOST side merely ANCESTORS the rootfs
 * (the proot-distro "--bind=$PREFIX" shape, where the rootfs lives
 * INSIDE $PREFIX/var/...) must not hijack the reverse-mapping of
 * guest-root paths */
static int main_ancestor_bind(void) {
    sp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.rootfs, "/pfx/var/lib/containers/ubuntu/rootfs");
    cfg.rootfs_len = strlen(cfg.rootfs);

    strcpy(cfg.binds[0].host, "/pfx");
    cfg.binds[0].host_len = strlen(cfg.binds[0].host);
    strcpy(cfg.binds[0].guest, "/pfx");
    cfg.binds[0].guest_len = strlen(cfg.binds[0].guest);
    cfg.nbinds = 1;

    char out[SP_PATH_MAX];
    int f = 0;
    size_t n;
    /* rootfs path must win over the shallow ancestor bind: */
    n = sp_reverse(&cfg, "/pfx/var/lib/containers/ubuntu/rootfs/root"
                    , out, sizeof(out));
    if (n == 0 || strcmp(out, "/root") != 0) {
        f++;
        printf("FAIL reverse(rootfs-root) -> \"%s\", want \"/root\"\n", out);
    }
    /* path OUTSIDE the rootfs still reverses through the identity bind: */
    n = sp_reverse(&cfg, "/pfx/bin/ls", out, sizeof(out));
    if (n == 0 || strcmp(out, "/pfx/bin/ls") != 0) {
        f++;
        printf("FAIL reverse(/pfx/bin/ls) -> \"%s\", want identity\n", out);
    }
    /* the specific-in-rootfs bind still beats rootfs: */
    cfg.nbinds = 2;
    strcpy(cfg.binds[1].host, "/pfx/var/lib/containers/ubuntu/rootfs/tmp");
    cfg.binds[1].host_len = strlen(cfg.binds[1].host);
    strcpy(cfg.binds[1].guest, "/dev/shm");
    cfg.binds[1].guest_len = strlen(cfg.binds[1].guest);
    n = sp_reverse(&cfg, "/pfx/var/lib/containers/ubuntu/rootfs/tmp/x.shm", out, sizeof(out));
    if (n == 0 || strcmp(out, "/dev/shm/x.shm") != 0) {
        f++;
        printf("FAIL reverse(shm) -> \"%s\", want \"/dev/shm/x.shm\"\n", out);
    }
    return f;
}
