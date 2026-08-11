/*
 * sprout_preload.h — shared types + pure translation functions for the
 * sprout LD_PRELOAD core.
 *
 * The translation functions (sp_translate / sp_reverse) take an explicit
 * config and use no globals, so they are unit-testable on any host
 * (see csrc/tests/test_translate.c). The interposition wrappers in
 * sprout_preload.c bind them to a singleton config loaded at
 * library-constructor time.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef SPROUT_PRELOAD_H
#define SPROUT_PRELOAD_H

#include <stddef.h>

/* Fixed limits: no malloc anywhere in the hot path. */
#define SP_PATH_MAX 4096
#define SP_MAX_BINDS 64

typedef struct {
    /* Absolute host directory that is this binding's real location. */
    char host[SP_PATH_MAX];
    size_t host_len;
    /* Absolute guest path where the host directory appears. */
    char guest[SP_PATH_MAX];
    size_t guest_len;
} sp_bind_t;

typedef struct {
    /* Absolute host path of the guest root (no trailing slash, unless "/"). */
    char rootfs[SP_PATH_MAX];
    size_t rootfs_len;

    /* Bindings, pre-sorted longest-guest-first at parse time. */
    sp_bind_t binds[SP_MAX_BINDS];
    int nbinds;

    int debug;          /* SPROUT_DEBUG=1: trace every translation */
    int fakeroot;       /* -0 / --root-id: spoof uid/gid 0 */
    int link2symlink;   /* --link2symlink: turn hardlinks into symlinks */
} sp_config_t;

/* Parse SPROUT_* environment into cfg. Idempotent; safe to call any time. */
void sp_config_load(sp_config_t *cfg);

/*
 * Translate a guest path into its host path.
 *
 *   out   — caller-provided buffer of SP_PATH_MAX bytes
 *   return — 1 if `out` was written (translation happened), 0 if the caller
 *            should use the original path unchanged (relative paths, paths
 *            already inside rootfs — the idempotence guard).
 *
 * Rules (in order):
 *   1. bindings (longest guest-prefix match wins)
 *   2. already-inside-rootfs guard (keeps us idempotent on re-entry)
 *   3. prefix with rootfs
 */
int sp_translate(const sp_config_t *cfg, const char *path, char out[SP_PATH_MAX]);

/*
 * Reverse-translate a host path back to its guest spelling, e.g. to make
 * readlink() results look right to the guest. Returns bytes written
 * (excluding NUL), or the untranslated length if no rule applied.
 */
size_t sp_reverse(const sp_config_t *cfg, const char *host, char *out, size_t outsz);

#endif /* SPROUT_PRELOAD_H */
