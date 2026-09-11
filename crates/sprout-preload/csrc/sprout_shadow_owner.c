/* sprout-shadow — writer-side implementation.
 * The session owner (CLI holder) is the ONLY process that calls these.
 * Publish contract: mutate entries with gen ODD, then call commit() to make
 * the generation EVEN — readers snap gen and validate immediately.       */
#include "sprout_shadow.h"
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/syscall.h>

static uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int sp_shadow_owner_init(sp_shadow_owner_t *o, uint64_t cap_entries) {
    memset(o, 0, sizeof *o);
    if (!cap_entries || cap_entries > 4096) return -1;
    uint64_t mapsz = SP_SHADOW_HDR_SIZE + SP_SHADOW_ENTRY_SIZE * cap_entries;
    mapsz = (mapsz + 4095) & ~4095ULL; /* round page */
    if (mapsz < (1u << 16)) mapsz = 1u << 16; /* readers map a fixed 64 KiB */
    int fd = (int)syscall(SYS_memfd_create, "sprout-shadow", 0 /* MFD_NO_SEAL so we can rewrite */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)mapsz) < 0) { close(fd); return -1; }
    void *m = mmap(NULL, mapsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    struct sp_shadow_hdr *h = (struct sp_shadow_hdr *)m;
    h->magic = SP_SHADOW_MAGIC;
    h->gen = 1; /* odd = uninitialized; commit() flips to even */
    h->cap = cap_entries;
    h->strtab_off = SP_SHADOW_HDR_SIZE + SP_SHADOW_ENTRY_SIZE * cap_entries;
    h->strtab_len = 0;
    h->count = 0;
    o->fd = fd;
    o->map = m;
    o->map_size = mapsz;
    o->cap = cap_entries;
    return 0;
}

void sp_shadow_owner_close(sp_shadow_owner_t *o) {
    if (o->map) munmap(o->map, o->map_size);
    if (o->fd >= 0) close(o->fd);
    memset(o, 0, sizeof *o);
    o->fd = -1;
}

long sp_shadow_owner_add_bind(sp_shadow_owner_t *o,
                              const char *dst_path, const char *src_path) {
    if (!o->map) return -1;
    struct sp_shadow_hdr *h = (struct sp_shadow_hdr *)o->map;
    if (h->count >= o->cap) return -1;
    uint32_t idx = h->count;
    char *strtab = (char *)o->map + h->strtab_off;
    uint32_t off = (uint32_t)h->strtab_len;
    uint32_t rem = (uint32_t)(o->map_size - h->strtab_off - h->strtab_len);
    uint32_t need = (uint32_t)(strlen(dst_path) + strlen(src_path) + 2);
    if (need > rem) return -1;
    struct sp_shadow_entry *e = (struct sp_shadow_entry *)
        ((char *)o->map + SP_SHADOW_HDR_SIZE + SP_SHADOW_ENTRY_SIZE * idx);
    memset(e, 0, sizeof *e);
    e->type = SP_SH_T_BIND;
    e->state = SP_SH_S_VALID;
    e->dst_off = off;
    e->dst_len = (uint32_t)strlen(dst_path);
    memcpy(strtab + e->dst_off, dst_path, e->dst_len + 1);
    e->src_off = e->dst_off + e->dst_len + 1;
    e->src_len = (uint32_t)strlen(src_path);
    memcpy(strtab + e->src_off, src_path, e->src_len + 1);
    h->strtab_len = e->src_off + e->src_len + 1;
    h->count = idx + 1;
    o->dirty = 1;
    return idx;
}

void sp_shadow_owner_mark_state(sp_shadow_owner_t *o, uint32_t idx, uint8_t state) {
    if (!o->map || idx >= o->cap) return;
    struct sp_shadow_entry *e = (struct sp_shadow_entry *)
        ((char *)o->map + SP_SHADOW_HDR_SIZE + SP_SHADOW_ENTRY_SIZE * idx);
    e->state = state;
    o->dirty = 1;
}

void sp_shadow_owner_commit(sp_shadow_owner_t *o) {
    if (!o->map) return;
    struct sp_shadow_hdr *h = (struct sp_shadow_hdr *)o->map;
    atomic_store_explicit((_Atomic uint64_t *)&h->heartbeat_ns,
                          now_mono_ns(), memory_order_release);
    uint64_t g = atomic_load_explicit((_Atomic uint64_t *)&h->gen, memory_order_relaxed);
    atomic_store_explicit((_Atomic uint64_t *)&h->gen,
                          (g & ~1ULL) + 2, memory_order_release);
    o->dirty = 0;
}

void sp_shadow_owner_pulse(sp_shadow_owner_t *o) {
    if (!o->map) return;
    struct sp_shadow_hdr *h = (struct sp_shadow_hdr *)o->map;
    atomic_store_explicit((_Atomic uint64_t *)&h->heartbeat_ns,
                          now_mono_ns(), memory_order_release);
}
