/* sprout-stub: freestanding (libc-less) static ELF trampoline.
 *
 * Launch contract (ADR-0016):
 *   argv = [guest_argv0_spelling, target_host_path, arg1, arg2, ...]
 *   env  = inherited; SPROUT_STUB_SOCK=<fd> for the listener handshake
 *
 * Job:
 *   0) (T2) install SA_SIGINFO SIGSYS emulation handler (Android-blocked
 *      syscall fakes from in-guest address space — ptrace-free),
 *   1) (T2) install the notify seccomp filter, handshake the listener fd
 *      to the supervisor (pidfd_getfd on its end) and block for its ACK,
 *   2) mmap the static guest binary (ET_EXEC) into this address space,
 *   3) build a fresh kernel-conventional stack (argc/argv/env/auxv),
 *   4) jump to the guest entry. From here on the stub is *the guest's
 *      address space*: its SIGSYS handler + seccomp filter ride along.
 *
 * No libc; every syscall is a raw svc. Linked at a high image base so a
 * sanely-linked guest (0x400000 region) never collides with the stub.
 *
 * Copyright (c) sprout contributors; MIT OR Apache-2.0.
 */

/* ------------------------------------------------------------------ */
/* syscall plumbing (aarch64)                                          */
/* ------------------------------------------------------------------ */

typedef unsigned long u64;
typedef unsigned int u32;
typedef int i32;
typedef long i64;
typedef unsigned short u16;
typedef unsigned char u8;

_Static_assert(sizeof(void *) == 8, "aarch64 only");

static inline long sc0(long n)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}
static inline long sc1(long n, long a0)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}
static inline long sc2(long n, long a0, long a1)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}
static inline long sc3(long n, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static inline long sc4(long n, long a0, long a1, long a2, long a3)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory", "cc");
    return x0;
}
static inline long sc6(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory", "cc");
    return x0;
}

/* ------------------------------------------------------------------ */
/* string/memory helpers (no libc)                                     */
/* ------------------------------------------------------------------ */

static unsigned long st_len(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}
static void st_copy(char *d, const char *s, unsigned long n)
{
    while (n--) *d++ = *s++;
}
static void st_zero(void *d, unsigned long n)
{
    char *p = (char *)d;
    while (n--) *p++ = 0;
}
/* clang lowers some loops to libc calls even with -fno-builtin; supply
 * exactly the symbols it may reference. */
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}
unsigned long strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

