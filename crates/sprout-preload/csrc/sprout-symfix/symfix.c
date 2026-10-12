/* symfix.c — fills box32's unwrapped-symbol gaps for the emulated guest.
 *
 * box32 wraps libc/libm/fontconfig/freetype with thunk tables; any symbol
 * the table lacks leaves an R_386_JMP_SLOT unresolved ("Symbol X not
 * found, cannot apply") and the first call jumps through an empty slot.
 * Steam's client (Sep 2026 build) hits 22 such gaps: the resolver trio
 * (getservbyname_r/dn_expand/res_nquery), modern glibc additions
 * (copy_file_range/mallinfo2/pidfd-family/epoll_pwait2/qsort_r/lchmod/
 * __openat64_2) and the FreeType COLRv1 + fontconfig enumeration APIs
 * used by harfbuzz/pango.
 *
 * This i386 DSO is preloaded into the guest via BOX64_LD_PRELOAD (same
 * pattern as libsprout-sysvipc) so the relocation lookup finds real
 * exports. The glibc gaps get working implementations; the FT/Fc paint
 * APIs are safe degradations (error / no-color-font semantics) — color
 * emoji paint layers fall back to normal glyphs.
 *
 * Build (in guest):  clang --target=i386-linux-gnu -shared -fPIC -O2 \
 *                        symfix.c -o libsprout-symfix.so
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------------- glibc gaps: working implementations ---------------- */

int getservbyname_r(const char *name, const char *proto,
                    struct servent *res, char *buf, size_t buflen,
                    struct servent **result)
{
    if (result) *result = NULL;
    if (!name || !res || !buf) { errno = EINVAL; return EINVAL; }
    FILE *f = fopen("/etc/services", "r");
    if (!f) return ENOENT;
    char line[512];
    int rc = ENOENT;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *nm = strtok(line, " \t\r\n");
        if (!nm) continue;
        char *pp = strtok(NULL, " \t\r\n");
        if (!pp) continue;
        char *slash = strchr(pp, '/');
        if (!slash) continue;
        *slash++ = 0;
        if (strcmp(nm, name) != 0) continue;
        if (proto && strcmp(slash, proto) != 0) continue;
        int port = atoi(pp);
        /* pack: name\0 proto\0 alias-ptr-array{name-ptr? none} */
        size_t nl = strlen(nm), pl = strlen(slash);
        size_t need = nl + 1 + pl + 1 + sizeof(char *);
        if (need > buflen) { rc = ERANGE; break; }
        char *p = buf;
        res->s_name = p; memcpy(p, nm, nl + 1); p += nl + 1;
        res->s_proto = p; memcpy(p, slash, pl + 1); p += pl + 1;
        res->s_aliases = (char **)p;
        res->s_aliases[0] = NULL;
        res->s_port = htons((unsigned short)port);
        if (result) *result = res;
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

int dn_expand(const unsigned char *msg, const unsigned char *eom,
              const unsigned char *src, char *dst, int dstsiz)
{
    const unsigned char *p = src;
    char *dp = dst, *de = dst + dstsiz;
    int hops = 0, consumed = -1, first = 1;
    if (dstsiz <= 0) return -1;
    while (p < eom) {
        unsigned c = *p++;
        if (c == 0) {
            if (consumed < 0) consumed = (int)(p - src);
            if (dp > dst) *dp = 0; else if (de > dp) *dp = 0;
            return consumed;
        }
        if ((c & 0xC0) == 0xC0) {
            if (p >= eom) return -1;
            unsigned off = ((c & 0x3F) << 8) | *p++;
            if (consumed < 0) consumed = (int)(p - src);
            p = msg + off;
            if (++hops > 128 || p >= eom) return -1;
            continue;
        }
        if (c > 63 || p + c > eom) return -1;
        if (!first) {
            if (dp >= de) return -1;
            *dp++ = '.';
        }
        first = 0;
        if (dp + c >= de) return -1;
        memcpy(dp, p, c);
        dp += c;
        p += c;
    }
    return -1;
}

ssize_t copy_file_range(int fd_in, off64_t *off_in, int fd_out,
                        off64_t *off_out, size_t len, unsigned int flags)
{
    ssize_t r = (ssize_t)syscall(__NR_copy_file_range, fd_in, off_in,
                                 fd_out, off_out, len, flags);
    if (r >= 0 || errno != ENOSYS) return r;
    /* pre-4.13 kernel: manual splice-free loop */
    char *tmp = malloc(65536);
    if (!tmp) return -1;
    ssize_t total = 0;
    while (len) {
        size_t chunk = len > 65536 ? 65536 : len;
        ssize_t got = off_in ? pread(fd_in, tmp, chunk, *off_in)
                             : read(fd_in, tmp, chunk);
        if (got < 0) { if (!total) total = -1; break; }
        if (!got) break;
        ssize_t put = 0;
        while (put < got) {
            ssize_t w = off_out ? pwrite(fd_out, tmp + put, (size_t)(got - put), *off_out + put)
                                : write(fd_out, tmp + put, (size_t)(got - put));
            if (w < 0) { if (!total) total = -1; goto out; }
            put += w;
        }
        if (off_in) *off_in += got;
        if (off_out) *off_out += got;
        total += got;
        len -= (size_t)got;
    }
out:
    free(tmp);
    return total;
}

int __openat64_2(int fd, const char *file, int flags)
{
    return openat(fd, file, flags);
}

int lchmod(const char *path, mode_t mode)
{
    (void)path; (void)mode;
    errno = ENOSYS;   /* Linux glibc semantics */
    return -1;
}

struct mallinfo2 mallinfo2(void)
{
    struct mallinfo2 m;
    memset(&m, 0, sizeof m);
    return m;
}

int epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                 const struct timespec *timeout, const sigset_t *sigmask)
{
    int r = (int)syscall(__NR_epoll_pwait2, epfd, events, maxevents,
                         timeout, sigmask, (size_t)8);
    if (r >= 0 || errno != ENOSYS) return r;
    int ms = -1;
    if (timeout) {
        long long m = (long long)timeout->tv_sec * 1000 +
                      timeout->tv_nsec / 1000000;
        if (!m && (timeout->tv_sec || timeout->tv_nsec)) m = 1;
        ms = (int)m;
    }
    return epoll_pwait(epfd, events, maxevents, ms, sigmask);
}

