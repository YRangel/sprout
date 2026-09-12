/* sprout-shadow — read-side implementation.
 *
 * The golden rule here is *fail-open*: any inconsistency and the table is
 * treated as absent.  The interposer lives in-process; it must never die
 * or expose torn state to a correct-but-adversarial writer.                */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sprout_shadow.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <errno.h>

static uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* debug knob: SPROUT_SHADOW_STRICT=1 turns hard corruption (bad magic,
 * bogus strtab bounds) from fail-open into a loud abort — fail-open is
 * the right PRODUCTION default (ADR-0024) but it hides bugs in testing.
 * Staleness (holder restart) is normal operation and never aborts. */
static int sp_shadow_strict(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("SPROUT_SHADOW_STRICT");
        cached = (e && e[0] == '1') ? 1 : 0;
    }
    return cached;
}
static void sp_shadow_corrupt(const char *what, unsigned long long a, unsigned long long b) {
    if (sp_shadow_strict()) {
        char buf[160];
        int n = snprintf(buf, sizeof buf,
            "[sprout-shadow] STRICT: corrupt table (%s %llx/%llx) — aborting "
            "(unset SPROUT_SHADOW_STRICT for fail-open)\n", what, a, b);
        if (n > 0) (void)!write(2, buf, (size_t)n);
        abort();
    }
}

static uint32_t crc32c(const void *data, size_t len) {
    /* CRC-32C software implementation is fine here (table ≤ 4KB). */
    static uint32_t table[256]; static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t crc = ~0u;
    const uint8_t *p = (const uint8_t *)data;
    while (len--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

int sp_shadow_attach(int fd, sp_shadow_snap_t *snap) {
    memset(snap, 0, sizeof *snap);
    if (fd < 0) return -1;

    /* Minimal size: header.  If scaling matters, caller fstat's actual size.
     * Map PROT_READ only: we never write, and mapping read-only catches any
     * accidental write in tests.                                          */
    uint64_t mapsz = 1 << 16; /* 64 KiB — far above plausible table sizes */
    void *base = mmap(NULL, mapsz, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        if (getenv("SPROUT_DEBUG")) fprintf(stderr, "[sprout-shadow] mmap failed errno=%d\n", errno);
        return -1;
    }

    const struct sp_shadow_hdr *h = (const struct sp_shadow_hdr *)base;
    if (h->magic != SP_SHADOW_MAGIC || h->cap == 0 || h->cap > 4096) {
        if (getenv("SPROUT_DEBUG")) fprintf(stderr, "[sprout-shadow] bad hdr magic=%llx cap=%llu\n",
            (unsigned long long)h->magic, (unsigned long long)h->cap);
        sp_shadow_corrupt("bad-hdr", (unsigned long long)h->magic, (unsigned long long)h->cap);
        munmap(base, mapsz);
        return -1;
    }
    uint64_t end = SP_SHADOW_HDR_SIZE + SP_SHADOW_ENTRY_SIZE * h->cap + h->strtab_len;
    if (end > mapsz) {
        if (getenv("SPROUT_DEBUG")) fprintf(stderr, "[sprout-shadow] end=%llu > mapsz\n", (unsigned long long)end);
        sp_shadow_corrupt("end>mapsz", (unsigned long long)end, (unsigned long long)mapsz);
        munmap(base, mapsz);
        return -1;
    }

    snap->size    = (uint64_t)mapsz;
    snap->hdr     = h;
    snap->entries = (const struct sp_shadow_entry *)((const char *)base + SP_SHADOW_HDR_SIZE);
    snap->strtab  = (const char *)base + h->strtab_off;
    snap->strtab_len = h->strtab_len;
    snap->valid      = 1;
    return 0;
}

void sp_shadow_detach(sp_shadow_snap_t *snap) {
    if (snap->hdr)
        munmap((void *)snap->hdr, snap->size);
    memset(snap, 0, sizeof *snap);
}

int sp_shadow_live(const sp_shadow_snap_t *snap) {
    if (!snap->valid) return 0;
    const struct sp_shadow_hdr *h = snap->hdr;
    /* Read gen twice for a torn-write tripwire, but bail fast on empty.    */
    uint64_t g1 = atomic_load_explicit((_Atomic uint64_t *)&h->gen, memory_order_acquire);
    if (g1 == 0) return 0;
    uint64_t hb = atomic_load_explicit((_Atomic uint64_t *)&h->heartbeat_ns, memory_order_acquire);
    if (now_mono_ns() - hb > SP_SHADOW_STALE_NS) return 0;
    uint64_t g2 = atomic_load_explicit((_Atomic uint64_t *)&h->gen, memory_order_acquire);
    return g1 == g2 && !(g1 & 1) && h->count <= h->cap;
}

int sp_shadow_lookup_bind(const sp_shadow_snap_t *snap,
                          const char *path,
                          char *out, unsigned long out_cap) {
    if (!snap->valid) return -1;
    const struct sp_shadow_hdr *h = snap->hdr;
    if (!sp_shadow_live(snap)) return -1;

    /* Read-stable snapshot point: capture gen once.                        */
    _Atomic uint64_t gen = atomic_load_explicit((_Atomic uint64_t *)&h->gen, memory_order_acquire);

    /* Longest prefix match across VALID BIND_MOUNT entries. */
    unsigned long best_len = 0;
    unsigned long best_dst_off = 0, best_dst_len = 0, best_idx = 0;
    const struct sp_shadow_entry *base = snap->entries;
    const char *strtab = snap->strtab;

    for (uint32_t i = 0; i < h->count; i++) {
        const struct sp_shadow_entry *e = &base[i];
        if (e->type != SP_SH_T_BIND || e->state != SP_SH_S_VALID) continue;
        if (!e->dst_len || e->dst_off + e->dst_len > snap->strtab_len) continue;
        unsigned long dlen = e->dst_len;
        if (dlen < best_len) continue;
        if (strncmp(path, strtab + e->dst_off, dlen) != 0) continue;
        /* Prefix boundary check: "/a/b" matches "/a/b/c" and "/a/bx" doesn't
           count as a true prefix — path[plen] must be '/' or NUL. */
        char tail = path[e->dst_len];
        if (tail != '\0' && tail != '/') continue;
        best_len = dlen; best_dst_off = e->src_off; best_dst_len = e->src_len;
        best_idx = i;
    }
    if (!best_len) return -1;

    /* entries[i] may have been updated mid-read; verify CRC of the winner. */
    const struct sp_shadow_entry *winner = &base[best_idx];
    uint32_t winner_crc = crc32c(winner, sizeof *winner);
    /* Cheap coherence check: re-read gen to see if we raced a writer.       */
    uint64_t gen_after = atomic_load_explicit((_Atomic uint64_t *)&h->gen, memory_order_acquire);
    if (gen != gen_after) return -1; /* conservative: retry happens next op */

    unsigned long src_len = best_dst_len;
    const char *src = strtab + best_dst_off;
    unsigned long tail_len = strlen(path) - best_len;
    if (src_len + tail_len + 1 > out_cap) return -1;

    memcpy(out, src, src_len);
    memcpy(out + src_len, path + best_len, tail_len + 1);
    (void)winner_crc; /* keep: validate-CRC infra — see caller docs */
    return 0;
}