static int st_eqn(const char *a, const char *b, unsigned long n)
{
    while (n--) {
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++; b++;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

enum {
    SYS_read = 63, SYS_write = 64, SYS_openat = 56, SYS_close = 57,
    SYS_exit_group = 94, SYS_rt_sigaction = 134, SYS_rt_sigprocmask = 135,
    SYS_ioctl = 29, SYS_lseek = 62, SYS_mmap = 222, SYS_mprotect = 226,
    SYS_prctl = 167, SYS_seccomp = 277, SYS_munmap = 215, SYS_fstat = 80,
};

enum { O_RDONLY_ = 0 };

enum {
    PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4,
    MAP_SHARED = 1, MAP_PRIVATE = 2, MAP_FIXED = 0x10,
    MAP_ANONYMOUS = 0x20, MAP_FIXED_NOREPLACE = 0x100000,
    MAP_NORESERVE = 0x4000,
};

enum { PT_LOAD = 1 };

enum {
    AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6,
    AT_BASE = 7, AT_ENTRY = 9,
};

enum {
    SIGSYS_ = 31, SA_SIGINFO = 4, SA_ONSTACK = 0x08000000,
    SA_RESTORER = 0x04000000, SA_RESTART = 0x10000000,
};

enum { PR_SET_NO_NEW_PRIVS = 38 };

/* BPF for the notify filter */
typedef struct { u16 code; u8 jt, jf; u32 k; } sp_bpf_insn_t;
typedef struct { u16 len; u64 filter; } sp_bpf_prog_t;

#define AUDIT_ARCH_AARCH64_OWN 0xC00000B7u
#define SECCOMP_RET_KILL 0x00000000u
#define SECCOMP_RET_ALLOW 0x7fff0000u
#define SECCOMP_RET_USER_NOTIF 0x7fc00000u
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#define SECCOMP_SET_MODE_FILTER 1

#define PAGE 4096UL
#define SP_PATH_MAX 4096

/* ------------------------------------------------------------------ */
/* diagnostics + exit                                                  */
/* ------------------------------------------------------------------ */

static void die(const char *msg, long code)
{
    sc3(SYS_write, 2, (long)"sprout-stub: ", 13);
    sc3(SYS_write, 2, (long)msg, (long)st_len(msg));
    sc3(SYS_write, 2, (long)"\n", 1);
    sc1(SYS_exit_group, code > 0 ? code : 127);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* ELF structures                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum;
    u16 e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr_t;

/* kernel signal frame for rt_sigaction on aarch64 */
typedef struct {
    unsigned long __opaque[2]; /* not used by us except for emulations */
} sp_sigset_min_t;

typedef struct {
    void (*handler)(int, void *, void *);
    unsigned long flags;
    void (*restorer)(void);
    u64 mask; /* one word is plenty (we only care about realtime-safe) */
} sp_kernel_sigaction_t;

/* ------------------------------------------------------------------ */
/* load the guest ET_EXEC image                                        */
/* ------------------------------------------------------------------ */

static long g_target_fd = -1;
static elf64_ehdr_t g_eh;
static elf64_phdr_t g_ph[32];

static unsigned long load_guest(const char *path, unsigned long *phdr_out)
{
    g_target_fd = sc4(SYS_openat, -100, (long)path, O_RDONLY_, 0);
    if (g_target_fd < 0) die("open target failed", 126);
    if (sc3(SYS_read, g_target_fd, (long)&g_eh, sizeof(g_eh)) != (long)sizeof(g_eh))
        die("read ehdr", 126);
    if (g_eh.ident[0] != 0x7f || g_eh.ident[1] != 'E' || g_eh.ident[2] != 'L' ||
        g_eh.ident[3] != 'F' || g_eh.ident[4] != 2 ||
        (g_eh.e_type != 2 /* ET_EXEC */ && g_eh.e_type != 3 /* ET_DYN(static-pie) */))
        die("target is not a static ET_EXEC/PIE ELF", 126);
    if (g_eh.e_phnum > 32 || g_eh.e_phentsize != 56)
        die("phdr table too big/weird", 126);
    if (sc3(SYS_lseek, g_target_fd, (long)g_eh.e_phoff, 0) < 0)
        die("seek phdrs", 126);
    unsigned long phbytes = (unsigned long)g_eh.e_phnum * 56;
    if (sc3(SYS_read, g_target_fd, (long)g_ph, (long)phbytes) != (long)phbytes)
        die("read phdrs", 126);

    /* static-PIE targets (ET_DYN, no PT_INTERP) get a fixed low base:
     * 0x40000000 keeps them clear of the stub (0x70000000) and of any
     * ET_EXEC guest (kernel-default 0x400000 region is likely free in our
     * custom image; MAP_FIXED_NOREPLACE gives us collision honesty). */
    unsigned long base = (g_eh.e_type == 3) ? 0x40000000UL : 0;

    for (unsigned i = 0; i < g_eh.e_phnum; i++) {
        elf64_phdr_t *p = &g_ph[i];
        if (p->p_type != PT_LOAD) continue;
        unsigned long abs_v = base + p->p_vaddr;
        unsigned long vpage = abs_v & ~(PAGE - 1);
        unsigned long fstart = p->p_offset & ~(PAGE - 1);
        unsigned long endf = (p->p_offset + p->p_filesz + PAGE - 1) & ~(PAGE - 1);
        unsigned long endm = (abs_v + p->p_memsz + PAGE - 1) & ~(PAGE - 1);
        unsigned long flen = endf - fstart;
        unsigned long mlen = endm - vpage;
        if (flen > mlen) flen = mlen;
        int prot = 0;
        if (p->p_flags & 4) prot |= PROT_READ;
        if (p->p_flags & 2) prot |= PROT_WRITE;
        if (p->p_flags & 1) prot |= PROT_EXEC;
        long m = (flen > 0)
            ? sc6(SYS_mmap, (long)vpage, (long)flen, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_FIXED_NOREPLACE, g_target_fd, (long)fstart)
            : 0;
        if (flen > 0 && (m < 0 || m == -1)) die("mmap PT_LOAD collide/perm", 125);
        if (mlen > flen) {
            long m2 = sc6(SYS_mmap, (long)(vpage + flen), (long)(mlen - flen),
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m2 < 0 || m2 == -1) die("mmap bss tail", 125);
        }
        unsigned long init_end = abs_v + p->p_filesz;
        unsigned long gap_to = ((init_end + PAGE - 1) & ~(PAGE - 1));
        if (gap_to > init_end && init_end < abs_v + p->p_memsz) {
            if (gap_to > abs_v + p->p_memsz) gap_to = abs_v + p->p_memsz;
            st_zero((void *)init_end, gap_to - init_end);
        }
        if (prot != (PROT_READ | PROT_WRITE)) {
            if (sc3(SYS_mprotect, (long)vpage, (long)mlen, prot) < 0)
                die("mprotect segment", 125);
        }
    }
    sc1(SYS_close, g_target_fd);

    /* phdr address inside the map: the PT_LOAD covering the phdr file
     * range tells us the virtual bias for the phdr table. */
    unsigned long phdr_addr = 0;
    for (unsigned i = 0; i < g_eh.e_phnum; i++) {
        elf64_phdr_t *p = &g_ph[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_offset <= g_eh.e_phoff && g_eh.e_phoff < p->p_offset + p->p_filesz) {
            phdr_addr = base + p->p_vaddr + (g_eh.e_phoff - p->p_offset);
            break;
        }
    }
    *phdr_out = phdr_addr;
    return base + g_eh.e_entry;
}

/* ------------------------------------------------------------------ */
/* SIGSYS emulation (ADR-0016 in-guest table)                          */
/* ------------------------------------------------------------------ */

/* kernel rt_sigframe uapi offsets (aarch64) — KERNEL layout, NOT glibc's
 * userland ucontext_t (whose sigset_t is 128 bytes):
 *   siginfo(128) then ucontext{ uc_flags(8) uc_link(8) uc_stack(24)
 *   uc_sigmask(8) } => sigcontext @ 176.
 *   sigcontext: fault_address@0, regs[31](x0..x30)@8, sp@8+31*8=256,
 *   pc@264, pstate@272.  siginfo._sigsys: call_addr@16, syscall@24, arch@28.
 * Empirically verified 2026-08 on Android 16 (HyperOS GKI) with a raw
 * dump of svc-99's frame: x8=99 at +248, x1=0x18 at +192,
 * pc=0x41351c(text) at +440, sp at +432 — all kernel-ABI-stable since
 * arm64's introduction (rt_sigreturn consumers pin this layout).
 *
 * ROOT CAUSE of issue #74 (glibc-static SIGBUS under the stub lane):
 * the previous constants assumed glibc's 128-byte sigset (ucontext @ 168,
 * 8 bytes early). Fake-success then wrote x0=0 into the fault_address
 * field (harmless) and — the killer — did SP+=4 instead of PC+=4
 * (offset 432 is SP at the correct layout). rt_sigreturn reinstated a
 * 4-byte-misaligned sp; the next stp/ldp pairs byte-shifted register
 * contents until the thread fetched instructions from inside its own
 * RW-anon stack (SIGBUS, fa==pc, high anon address).
 */
#define UC_X0_OFF 184
#define UC_X3_OFF 208
#define UC_X8_OFF 248
#define UC_PC_OFF 440
#define SI_SYSCALL_OFF 24

/* glibc table: set_robust_list, rseq */
static const long g_glibc_ok[] = { 99, 293 };
/* musl table: faccessat, set*id family, setgroups */
static const long g_musl_ok[] = { 48, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 159 };
/* shared -ENOSYS table (fake success would be a LIE here: callers probe
 * and fall back — io_uring probes; futex_waitv on Android-16 TRAPs and
 * glibc≥2.41/libevent fall back to futex(2) on ENOSYS). */
static const long g_enosys[] = { 202, 425, 426, 427 };

static int stub_is_ok(long nr, const long *tbl, int n)
{
    for (int i = 0; i < n; i++) if (tbl[i] == nr) return 1;
    return 0;
}

static const char *g_stub_env_musl = "SPROUT_LIBC=musl";
static int g_stub_flavor_musl = 0;

static int stub_crashdump(void);
static int stub_hex(char *db, int n, unsigned long long v);

static void stub_sigsys_handler(int sig __attribute__((unused)),
                                void *vinfo, void *vuctx)
{
    u32 *info = (u32 *)vinfo;
    u64 *uc = (u64 *)vuctx;
    int musl = g_stub_flavor_musl;
    long nr = (long)(i32)info[SI_SYSCALL_OFF / 4];
    int ok = musl ? stub_is_ok(nr, g_musl_ok,
                               (int)(sizeof(g_musl_ok) / sizeof(g_musl_ok[0])))
                  : stub_is_ok(nr, g_glibc_ok,
                               (int)(sizeof(g_glibc_ok) / sizeof(g_glibc_ok[0])));
    if (!ok && nr == 202 /*accept*/) {
        /* ANDROID TRAPs legacy accept(2), accepts accept4: rewrite the
         * interrupted frame (x8=242, x3=flags=0) and DON'T advance pc —
         * sigreturn re-executes the same svc, now as accept4(fd,addr,len,0). */
        uc[UC_X8_OFF / 8] = 242;
        uc[UC_X3_OFF / 8] = 0;
        return;
    }
    if (!ok) {
        if (stub_is_ok(nr, g_enosys, (int)(sizeof(g_enosys) / sizeof(g_enosys[0])))) {
            uc[UC_X0_OFF / 8] = (u64)-38; /* -ENOSYS */
            uc[UC_PC_OFF / 8] += 4;
            return;
        }
        char db[48];
        db[0]='s'; db[1]='g'; db[2]='s'; db[3]='y'; db[4]='s'; db[5]='-'; db[6]='n'; db[7]='r'; db[8]='='; int n=9;
        long m2 = nr;
        if (m2 < 0) { db[n++]='-'; m2=-m2; }
        char rdc[12]; int m=0; long t=m2;
        do { rdc[m++]=(char)('0'+t%10); t/=10; } while (t);
        while (m>0) db[n++]=rdc[--m];
        db[n++]='\n';
        (void)sc3(SYS_write, 2, (long)db, n);
        return; /* let default action kill: honest */
    }
    if (stub_crashdump()) {
        static volatile long cd_n;
        if (cd_n == 0) {
            /* raw ucontext window: locate x8/pc/sp empirically (#74) */
            char rb[4096]; int rn = 0;
            for (int off = 136; off <= 448; off += 4) {
                if (((off - 136) % 8) == 0) { rb[rn++]='\n'; rb[rn++]='@'; rn = stub_hex(rb, rn, (unsigned long long)off); rb[rn++]=':'; }
                rb[rn++]=' ';
                rn = stub_hex(rb, rn, *(unsigned int *)((char *)vuctx + off));
            }
            rb[rn++]='\n';
            (void)sc3(SYS_write, 2, (long)rb, rn);
        }
        if (cd_n < 4) {
            cd_n++;
            char eb[96]; int en = 0;
            const char *t = "EMU nr="; for (const char *c = t; *c; c++) eb[en++] = *c;
            long m2 = nr; if (m2 < 0) { eb[en++]='-'; m2=-m2; }
            char rd[12]; int m = 0; long tt = m2;
            do { rd[m++] = (char)('0' + tt % 10); tt /= 10; } while (tt);
            while (m > 0) eb[en++] = rd[--m];
            const char *pc = " pc="; for (const char *c = pc; *c; c++) eb[en++] = *c;
            en = stub_hex(eb, en, uc[UC_PC_OFF / 8]);
            const char *x8 = " x8="; for (const char *c = x8; *c; c++) eb[en++] = *c;
            en = stub_hex(eb, en, uc[UC_X8_OFF / 8]);
            const char *sp = " sp="; for (const char *c = sp; *c; c++) eb[en++] = *c;
            en = stub_hex(eb, en, uc[(UC_PC_OFF - 8) / 8]);
            eb[en++] = '\n';
            (void)sc3(SYS_write, 2, (long)eb, en);
        }
    }
    uc[UC_X0_OFF / 8] = 0;          /* fake success */
    uc[UC_PC_OFF / 8] += 4;         /* step over the svc */
}

extern void stub_restorer(void);

/* #74 bisect dump: glibc-statics die BEFORE first write under the stub,
 * SIGBUS(7) where the bare kernel raises SIGSYS(31). Dump the faulting
 * pc + si_addr from ucontext so the death instruction gets named.
 * Gated: only when SPROUT_STUB_CRASHDUMP=1 (guest sigsegv semantics stay
 * kernel-default otherwise). */
static int stub_crashdump(void)
{
    extern char **stub_environ;
    const char *pfx = "SPROUT_STUB_CRASHDUMP=1";
    for (char **e = stub_environ; e && *e; e++) {
        const char *s = *e; unsigned long i = 0;
        while (pfx[i] && s[i] == pfx[i]) i++;
        if (pfx[i] == '\0' && s[i] == '\0') return 1;
    }
    return 0;
}
static int stub_hex(char *db, int n, unsigned long long v)
{
    db[n++]='0'; db[n++]='x';
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) {
        int d = (int)((v >> s) & 0xf);
        if (d || started || s == 0) { db[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); started = 1; }
    }
    return n;
}
static void stub_crash_handler(int sig, void *vinfo, void *vuctx)
{
    u64 *uc = (u64 *)vuctx;
    unsigned long long fa = 0;
    if (vinfo) fa = ((unsigned long long *)vinfo)[2]; /* si_addr @ off 16 */
    char db[160]; int n = 0;
    const char *h = "CRASHDUMP sig"; for (const char *c = h; *c; c++) db[n++]=*c;
    db[n++]='='; if (sig > 9) { db[n++]=(char)('0'+sig/10); db[n++]=(char)('0'+sig%10); } else db[n++]=(char)('0'+sig); db[n++]=' ';
    db[n++]='p'; db[n++]='c'; db[n++]='='; n = stub_hex(db, n, uc[UC_PC_OFF/8]); db[n++]=' ';
    db[n++]='f'; db[n++]='a'; db[n++]='='; n = stub_hex(db, n, fa); db[n++]=' ';
    db[n++]='s'; db[n++]='p'; db[n++]='='; n = stub_hex(db, n, uc[(UC_PC_OFF-8)/8]); /* sp @ pc-8 */
    /* sigcontext regs[31]=x0..x30 @ 168+8.. then fp=x29 lr=x30, plus
     * si_code and a small window around fa (mapped R/W anon -> readable;
     * we print raw words so the frame content gets named). */
    db[n++]=' '; db[n++]='c'; db[n++]='=';
    if (vinfo) db[n++] = (char)('0' + (((u32 *)vinfo)[2] & 0xf));
    db[n++]=' '; db[n++]='f'; db[n++]='p'; db[n++]='=';
    n = stub_hex(db, n, uc[(UC_X0_OFF + 29*8)/8]);   /* x29 */
    db[n++]=' '; db[n++]='l'; db[n++]='r'; db[n++]='=';
    n = stub_hex(db, n, uc[(UC_X0_OFF + 30*8)/8]);   /* x30 */
    db[n++]='\n';
    (void)sc3(SYS_write, 2, (long)db, n);
    if (fa >= 0x1000) {
        /* dump as little-endian u32 words so aarch64 instructions decode */
        char w[256]; int wn = 0;
        for (long long off = -32; off <= 24; off += 4) {
            w[wn++]=' ';
            wn = stub_hex(w, wn, *(unsigned int *)(fa + off));
        }
        w[wn++]='\n';
        (void)sc3(SYS_write, 2, (long)w, wn);
    }
    sc1(SYS_exit_group, 128 + sig);
}

static void install_emulation(void)
{
    sp_kernel_sigaction_t sa;
    st_zero(&sa, sizeof(sa));
    sa.handler = stub_sigsys_handler;
    sa.flags = SA_SIGINFO | SA_RESTART | SA_RESTORER;
    sa.restorer = stub_restorer;
    if (sc4(SYS_rt_sigaction, SIGSYS_, (long)&sa, 0, 8) < 0)
        die("sigaction SIGSYS", 124);
}

__asm__(
    ".text\n"
    ".global stub_restorer\n"
    "stub_restorer:\n"
    "  mov x8, #139\n" /* rt_sigreturn */
    "  svc #0\n");

/* ------------------------------------------------------------------ */
/* notify filter install + supervisor handshake                        */
/* ------------------------------------------------------------------ */

/* Trap set: the supervisor's serve set (path ops, AF_UNIX ops) plus
 * execve(221) for lazy-attach chain rewrites and the stat family
 * (79/291/78) which statics have no interposer to cover. Sync with
 * crates/sprout-ptrace/csrc/sprout_ptrace.c sp_notify_install(). */
static int install_notify_filter(void)
{
    static const int traps[] = {
        56, 437, 48, 34, 35, 33, 53, 54, 88, 36, 37, 38, 276,
        79, 291, 78,
        221 /*execve: lazy-attach rewrite by the supervisor (ADR-0016 T3):
             * trapping execve HERE is safe — the supervisor child exec'd
             * the stub BEFORE any filter existed, so no guest-side exec
             * of the stub itself can deadlock on an unserved notify. The
             * filter survives the guest's exec, so the new image keeps
             * reporting to this same listener.*/,
        200, 203, 206, 211,
    };
    const int ntr = (int)(sizeof(traps) / sizeof(traps[0]));
    sp_bpf_insn_t prog[4 + 24 + 2];
    int p = 0;
    const u32 BPF_LD = 0x00, BPF_W = 0x00, BPF_ABS = 0x20;
    const u32 BPF_JMP = 0x05, BPF_JEQ = 0x10, BPF_K = 0x00, BPF_RET = 0x06;
    /* arch check */
    prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_LD | BPF_W | BPF_ABS), 0, 0, 4 };
    prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_JMP | BPF_JEQ | BPF_K), 1, 0, AUDIT_ARCH_AARCH64_OWN };
    prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_KILL };
    prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_LD | BPF_W | BPF_ABS), 0, 0, 0 };
    for (int i = 0; i < ntr; i++)
        prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_JMP | BPF_JEQ | BPF_K),
                                     (u8)(ntr + 2 - (i + 1) + 0), 0, (u32)traps[i] };
    prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_ALLOW };
    prog[p++] = (sp_bpf_insn_t){ (u16)(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_USER_NOTIF };

    /* jump-target math like the supervisor: on nr-match jump over
     * (remaining-JEQs + 1) to the USER_NOTIF ret */
    int base = 4;
    for (int i = base; i < base + ntr; i++)
        prog[i].jt = (u8)((base + ntr + 1) - (i + 1));

    /* kernel validates arg3..5==0 for NNP: x4 must be zeroed */
    if (sc6(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0) < 0)
        die("no_new_privs", 124);
    sp_bpf_prog_t fa;
    fa.len = (u16)p;
    fa.filter = (u64)(unsigned long)prog;
    long fd = sc3(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                  SECCOMP_FILTER_FLAG_NEW_LISTENER, (long)&fa);
    if (fd < 0) die("seccomp install failed", 124);
    return (int)fd;
}