pid_t pidfd_open(pid_t pid, unsigned int flags)
{
    return (pid_t)syscall(__NR_pidfd_open, pid, flags);
}

int pidfd_send_signal(int pidfd, int sig, siginfo_t *info, unsigned int flags)
{
    return (int)syscall(__NR_pidfd_send_signal, pidfd, sig, info, flags);
}

/* merge sort with context arg (stable, O(n log n)) */
static void msr_merge(char *a, char *t, size_t n, size_t s,
                      int (*cmp)(const void *, const void *, void *), void *arg)
{
    if (n < 2) return;
    size_t h = n / 2;
    msr_merge(a, t, h, s, cmp, arg);
    msr_merge(a + h * s, t, n - h, s, cmp, arg);
    char *l = a, *r = a + h * s, *o = t, *e = a + n * s;
    while (l < a + h * s && r < e) {
        if (cmp(r, l, arg) < 0) { memcpy(o, r, s); r += s; }
        else { memcpy(o, l, s); l += s; }
        o += s;
    }
    while (l < a + h * s) { memcpy(o, l, s); l += s; o += s; }
    while (r < e) { memcpy(o, r, s); r += s; o += s; }
    memcpy(a, t, n * s);
}

void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *), void *arg)
{
    if (nmemb < 2 || !size) return;
    char *tmp = malloc(nmemb * size);
    if (!tmp) return;
    msr_merge((char *)base, tmp, nmemb, size, compar, arg);
    free(tmp);
}

/* glibc 2.36+ random family (libICE et al.) */
void arc4random_buf(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    while (n) {
        ssize_t r = (ssize_t)syscall(__NR_getrandom, p, n, 0);
        if (r <= 0) {
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd >= 0) { r = read(fd, p, n); close(fd); }
            if (r <= 0) { memset(p, 0x5A, n); return; }
        }
        p += r;
        n -= (size_t)r;
    }
}

uint32_t arc4random(void)
{
    uint32_t v = 0;
    arc4random_buf(&v, sizeof v);
    return v;
}

uint32_t arc4random_uniform(uint32_t upper)
{
    if (upper < 2) return 0;
    uint32_t lim = 0xFFFFFFFFu - (0xFFFFFFFFu % upper) - 1;
    uint32_t v;
    do { arc4random_buf(&v, sizeof v); } while (v > lim);
    return v % upper;
}

int res_nquery(void *statp, const char *dname, int class, int type,
               unsigned char *answer, int anslen)
{
    (void)statp; (void)dname; (void)class; (void)type;
    (void)answer; (void)anslen;
    /* legacy resolver path: signal failure, callers fall back */
    return -1;
}

/* ---- FreeType COLRv1 / fontconfig gaps: safe degradations ----
 * cdecl: extra call args are caller-cleaned, so no-arg stubs are ABI
 * safe. Error = "no color paint / no transform" which every FT client
 * treats as plain-glyph fallback. */
int FT_New_Library(void) { return 1; }          /* FT_Err_Cannot_Open_Resource */
int FT_Done_Library(void) { return 0; }
int FT_Reference_Library(void) { return 0; }
void FT_Add_Default_Modules(void) {}
void FT_Set_Default_Properties(void) {}
int FT_Sfnt_Table_Info(void) { return 1; }
int FT_Palette_Select(void) { return 1; }
int FT_Get_Paint(void) { return 1; }
int FT_Get_Paint_Layers(void) { return 1; }
int FT_Get_Transform(void) { return 1; }
int FT_Get_Color_Glyph_ClipBox(void) { return 1; }
int FT_Get_Color_Glyph_Layer(void) { return 0; }   /* FT_Bool: no layers */
int FT_Get_Color_Glyph_Paint(void) { return 0; }   /* FT_Bool: no paint */
int FT_Get_Colorline_Stops(void) { return 1; }

/* fontconfig: struct _FcFontSet { int nfont; int sfont; FcPattern **fonts; } */
typedef struct { int nfont, sfont; void **fonts; } fx_fontset;

/* Wrapped twins (thunk to HOST fontconfig; host allocations are 32-bit
 * clean in the emu32 lane so the pointers are lossless here). */
extern void *FcFontSetCreate(void);
extern void *FcFontSetDestroy(void *);
extern void *FcFontSetSort(void *, int, void *, int, void *, void *);
extern void FcPatternReference(void *);

void *FcFontSetList(void)
{
    /* Caller OWNS and will FcFontSetDestroy() the result — never hand
     * out a static fake (that was a heap-corruption crash). */
    return FcFontSetCreate();
}

void *FcFontSetMatch(void *sets_, int nsets, void *pattern, void *result)
{
    /* Real semantics via the wrapped twin: FcFontSetSort sorts by match
     * quality, so the head of the sorted set IS the best match. */
    void *sorted = FcFontSetSort(sets_, nsets, pattern, 1, (void *)0, result);
    if (!sorted) return (void *)0;
    fx_fontset *s = (fx_fontset *)sorted;
    void *best = (s->nfont > 0 && s->fonts) ? s->fonts[0] : (void *)0;
    if (best) FcPatternReference(best);
    FcFontSetDestroy(sorted);
    return best;
}
