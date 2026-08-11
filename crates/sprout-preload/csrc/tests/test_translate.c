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

static sp_config_t make_cfg(void) {
    sp_config_t c;
    memset(&c, 0, sizeof(c));
    strcpy(c.rootfs, "/data/local/rootfs");
    c.rootfs_len = strlen(c.rootfs);

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

    if (failures == 0) {
        puts("test_translate: all OK");
        return 0;
    }
    printf("test_translate: %d failure(s)\n", failures);
    return failures;
}