/* Handshake: 4-byte fdnum to supervisor, 1-byte ACK back (mirrors
 * supervisor's sp_notify_parent_recv). */
static void stub_handshake(int listener)
{
    const char *s = 0;
    /* env scan happens via the global set in stub_main */
    extern char **stub_environ;
    for (char **e = stub_environ; e && *e; e++) {
        const char *prefix = "SPROUT_STUB_SOCK=";
        unsigned long pl = 0;
        while (prefix[pl] && (*e)[pl] == prefix[pl]) pl++;
        if (pl == st_len(prefix) && pl > 0 && prefix[pl - 1] == '=') { s = *e + pl; break; }
    }
    if (!s) die("SPROUT_STUB_SOCK missing", 123);
    long sock = 0;
    while (*s >= '0' && *s <= '9') { sock = sock * 10 + (*s - '0'); s++; }
    u32 fdnum = (u32)listener;
    if (sc3(SYS_write, sock, (long)&fdnum, 4) != 4)
        die("handshake write", 123);
    u8 ack;
    (void)sc3(SYS_read, sock, (long)&ack, 1);
    sc1(SYS_close, sock);
}

char **stub_environ = 0;

/* ------------------------------------------------------------------ */
/* stack builder                                                       */
/* ------------------------------------------------------------------ */

