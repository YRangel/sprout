/* sprout-shadow — ADR-0024 layer-0 shared table.
 *
 * Single writer (session owner, supervisor) lives in the CLI; every fast-lane
 * process maps this memfd read-only via SPROUT_SHADOW_FD and snapshots it
 * lock-free.  Fail-open rule: magic/CRC/heartbeat/seqlock mismatch anywhere
 * means "treat as empty" — never a partial lie, never a crash of the child.
 */
#ifndef SPROUT_SHADOW_H
#define SPROUT_SHADOW_H

#include <stdint.h>

#define SP_SHADOW_MAGIC   0x5350525453484457ULL /* "SPRSHDW" */
#define SP_SHADOW_VERSION 4u

/* Heartbeat: writer bumps heartbeat_ns at least every 250ms; a table older
 * than STALE_NS is treated as dead (fail-open).                    */
#define SP_SHADOW_STALE_NS 2000000000ULL /* 2s */

/* ------------------------- WRITER-side API (owner) -------------------------
 * Owner flow: create memfd -> sp_shadow_owner_init -> fill entries (any
 * order) -> sp_shadow_owner_gen_bump to flip seqlock + heartbeat.
 * Fast lane observes gen flip + Valid entries in ≤250ms window.
 */
typedef struct sp_shadow_owner {
    int      fd;
    void    *map;
    uint64_t map_size;
    uint64_t cap;
    uint8_t  dirty;       /* entries changed since last bump */
} sp_shadow_owner_t;

int  sp_shadow_owner_init(sp_shadow_owner_t *o, uint64_t cap_entries);
void sp_shadow_owner_close(sp_shadow_owner_t *o);

/* Writers: >0 idx on success, -1 on no-space / bad input. */
long sp_shadow_owner_add_bind(sp_shadow_owner_t *o,
                              const char *dst_path, const char *src_path);
void sp_shadow_owner_mark_state(sp_shadow_owner_t *o, uint32_t idx, uint8_t state);
/* Publish a batch (gen increments): stamps heartbeat too. */
void sp_shadow_owner_commit(sp_shadow_owner_t *o);
/* Bump just the heartbeat (cheap; call from the owner loop). */
void sp_shadow_owner_pulse(sp_shadow_owner_t *o);

/* Entry types */
#define SP_SH_T_BIND     1u /* bind mount: src -> dst path rewrite       */
#define SP_SH_T_PROC     2u /* proc override entry                       */
#define SP_SH_T_NSFAKE   3u /* faked namespace marker                    */
#define SP_SH_T_DEVNODE  4u /* fake device node                          */

/* Entry states */
#define SP_SH_S_VALID    1u
#define SP_SH_S_INFLIGHT 2u /* PID journal pending; treat as valid+warn  */
#define SP_SH_S_FAILED   3u /* replay-failed: invisible + counts         */
#define SP_SH_S_REMOVED  4u /* tombstone: invisible                      */

/* Fixed layout — shared across CLI (Rust), interposer (C), agent. */
struct sp_shadow_hdr {
    uint64_t magic;            /* offset 0  */
    uint64_t gen;              /* seqlock: odd = write in flight         */
    uint64_t heartbeat_ns;     /* CLOCK_MONOTONIC at last writer touch   */
    uint32_t crc32;            /* CRC32 of gen-stable region             */
    uint32_t count;            /* entries actually used                  */
    uint64_t cap;              /* allocated entry count                  */
    uint64_t strtab_off;       /* offset of paths blob (past entries)    */
    uint64_t strtab_len;       /* live bytes in strtab                   */
}; /* 56 bytes (meticulous: 3×u64 + 2×u32 + 3×u64); the C comment once lied */

#define SP_SHADOW_HDR_SIZE ((uint64_t)sizeof(struct sp_shadow_hdr))

struct sp_shadow_entry {
    uint8_t  type;      /* SP_SH_T_*  */
    uint8_t  state;     /* SP_SH_S_*  */
    uint16_t flags;
    uint32_t src_off;   /* offset into strtab of src path  (NUL-terminated) */
    uint32_t src_len;
    uint32_t dst_off;   /* offset into strtab of dst path  (NUL-terminated) */
    uint32_t dst_len;
    uint32_t aux;       /* type-specific: proc id, dev major<<8|minor... */
    uint32_t pad;
}; /* 28 bytes: 2×u8 + u16 + 6×u32 — sizeof() is the only truth */

#define SP_SHADOW_ENTRY_SIZE ((uint64_t)sizeof(struct sp_shadow_entry))

/* --- Reader API (interposer side).  Zero syscalls on hit path: a validated
 * snapshot is pure userspace memory. ------------------------------------ */

typedef struct sp_shadow_snap {
    const struct sp_shadow_hdr *hdr;  /* points *into* the mmap */
    uint64_t    size;                 /* map size validated     */
    uint64_t    gen;                  /* snapshot generation    */
    const struct sp_shadow_entry *entries;
    const char *strtab;
    uint64_t    strtab_len;
    int         valid;                /* 1 = contents trustworthy */
} sp_shadow_snap_t;

/* Attach: mmap the fd read-only, validate header + CRC + heartbeat.
 * Returns 0 on success, fills snap. Never writes. Safe to call repeatedly. */
int  sp_shadow_attach(int fd, sp_shadow_snap_t *snap);
void sp_shadow_detach(sp_shadow_snap_t *snap);

/* Fast validation of current generation (for "did the table change?"): if
 * zero cost gates hold (magic + heartbeat + seqlock even), just the header
 * is checked — count == 0 → "empty".  Returns 1 if shadow considered live.  */
int  sp_shadow_live(const sp_shadow_snap_t *snap);

/* Bind-mount lookup: longest-prefix match over dst entries; returns pointer
 * to rewritten string in `buf` (caller-owned, cap PATH_MAX) or NULL.
 * REWRITE RULE: only when entry type == BIND_MOUNT and state == VALID and
 * path starts with dst as directory prefix.  No alias resolution, no
 * symlinks - just raw path prefix math.                              */
int  sp_shadow_lookup_bind(const sp_shadow_snap_t *snap,
                           const char *path,
                           char *out, unsigned long out_cap);

#endif /* SPROUT_SHADOW_H */