static u64 *build_stack(u64 *sp_top, long argc0, char **argv0, char **envp,
                        u64 *auxv, unsigned long entry,
                        unsigned long phdr_addr, unsigned long phnum)
{
    /* push strings is unnecessary: kernel text stays valid (same stack
     * region already); we re-vector pointers only. Walk auxv pairs and
     * patch the ELF-located ones to the newly mapped binary. */
    long argc = argc0 - 1;
    long envc = 0;
    for (char **e = envp; *e; e++) envc++;

    /* total words to allocate */
    unsigned long words = 1 + (unsigned long)argc + 1;
    words += (unsigned long)envc + 1;
    /* count auxv */
    unsigned long auxn = 0;
    for (u64 *a = auxv; a[0] != AT_NULL; a += 2) auxn++;
    words += (auxn + 1) * 2;

    /* argv word layout under us: argv0 stays argv[0]; argv[1]=target is
     * dropped; argv[2..] shift-left one */
    u64 *base = sp_top - words;
    base = (u64 *)((unsigned long)base & ~15UL);
    unsigned long w = 0;
    base[w++] = (u64)argc;
    base[w++] = (u64)argv0[0];
    for (long i = 2; i < argc0; i++) base[w++] = (u64)argv0[i];
    base[w++] = 0;
    for (long i = 0; i < envc; i++) base[w++] = (u64)envp[i];
    base[w++] = 0;
    for (unsigned long i = 0; i < auxn; i++) {
        u64 key = auxv[i * 2], val = auxv[i * 2 + 1];
        if (key == AT_PHDR) val = phdr_addr;
        else if (key == AT_PHENT) val = 56;
        else if (key == AT_PHNUM) val = phnum;
        else if (key == AT_ENTRY) val = entry;
        else if (key == AT_BASE) val = 0;
        base[w++] = key;
        base[w++] = val;
    }
    base[w++] = AT_NULL;
    base[w++] = 0;
    return base;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "  mov x29, #0\n"
    "  mov x30, #0\n"
    "  mov x0, sp\n"
    "  bl stub_main\n"
    "  brk #0\n" /* stub_main never returns */
);
extern void stub_main(void *sp0);

void stub_main(void *sp0)
{
    u64 *sp = (u64 *)sp0;
    long argc0 = (long)sp[0];
    char **argv = (char **)&sp[1];
    char **envp = (char **)&sp[1 + (unsigned long)argc0 + 1];
    while (*envp) envp++;
    u64 *auxv = (u64 *)(envp + 1);

    stub_environ = (char **)&sp[1 + (unsigned long)argc0 + 1];

    if (argc0 < 2) die("argv[1] must be target host path", 127);
    const char *target_path = argv[1];

    /* flavor flag (from env scan): musl implies the bigger emulate table */
    for (char **e = envp; *e; e++) {
        const char *v = *e;
        unsigned long want = st_len(g_stub_env_musl);
        unsigned long i = 0;
        for (i = 0; i < want && v[i] == g_stub_env_musl[i]; i++);
        if (i == want) {
            g_stub_flavor_musl = 1;
            break;
        }
    }

    install_emulation();

    /* #74 bisect: env-gated crash dump before any of the real work. */
    if (stub_crashdump()) {
        sp_kernel_sigaction_t sa;
        st_zero(&sa, sizeof(sa));
        sa.handler = stub_crash_handler;
        sa.flags = SA_SIGINFO | SA_RESTORER;
        sa.restorer = stub_restorer;
        long r1 = sc4(SYS_rt_sigaction, 11 /*SEGV*/, (long)&sa, 0, 8);
        long r2 = sc4(SYS_rt_sigaction, 7  /*BUS*/,  (long)&sa, 0, 8);
        long r3 = sc4(SYS_rt_sigaction, 4  /*ILL*/,  (long)&sa, 0, 8);
        const char *ok = "CRASHDUMP armed\n";
        if (!r1 && !r2 && !r3) (void)sc3(SYS_write, 2, (long)ok, 17);
        else { const char *no = "CRASHDUMP arm-FAIL\n"; (void)sc3(SYS_write, 2, (long)no, 19); }
    }

    /* LOAD FIRST: our own loader phase (open/read/lseek/mmap) must run
     * filter-free — no listener consumer exists until the supervisor
     * pidfd-getfd's it AFTER the handshake, so trapping ourselves here
     * would park-deadlock (observed as ENETDOWN/SIGSYS on the child). */
    unsigned long phdr_addr = 0;
    unsigned long entry = load_guest(target_path, &phdr_addr);
    
    /* Now install filter and EMULATION-filter state, handshake the
     * listener to the supervisor, THEN jump. All post-install stub
     * syscalls are in the ALLOW set (write/read/close/seccomp/prctl) so
     * no self-trap can fire in this window. */
    int listener = -1;
    int nofilter = 0;
    for (char **e = stub_environ; e && *e; e++) {
        const char *p = "SPROUT_NS_NOFILTER=";
        unsigned i = 0;
        while (p[i] && (*e)[i] == p[i]) i++;
        if (i && !p[i]) { nofilter = 1; break; }
    }
    if (!nofilter) {
        listener = install_notify_filter();
            stub_handshake(listener);
        }

    /* guest stack: grow-down anonymous region */
    unsigned long stack_sz = 8UL * 1024 * 1024;
    long m = sc6(SYS_mmap, 0, (long)stack_sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m < 0 || m == -1) die("mmap guest stack", 125);
    u64 *stack_top = (u64 *)(m + stack_sz);

    u64 *guest_sp = build_stack(stack_top, argc0, argv, envp, auxv,
                                entry, phdr_addr, g_eh.e_phnum);

    if (stub_crashdump()) {
        char jb[196]; int jn = 0;
        const char *j = "JUMP entry="; for (const char *c = j; *c; c++) jb[jn++] = *c;
        jn = stub_hex(jb, jn, (unsigned long long)entry); jb[jn++] = ' ';
        const char *s2 = "sp="; for (const char *c = s2; *c; c++) jb[jn++] = *c;
        jn = stub_hex(jb, jn, (unsigned long long)guest_sp); jb[jn++] = ' ';
        const char *p2 = "phdr="; for (const char *c = p2; *c; c++) jb[jn++] = *c;
        jn = stub_hex(jb, jn, (unsigned long long)phdr_addr);
        jb[jn++] = '\n';
        (void)sc3(SYS_write, 2, (long)jb, jn);
    }
    /* jump: fresh sp + entry, no libc shutdown. REGISTER ORDER MATTERS:
     * entry must leave the compiler's hands BEFORE sp is clobbered — a
     * spill-reload of %1 after `mov sp` would read the *guest* stack
     * (fresh zeros/garbage) and branch into a non-exec anon mapping
     * (observed: glibc-static SIGBUS, fa==pc, high anon-stack address).
     * x9 scratch + clobber pins the entry in a known register first. */
    __asm__ volatile(
        "mov x9, %1\n"
        "mov sp, %0\n"
        "mov x29, #0\n"
        "mov x30, #0\n"
        "br x9\n"
        :
        : "r"(guest_sp), "r"(entry)
        : "memory", "x9");
    __builtin_unreachable();
}
