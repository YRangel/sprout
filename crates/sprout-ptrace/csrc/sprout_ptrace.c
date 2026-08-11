/*
 * sprout_ptrace.c — ptrace supervisor for sprout.
 *
 * Purpose (ADR-0006): guest glibc 2.41+ on Android 15+ dies during startup
 * because Android's untrusted-app seccomp policy lacks set_robust_list
 * (SIGSYS on use). LD_PRELOAD cannot help: the interposer maps *after*
 * the dying syscall. This tracer supervisor works around it by acting as
 * a parent ptracer that intercepts every syscall-stop and applies a
 * narrow policy.
 *
 * Policy:
 *   1. set_robust_list → replace sysno with getpid on entry (idempotent,
 *      always allowed); forge 0 as return value on exit. glibc's
 *      __pthread_initialize_minimal consumes the result but robust-list is
 *      optional on this kernel because we don't enable robust mutexes.
 *   2. openat/openat2/statx/newfstatat/faccessat/faccessat2/readlinkat/
 *      execve/execveat with absolute paths: translate via sp_translate
 *      (same pure core as the preload path), write back in place.
 *      caller-visible output buffers are NOT translated — that's v0.3.
 *   3. every other syscall: not touched.
 *
 * Tracee-state tracking is per-process and happens via PTRACE_GET_SYSCALL_INFO
 * (no in_syscall booleans), so fork/clone in the guest does not confuse us.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/* Pure translation from the preload core. */
#include "../../sprout-preload/csrc/sprout_preload.h"

/* aarch64 syscalls. */
#define SYS_openat            56
#define SYS_openat2          437
#define SYS_newfstatat        79
#define SYS_faccessat         48
#define SYS_faccessat2       439
#define SYS_readlinkat         78
#define SYS_execve            221
#define SYS_execveat          281
#define SYS_statx             291
#define SYS_set_robust_list    99
#define SYS_getpid            172

#define AT_FDCWD_VAL         (-100)

/* PTRACE_SET_SYSCALL rewrites the *cached* syscall number
 * (regs->syscallno) that both seccomp and the dispatcher consult on
 * aarch64. Writing x8 via PTRACE_SETREGSET is NOT sufficient — the kernel
 * already latched the original nr at entry. Value 23 is the arch-fixed
 * request on arm64; bionic headers simply don't export it. */
#define SP_PTRACE_SET_SYSCALL 23

/* Syscalls we are willing to silently emulate as success when seccomp
 * traps them (signal-stop fallback path).
 *
 * Known on Android 15+:
 *   SYS_set_robust_list  — glibc ≥ 2.39 unconditionally; robust-list is
 *                          advisory, so zeroing is fully safe.
 *   SYS_rseq             — glibc 2.41's rseq registration; treated as
 *                          ENOSYS on Android (rseq registration denied).
 *                          0 is safe: glibc falls back to non-rseq path.
 *
 * Unknown traps are still forwarded to the tracee with the original
 * signal so we don't silently mask real seccomp policy. */
/* Base table (all guests): glibc tolerates ENOSYS for these two init calls. */
static const long SP_EMULATE_BASE[] = { 99 /*set_robust_list*/, 293 /*rseq*/ };
/* Musl extra: faccessat init-poll + Android-blocked set*id family — the
 * "already at minimal privilege" truth of a rootless sandbox. Applied at
 * signal-stop level for musl-flavored tracees (kind 3, dynamic -of-musl),
 * NOT glibc (a guest that *legitimately* changes IDs must see EPERM, not
 * a fabricated success — that's why the fast path forgives only musl). */
static const long SP_EMULATE_MUSL_EXTRA[] = {
    48 /*faccessat*/, 143,144,145,146,147,149,151,152 /*set*id*/,
    159 /*setgroups*/
};

/* Linux 5.3+ exposes structured syscall info. */
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#define PTRACE_SYSCALL_INFO_ENTRY 1
#define PTRACE_SYSCALL_INFO_EXIT  2

static int g_debug = 0;
#define SP_TRACE(...) do { if (g_debug) fprintf(stderr, "[ptrace] " __VA_ARGS__); } while (0)

/* Ptrace state per tracee. */
#define SP_MAX_TRACEES 512
typedef struct {
    pid_t pid;
    int   sysno;
    int   in_sys;              /* 1 while inside a syscall (fallback state) */
    unsigned long long arg0;
    unsigned long long argN;
    int   got_robust_list;
    /* -1 = not yet classified at exec event, 0 = dynamic (preload governs),
     * 1 = static: no LD_PRELOAD possible, supervisor must translate. */
    int   static_kind;
} tracee_t;

static tracee_t g_tracees[SP_MAX_TRACEES];
static sp_config_t g_cfg;

/* Loader-chain context for rewriting static→dynamic execve (empty-/lib64
 * guest rootfs cannot satisfy PT_INTERP on the host). Provided by the CLI
 * plan via env; see LaunchPlan::supervisor. */
/* helpers defined below (forward decls for the exec rewriter) */
static long peek_str(pid_t pid, unsigned long long addr, char *out, size_t cap);
static int poke_str(pid_t pid, unsigned long long addr, const char *s, size_t len);

static const char *g_loader;      /* SPROUT_LOADER: sanitized guest loader (host) */
static const char *g_libpath;     /* SPROUT_LIBRARY_PATH: guest lib dirs (host) */
static const char *g_guestpreload;/* SPROUT_GUEST_PRELOAD: interposer:sanitized-libc */
static int g_libc_kind;           /* 0=glibc, 1=musl (SPROUT_LIBC) */
#define SP_LIBC_MUSL 1

/* tracee.kind: -1 unclassified, 0 dynamic preload-governed, 1 static,
 * 2 dynamic-Go (libc-linked yet raw-syscall — supervisor must translate).
 */
static const char *sp_kind_name(int k) {
    return k == 3 ? "MUSL-DYNAMIC (supervisor translates)" :
           k == 2 ? "GO-DYNAMIC (supervisor translates)" :
           k == 1 ? "STATIC (supervisor translates)" :
           k == 0 ? "dynamic (preload governs)" : "unclassified";
}
static const char *g_rootfs;      /* SPROUT_ROOTFS (guest root, host absolute) */

/* Host-file ELF inspection (open + phdrs). Returns 1 static, 0 dynamic,
 * 2 shebang-script (interp copied into buf), -1 not recognized. */
/* Busybox alpine layout: /bin/ls -> /bin/busybox absolute symlink would
 * resolve on the HOST (missing). Chase absolute symlink targets back
 * through the guest translation; relatives pass through. 8 hops. */
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

static int classify_host_file(const char *path, char interp_buf[SP_PATH_MAX],
                              char opt_buf[SP_PATH_MAX]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char head[256];
    size_t n = fread(head, 1, sizeof(head), f);
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        size_t i = 2;
        while (i < n && (head[i] == ' ' || head[i] == '\t')) i++;
        size_t j = 0;
        while (i < n && j < SP_PATH_MAX - 1 && head[i] != ' ' && head[i] != '\t'
               && head[i] != '\n' && head[i] != '\r') {
            interp_buf[j++] = (char)head[i++];
        }
        interp_buf[j] = '\0';
        /* ONE optional argument (kernel semantics): rest of the line */
        while (i < n && (head[i] == ' ' || head[i] == '\t')) i++;
        size_t k = 0;
        while (i < n && k < SP_PATH_MAX - 1 && head[i] != '\n' && head[i] != '\r') {
            opt_buf[k++] = (char)head[i++];
        }
        while (k > 0 && (opt_buf[k - 1] == ' ' || opt_buf[k - 1] == '\t')) k--;
        opt_buf[k] = '\0';
        fclose(f);
        return j ? 2 : -1;
    }
    if (n < 64 || memcmp(head, "\x7f" "ELF", 4) != 0 || head[4] != 2) { fclose(f); return -1; }
    unsigned long long e_phoff = *(unsigned long long *)(head + 32);
    unsigned short e_phentsize = *(unsigned short *)(head + 54);
    unsigned short e_phnum = *(unsigned short *)(head + 56);
    for (unsigned int i = 0; i < e_phnum && i < 32; i++) {
        if (fseek(f, (long)(e_phoff + i * e_phentsize), SEEK_SET) != 0) break;
        unsigned char ph[56];
        if (fread(ph, 1, sizeof(ph), f) != sizeof(ph)) break;
        if (*(unsigned int *)ph == PT_INTERP) { fclose(f); return 0; }
    }
    fclose(f);
    return 1;
}

static long peek_u64(pid_t pid, unsigned long long addr) {
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
    if (v == -1 && errno) return -1;
    return v;
}

/* Rewrite a STATIC tracee's execve of a DYNAMIC target into the sanitized
 * loader with the standard chain argv (mirrors the interposer's chain).
 * All new strings/arrays are built in the tracee's stack scratch area.
 * envp is rebuilt too, with LD_PRELOAD set so the chained (dynamic) child
 * gets the interposer (single-layer translation discipline). */
#define SP_EXEC_SCRATCH_BELOW_SP 65536
#define SP_EXEC_MAX_ARGS 256
#define SP_EXEC_STRING_CAP (SP_EXEC_SCRATCH_BELOW_SP - 4*4096)

static int sp_rewrite_exec_to_loader(tracee_t *t, pid_t pid, struct user_pt_regs *r,
                                     const char *host_prog, const char *guest_prog,
                                     int path_argi, int depth) {
    (void)t;
    if (!g_loader || !g_libpath || !g_rootfs) {
        SP_TRACE("[%d] rewrite bail: g_loader=%p g_libpath=%p g_rootfs=%p\n", pid, (void*)g_loader, (void*)g_libpath, (void*)g_rootfs);
        return 0;
    }
    if (depth > 2) { SP_TRACE("[%d] rewrite bail: depth\n", pid); return 0; }

    /* fetch original argv/envp */
    unsigned long long orig_argv = r->regs[path_argi + 1];
    unsigned long long orig_envp = r->regs[path_argi + 2];

    char *arg_strs[SP_EXEC_MAX_ARGS];
    int argc = 0;
    for (; argc < SP_EXEC_MAX_ARGS; argc++) {
        unsigned long long sp_ = (unsigned long long)peek_u64(pid, orig_argv + (unsigned long long)argc * 8);
        if (sp_ == 0 || sp_ == (unsigned long long)-1) break;
        arg_strs[argc] = malloc(SP_PATH_MAX);
        if (!arg_strs[argc]) return 0;
        long nn = peek_str(pid, sp_, arg_strs[argc], SP_PATH_MAX);
        (void)nn;
        if (nn < 0) { free(arg_strs[argc]); break; }
    }
    if (argc == 0) { SP_TRACE("[%d] rewrite bail: argc==0 orig_argv=%llx\n", pid, orig_argv); return 0; }

    int envc = 0;
    char *env_strs[SP_EXEC_MAX_ARGS];
    if (orig_envp) {
        for (; envc < SP_EXEC_MAX_ARGS; envc++) {
            unsigned long long sp_ = (unsigned long long)peek_u64(pid, orig_envp + (unsigned long long)envc * 8);
            if (sp_ == 0 || sp_ == (unsigned long long)-1) break;
            env_strs[envc] = malloc(SP_PATH_MAX);
            if (!env_strs[envc]) return 0;
            long nn = peek_str(pid, sp_, env_strs[envc], SP_PATH_MAX);
            if (nn < 0) { free(env_strs[envc]); break; }
        }
    }

    /* compose new argv: loader --argv0 a0 [--inhibit-cache] --library-path lp hostprog rest...
     * (--inhibit-cache is glibc-only; musl ldso would reject unknown opts) */

int musl = g_libc_kind == SP_LIBC_MUSL;
    int fixed_nbase = musl ? 6 : 7;
    /* entry COUNT (excluding NULL): N chain items + orig args[1..] */
    int new_argc = fixed_nbase + (argc - 1);
    if (new_argc > SP_EXEC_MAX_ARGS - 1) return 0;

    /* envp append/update set */
    char ld_preload[SP_PATH_MAX];
    snprintf(ld_preload, sizeof(ld_preload), "LD_PRELOAD=%s", g_guestpreload ? g_guestpreload : "");
    char env_loader[SP_PATH_MAX];
    snprintf(env_loader, sizeof(env_loader), "SPROUT_LOADER=%s", g_loader);
    char env_libpath[SP_PATH_MAX];
    snprintf(env_libpath, sizeof(env_libpath), "SPROUT_LIBRARY_PATH=%s", g_libpath);
    char env_rootfs[SP_PATH_MAX];
    snprintf(env_rootfs, sizeof(env_rootfs), "SPROUT_ROOTFS=%s", g_rootfs);
    char env_path[1024];
    snprintf(env_path, sizeof(env_path), "PATH=%s", getenv("PATH") ? getenv("PATH") : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    const char *inject_env_dbg[6];
    const char **inject_env = NULL;
    if (g_debug) {
        inject_env_dbg[0] = ld_preload;
        inject_env_dbg[1] = env_loader;
        inject_env_dbg[2] = env_libpath;
        inject_env_dbg[3] = "LD_DEBUG=libs";
        inject_env_dbg[4] = env_rootfs;
        inject_env_dbg[5] = env_path;
        inject_env = inject_env_dbg;
    } else {
        static const char *ie[6] = { NULL };
        ie[0] = ld_preload; ie[1] = env_loader; ie[2] = env_libpath;
        ie[3] = env_rootfs; ie[4] = env_path;
        inject_env = ie;
    }
    size_t inject_n = g_debug ? 6 : 5;

    unsigned long long base = (unsigned long long)r->sp - SP_EXEC_SCRATCH_BELOW_SP;
    /* probe *mapped-ness of the arena*/
    errno = 0;
    if (ptrace(PTRACE_PEEKDATA, pid, (void *)base, NULL) == -1 && errno) {
        SP_TRACE("[%d] rewrite bail: scratch probe @%llx errno=%d\n", pid, base, errno);
        return 0;
    }

    /* layout: argv array | envp array | strings arena */
    int new_envc = envc + (int)inject_n;
    unsigned long long argv_arr = base;
    unsigned long long envp_arr = argv_arr + (unsigned long long)(new_argc + 1) * 8;
    unsigned long long str_cur = envp_arr + (unsigned long long)(new_envc + 1) * 8;
    unsigned long long str_end = base + (unsigned long long)(2*4096) + 24*1024;
    if (str_end > base + SP_EXEC_SCRATCH_BELOW_SP) str_end = base + SP_EXEC_SCRATCH_BELOW_SP;

    char a0buf[SP_PATH_MAX];
    snprintf(a0buf, sizeof(a0buf), "%s", arg_strs[0][0] ? arg_strs[0] : guest_prog);

    const char **new_args = calloc((size_t)new_argc + 1, sizeof(char *));
    if (!new_args) return 0;
    int w = 0;
    new_args[w++] = g_loader;
    new_args[w++] = "--argv0";
    new_args[w++] = a0buf;
    if (!musl) new_args[w++] = "--inhibit-cache";
    new_args[w++] = "--library-path";
    new_args[w++] = g_libpath;
    new_args[w++] = host_prog;
    for (int i = 1; i < argc; i++) new_args[w++] = arg_strs[i];
    new_args[w] = NULL;

    /* envp: copy, replacing our injected keys if present */
    const char **new_env = calloc((size_t)new_envc + 1, sizeof(char *));
    if (!new_env) { free(new_args); return 0; }
    size_t injected_mask = 0; /* bit i = inject_env[i] matched an existing key */
    int ew = 0;
    for (int i = 0; i < envc; i++) {
        const char *e = env_strs[i];
        int replaced = 0;
        for (size_t k = 0; k < inject_n; k++) {
            const char *ik = inject_env[k];
            size_t knl = strchr(ik, '=') - ik + 1;
            if (strncmp(e, ik, knl) == 0) {
                new_env[ew++] = ik;
                injected_mask |= (1u << k);
                replaced = 1;
                break;
            }
        }
        if (!replaced) new_env[ew++] = e;
    }
    for (size_t k = 0; k < inject_n; k++)
        if (!(injected_mask & (1u << k))) new_env[ew++] = inject_env[k];
    new_env[ew] = NULL;
    new_envc = ew;

    /* now write arrays + strings into tracee */
    unsigned long long strptr[SP_EXEC_MAX_ARGS * 2];
    int nstr = 0;
    int total = 0;
    for (int i = 0; i < new_argc; i++) total += (int)strlen(new_args[i]) + 1;
    for (int i = 0; i < new_envc; i++) total += (int)strlen(new_env[i]) + 1;
    if ((unsigned long long)(str_cur + (unsigned long long)total) > str_end) {
        free(new_args); free(new_env); return 0;
    }

    /* write strings, record their tracee addrs */
    unsigned long long sc = str_cur;
    for (int i = 0; i < new_argc + new_envc; i++) {
        const char *s = (i < new_argc) ? new_args[i] : new_env[i - new_argc];
        size_t sl = strlen(s);
        if (poke_str(pid, sc, s, sl) != 0) { free(new_args); free(new_env); return 0; }
        strptr[nstr++] = sc;
        sc += ((unsigned long long)sl + 8) & ~7ULL; /* keep 8-aligned */
    }
    /* write argv array */
    for (int i = 0; i <= new_argc; i++) {
        unsigned long long val = (i == new_argc) ? 0 : strptr[i];
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(argv_arr + (unsigned long long)i * 8), (void *)val) == -1) {
            free(new_args); free(new_env); return 0;
        }
    }
    /* write envp array */
    for (int i = 0; i <= new_envc; i++) {
        unsigned long long val = (i == new_envc) ? 0 : strptr[new_argc + i];
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(envp_arr + (unsigned long long)i * 8), (void *)val) == -1) {
            free(new_args); free(new_env); return 0;
        }
    }
    r->regs[path_argi] = strptr[0];           /* loader path as exec target */
    r->regs[path_argi + 1] = argv_arr;
    r->regs[path_argi + 2] = envp_arr;

    if (g_debug) {
        SP_TRACE("[%d] static→dynamic exec: %s via loader (%d args, %d env)\n",
                 pid, guest_prog, new_argc, new_envc);
    }

    for (int i = 0; i < argc; i++) free(arg_strs[i]);
    for (int i = 0; i < envc; i++) free(env_strs[i]);
    free(new_args); free(new_env);
    return 1;
}

static tracee_t *find_or_add(pid_t pid) {
    for (int i = 0; i < SP_MAX_TRACEES; i++)
        if (g_tracees[i].pid == pid) return &g_tracees[i];
    for (int i = 0; i < SP_MAX_TRACEES; i++)
        if (g_tracees[i].pid == 0) {
            memset(&g_tracees[i], 0, sizeof(g_tracees[i]));
            g_tracees[i].pid = pid;
            g_tracees[i].static_kind = -1;
            return &g_tracees[i];
        }
    return NULL;
}

/* helpers */

static int set_ret_0(pid_t pid) {
    struct user_pt_regs r;
    struct iovec iov = { &r, sizeof(r) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
    r.regs[0] = 0;
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static long peek_str(pid_t pid, unsigned long long addr, char *out, size_t cap) {
    size_t i = 0;
    union { long v; char b[sizeof(long)]; } u;
    while (i < cap) {
        errno = 0;
        u.v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
        if (u.v == -1 && errno) return -errno;
        for (size_t k = 0; k < sizeof(long) && i + k < cap; k++) {
            out[i + k] = u.b[k];
            if (u.b[k] == '\0') return (long)(i + k);
        }
        i += sizeof(long);
    }
    return -ENAMETOOLONG;
}

static int poke_str(pid_t pid, unsigned long long addr, const char *s, size_t len) {
    for (size_t i = 0; i < len + 1; i += sizeof(long)) {
        union { long v; char b[sizeof(long)]; } u;
        size_t wr = (len + 1 - i < sizeof(long)) ? (len + 1 - i) : sizeof(long);
        u.v = 0;
        if (wr < sizeof(long)) {
            u.v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
            if (u.v == -1 && errno) return -errno;
        }
        memcpy(u.b, s + i, wr);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + i), (void *)u.v) == -1)
            return -errno;
    }
    return 0;
}

/* Detect a Go runtime binary by its PT_NOTE "Go" buildid note. Present in
 * all Go binaries (CGO or not, stripped or not): namesz=4, name="Go". */
#define SP_PT_NOTE 4
static int is_go_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char eh[64];
    if (fread(eh, 1, 64, f) != 64 || memcmp(eh, "\x7f" "ELF", 4) || eh[4] != 2) {
        fclose(f);
        return 0;
    }
    unsigned long long phoff = *(unsigned long long *)(eh + 32);
    unsigned short phentsize = *(unsigned short *)(eh + 54);
    unsigned short phnum = *(unsigned short *)(eh + 56);
    int go = 0;
    for (unsigned i = 0; i < phnum && i < 32 && !go; i++) {
        unsigned char ph[56];
        if (fseek(f, (long)(phoff + i * (unsigned long long)phentsize), SEEK_SET) != 0 ||
            fread(ph, 1, 56, f) != 56) break;
        if (*(unsigned *)ph != SP_PT_NOTE) continue;
        unsigned long long noff = *(unsigned long long *)(ph + 8);
        unsigned long long nsz  = *(unsigned long long *)(ph + 32);
        if (!nsz || nsz > (1 << 20)) continue;
        unsigned char *seg = malloc(nsz);
        if (!seg) break;
        if (fseek(f, (long)noff, SEEK_SET) != 0 || fread(seg, 1, nsz, f) != nsz) {
            free(seg);
            break;
        }
        size_t p = 0;
        while (p + 12 <= nsz) {
            unsigned namesz = *(unsigned *)(seg + p);
            unsigned descsz = *(unsigned *)(seg + p + 4);
            if (!namesz || namesz > 256 || p + 12 + namesz > nsz) break;
            if (namesz == 4 && memcmp(seg + p + 12, "Go\0\0", 4) == 0) { go = 1; break; }
            size_t na = ((size_t)namesz + 3) & ~(size_t)3;
            size_t da = ((size_t)descsz + 3) & ~(size_t)3;
            size_t np = p + 12 + na + da;
            if (np <= p || np > nsz) break;
            p = np;
        }
        free(seg);
    }
    fclose(f);
    return go;
}

/* For loader-chain images, /proc/pid/exe forever reports the LOADER (the
 * kernel exec'd ld.so; ld.so merely mapped the app). The real app lives in
 * the chain argv: [loader, --argv0, ., --inhibit-cache, --library-path,
 *   <lp>, <APP>, ...]. Extract the app path so classification sees the
 * real image (Go note scan applies to it, not the loader). */
static int chain_app_path(pid_t pid, char out[SP_PATH_MAX]) {
    char cp[64];
    snprintf(cp, sizeof(cp), "/proc/%d/cmdline", pid);
    FILE *f = fopen(cp, "rb");
    if (!f) return 0;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n < 2 || !g_loader) return 0;
    buf[n] = '\0';
    /* token 0 must be exactly the sanitized loader path (argv[0] of the
     * exec the supervisor or the chain rewriter issued). */
    size_t t0len = strlen(buf);
    if (t0len == 0 || strcmp(buf, g_loader) != 0) return 0;
    size_t pos = t0len + 1;
    size_t lp_pos = (size_t)-1;
    while (pos < n) {
        const char *tok = buf + pos;
        size_t tl = strlen(tok);
        if (strcmp(tok, "--library-path") == 0 && pos + tl + 1 < n) {
            lp_pos = pos + tl + 1;   /* libpath token */
            size_t ll = strlen(buf + lp_pos);
            if (lp_pos + ll + 1 >= n) return 0;
            const char *app = buf + lp_pos + ll + 1;
            if (*app) {
                snprintf(out, SP_PATH_MAX, "%s", app);
                return 1;
            }
            return 0;
        }
        pos += tl + 1;
    }
    return 0;
}

/* Classify the image a tracee just exec'd: static ELF or not.
 * Reads /proc/<pid>/exe's program headers; PT_INTERP presence means a
 * dynamic loader (preload path); ET_EXEC without PT_INTERP, or static-PIE
 * ET_DYN without PT_INTERP, are "static" for policy purposes.
 * Loader-chain images: classification targets the CHAIN APP (see above),
 * so a dynamic Go binary surfaces as kind 2 (translate + exec-rewrite,
 * same posture as static) because Go's runtime syscalls bypass libc. */
static int classify_tracee_image(pid_t pid) {
    char app[SP_PATH_MAX];
    if (chain_app_path(pid, app))
        return is_go_file(app) ? 2 : (g_libc_kind == SP_LIBC_MUSL ? 3 : 0);
    char exe[64];
    snprintf(exe, sizeof(exe), "/proc/%d/exe", pid);
    FILE *f = fopen(exe, "rb");
    if (!f) return -1;
    unsigned char eh[64];
    if (fread(eh, 1, sizeof(eh), f) != sizeof(eh)) { fclose(f); return -1; }
    if (memcmp(eh, "\x7f" "ELF", 4) != 0 || eh[4] != 2 /* ELFCLASS64 */) { fclose(f); return -1; }
    unsigned long long e_phoff = *(unsigned long long *)(eh + 32);
    unsigned short e_phentsize = *(unsigned short *)(eh + 54);
    unsigned short e_phnum = *(unsigned short *)(eh + 56);
    for (unsigned int i = 0; i < e_phnum && i < 32; i++) {
        if (fseek(f, (long)(e_phoff + i * e_phentsize), SEEK_SET) != 0) break;
        unsigned char ph[56];
        if (fread(ph, 1, sizeof(ph), f) != sizeof(ph)) break;
        unsigned int p_type = *(unsigned int *)ph;
        if (p_type == PT_INTERP) { fclose(f); return 0; }
    }
    fclose(f);
    /* No PT_INTERP: could be a static binary OR our sanitized guest
     * loader, which is static-PIE (ET_DYN). Loader-chain children exec it
     * by its SPROUT_LOADER path; that image is the dynamic runtime, not a
     * static target, so leave its host paths alone. */
    if (g_loader) {
        char link[SP_PATH_MAX];
        ssize_t nl = readlink(exe, link, sizeof(link) - 1);
        if (nl > 0) {
            link[nl] = '\0';
            if (strcmp(link, g_loader) == 0) return 0;
        }
    }
    if (is_go_file(exe)) return 2;   /* static Go: raw syscalls, translate */
    return 1;
}

/* Where we scribble translated path strings inside the tracee: 16 KiB
 * below the *current* stack pointer of the stopped thread. aarch64 has no
 * red zone and the thread is stopped at a syscall entry (its own code
 * cannot run until we continue it), so the region below SP is guaranteed
 * free of live data. Other threads never write below this thread's SP.
 * Max 4 KiB per stop; guest paths are usually << 1 KiB. */
#define SP_SCRATCH_BELOW_SP 16384
#define SP_SCRATCH_CAP      3072

/* Translate the pathname argument regs[argi] of a stopped STATIC tracee.
 * Current working directory of the tracee is not tracked, so only absolute
 * guest paths are rewritten (documented gap; at-family syscalls with a
 * real dirfd are skipped by callers). Returns 1 when regs were modified. */
/* Turn a relative guest path absolute using the tracee's cwd (host view
 * via /proc/<pid>/cwd, stripped of the rootfs prefix so it re-enters the
 * guest spelling). Returns 0 on success; -1 when cwd is outside the
 * guest rootfs (kernel judges then). */
static int guest_absolutize(pid_t pid, char guest[SP_PATH_MAX]) {
    if (guest[0] == '/') return 0;
    char cw[SP_PATH_MAX];
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/cwd", pid);
    ssize_t n = readlink(link, cw, sizeof(cw) - 1);
    if (n <= 0) return -1;
    cw[n] = '\0';
    size_t rl = strlen(g_cfg.rootfs);
    if (strncmp(cw, g_cfg.rootfs, rl) != 0) return -1;
    char joined[SP_PATH_MAX];
    int w = snprintf(joined, sizeof(joined), "%s/%s", cw[rl] ? cw + rl : "/", guest);
    if (w < 0 || (size_t)w >= sizeof(joined)) return -1;
    snprintf(guest, SP_PATH_MAX, "%s", joined);
    return 0;
}

static int translate_reg_path(tracee_t *t, pid_t pid, struct user_pt_regs *r, int argi,
                              const char *name) {
    (void)t;
    unsigned long long ptr = r->regs[argi];
    if (ptr == 0 || ptr >= 0x800000000000ULL) return 0;
    char guest[SP_PATH_MAX];
    if (peek_str(pid, ptr, guest, sizeof(guest)) < 0) return 0;
    if (guest_absolutize(pid, guest) != 0) return 0;
    char host[SP_PATH_MAX];
    if (!sp_translate(&g_cfg, guest, host)) return 0;
    /* Existence filter (phase-guard): force-prefixing loader-phase host
     * opens (cache .so under $PREFIX/...) would corrupt them. Translate
     * only when the candidate (or its parent, for O_CREAT-ish callers)
     * actually exists; otherwise leave the call against the host tree.
     * For pure static tracees this is equivalent (guest files all exist
     * under the rootfs); pseudo-paths like /proc fall to host anyway. */
    {
        struct stat st;
        if (lstat(host, &st) != 0) {
            char par[SP_PATH_MAX];
            snprintf(par, sizeof(par), "%s", host);
            char *sl = strrchr(par, '/');
            if (sl == par || !sl) {
                if (lstat("/", &st) != 0) return 0;
            } else {
                *sl = '\0';
                if (lstat(par, &st) != 0) return 0;
            }
        }
    }
    size_t hl = strlen(host);
    if (hl >= SP_SCRATCH_CAP) return 0;
    unsigned long long scratch = (unsigned long long)r->sp - SP_SCRATCH_BELOW_SP;
    /* probe mapped-ness of the scratch page first */
    errno = 0;
    if (ptrace(PTRACE_PEEKDATA, pid, (void *)scratch, NULL) == -1 && errno) return 0;
    if (poke_str(pid, scratch, host, hl) != 0) return 0;
    r->regs[argi] = scratch;
    if (g_debug) SP_TRACE("[%d] %s arg%d %s -> %s (scratch@sp-16k)\n", pid, name, argi, guest, host);
    return 1;
}

typedef struct {
    int ok;             /* 1 if GSI is available and op == ENTRY */
    long nr;            /* syscall number (kernel view, cached in regs->syscallno) */
    unsigned long long args[6];
} sp_syscall_view;

/* Fetch syscall info. GSI returns the *authoritative* nr; if GSI is not
 * supported, fall back to x8 from the register set. */
static sp_syscall_view sp_view_syscall(pid_t pid, struct user_pt_regs *regs) {
    sp_syscall_view v = {0};

    struct {
        unsigned char op;
        unsigned char pad[3];
        unsigned int arch;
        unsigned long long ip, sp;
        union {
            struct { unsigned long long nr; unsigned long long args[6]; } entry;
            struct { long long      rval; unsigned char     is_error; } exit_;
        } u;
    } info;

    long rc = ptrace((int)0x420e /*PTRACE_GET_SYSCALL_INFO*/,
                     pid, (void *)sizeof(info), &info);
    if (rc > 0 && info.op == 1 /*PTRACE_SYSCALL_INFO_ENTRY*/) {
        v.ok = 1;
        v.nr = (long)info.u.entry.nr;
        for (int i = 0; i < 6; i++) v.args[i] = info.u.entry.args[i];
        /* regs is authoritative for the rest of the policy code anyway */
        return v;
    }
    /* Fallback: nr from x8 in regs (caller must have already GETREGSET'd). */
    if (regs) {
        v.ok = 0;
        v.nr = (long)regs->regs[8];
        for (int i = 0; i < 6; i++) v.args[i] = regs->regs[i];
    }
    return v;
}

/* On aarch64, GET_SYSCALL_INFO arguments: for ENTRY the six args are at
 * indices 0..5 and syscall nr at 6. arg0 is in x0. */
/* dirfd-at-family syscalls whose pathname argument we translate for
 * STATIC tracees: {sysno, dirfd_argi, path_argi}. dirfd_argi == -1 means
 * there is no dirfd (path arg is standalone, e.g. execve/chdir). A real
 * dirfd (>= 0) skips translation: the fd was opened previously and the
 * kernel resolves relative to it. */
typedef struct { long sysno; int dirfd_argi; int path_argi; const char *name; } sp_path_rule;
static const sp_path_rule SP_PATH_RULES[] = {
    { SYS_openat,      0, 1, "openat"      },
    { SYS_openat2,     0, 1, "openat2"     },
    { SYS_newfstatat,  0, 1, "newfstatat"  },
    { SYS_faccessat,   0, 1, "faccessat"   },
    { SYS_faccessat2,  0, 1, "faccessat2"  },
    { SYS_readlinkat,  0, 1, "readlinkat"  },
    { SYS_statx,       0, 1, "statx"       },
    { 34 /*mkdirat*/,  0, 1, "mkdirat"     },
    { 35 /*unlinkat*/, 0, 1, "unlinkat"    },
    { 33 /*mknodat*/,  0, 1, "mknodat"     },
    { 53 /*fchmodat*/, 0, 1, "fchmodat"    },
    { 54 /*fchownat*/, 0, 1, "fchownat"    },
    { 88 /*utimensat*/,0, 1, "utimensat"   },
    { 36 /*symlinkat*/,   -1, 1, "symlinkat.linkpath" }, /* arg1 only: target is written literally */
    { 37 /*linkat*/,       0, 1, "linkat.oldpath"       },
    { 38 /*renameat*/,     0, 1, "renameat.oldpath"     },
    { 276/*renameat2*/,    0, 1, "renameat2.oldpath"    },
    { 49 /*chdir*/,       -1, 0, "chdir"     },
    { SYS_execve,     -1, 0, "execve"      },
    { SYS_execveat,    0, 1, "execveat"    },
};

static void apply_policy_entry(tracee_t *t, pid_t pid,
                                long sysno, unsigned long long x0, unsigned long long x1) {
    (void)x0; (void)x1;
    if (sysno == SYS_set_robust_list) {
        /* Rewrite the cached syscall number via PTRACE_SET_SYSCALL so
         * seccomp evaluates getpid (always allowed) instead. errno
         * distinguishes "no support" (EINVAL/ENOTSUP) from "stopped
         * mid-syscall" (EIO) etc. */
        errno = 0;
        long rc = ptrace((int)23, pid, (void *)0, (void *)(long)SYS_getpid);
        if (rc != 0) {
            if (g_debug) fprintf(stderr,
                "[ptrace] %d PTRACE_SET_SYSCALL failed rc=%ld errno=%d; relying on SIGSYS-swallow fallback\n",
                pid, rc, errno);
            return; /* signal-stop fallback below will handle it */
        }
        t->got_robust_list = 1;
        if (g_debug) SP_TRACE("[%d] set_robust_list → getpid\n", pid);
        return;
    }

    /* Lazy classification: the initial exec stop (post-TRACEME) is consumed
     * by the bare waitpid that precedes PTRACE_SETOPTIONS, so the main
     * program never produces a PTRACE_EVENT_EXEC we can see. The first
     * syscall-stop of any tracee, however, reliably runs after its exec. */
    if (t->static_kind == -1) {
        int kind = classify_tracee_image(pid);
        if (kind >= 0) {
            t->static_kind = kind;
            if (g_debug)
                SP_TRACE("[%d] lazily classified as %s\n", pid, sp_kind_name(kind));
        }
    }

    /* Path translation for supervisor-governed tracees (static=1, dynamic
     * Go=2). Plain dynamic processes have the LD_PRELOAD interposer. */
    if (t->static_kind <= 0) return;

    /* execve/execveat of a DYNAMIC (or script) target from a static
     * process: the kernel cannot satisfy PT_INTERP on the host (empty
     * /lib64 in proot-distro containers), so we rewrite the call into
     * the sanitized-loader chain (same shape the interposer uses). */
    if (sysno == SYS_execve || sysno == SYS_execveat) {
        int path_argi = (sysno == SYS_execve) ? 0 : 1;
        struct user_pt_regs rex;
        struct iovec iovex = { &rex, sizeof(rex) };
        if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovex) != 0) return;
        if (sysno == SYS_execveat && (int)rex.regs[0] != AT_FDCWD_VAL) return;

        unsigned long long pptr = rex.regs[path_argi];
        if (pptr == 0 || pptr >= 0x800000000000ULL) return;
        char guest[SP_PATH_MAX];
        if (peek_str(pid, pptr, guest, sizeof(guest)) < 0) return;
        if (guest_absolutize(pid, guest) != 0) return;
        char host[SP_PATH_MAX];
        if (!sp_translate(&g_cfg, guest, host)) return;
        sp_resolve_absolute_symlink(host);

        char obuf[SP_PATH_MAX];
        char ibuf[SP_PATH_MAX];
        int cls = classify_host_file(host, ibuf, obuf);
        if (g_debug) SP_TRACE("[%d] exec target %s -> host %s cls=%d\n", pid, guest, host, cls);
        if (cls == 0) {
            /* dynamic: full loader-chain rewrite */
            if (sp_rewrite_exec_to_loader(t, pid, &rex, host, guest, path_argi, 0))
                ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovex);
            return;
        }
        if (cls == 2) {
            /* script from a static process: resolve interpreter via guest
             * PATH (fallback default), build argv [interp, opt?, script,
             * rest...] and try to chain once more (depth-guarded inside). */
            char cand[SP_PATH_MAX];
            int found = 0;
            if (ibuf[0] == '/' || strchr(ibuf, '/') != NULL) {
                /* absolute or path-qualified interpreter: use as-is */
                snprintf(cand, sizeof(cand), "%s", ibuf);
                char hc0[SP_PATH_MAX];
                const char *h0 = sp_translate(&g_cfg, cand, hc0) ? hc0 : cand;
                found = (access(h0, X_OK) == 0);
            } else {
                const char *sp_ = getenv("SPROUT_GUEST_PATH");
                if (!sp_) sp_ = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
                char pbuf[4096];
                snprintf(pbuf, sizeof(pbuf), "%s", sp_);
                for (char *d = strtok(pbuf, ":"); d && !found; d = strtok(NULL, ":")) {
                    char cc[SP_PATH_MAX];
                    int n = snprintf(cc, sizeof(cc), "%s/%s", *d ? d : ".", ibuf);
                    if (n < 0 || (size_t)n >= sizeof(cc)) continue;
                    char hc[SP_PATH_MAX];
                    const char *h = sp_translate(&g_cfg, cc, hc) ? hc : cc;
                    if (access(h, X_OK) == 0) { snprintf(cand, sizeof(cand), "%s", cc); found = 1; }
                }
            }
            if (!found) return; /* honest ENOENT for the kernel */
            char hcan[SP_PATH_MAX];
            if (!sp_translate(&g_cfg, cand, hcan)) return;
            char ibuf2[SP_PATH_MAX];
            int cls2 = classify_host_file(hcan, ibuf2, obuf);
            if (cls2 == 0) {
                /* interp is dynamic: rewrite exec with interp as target and
                 * argv [interp, opt?, script, rest]. We re-use the loader
                 * rewriter by constructing a synthetic argv in the tracee:
                 * simpler path: rewrite argv array first (interp, script + rest)
                 * then let the loader rewriter consume it. */
                /* build synthetic argv in scratch: strings [interp, script, rest1...] */
                unsigned long long base = (unsigned long long)rex.sp - SP_EXEC_SCRATCH_BELOW_SP;
                errno = 0;
                if (ptrace(PTRACE_PEEKDATA, pid, (void *)base, NULL) == -1 && errno) return;
                unsigned long long orig_argv = rex.regs[path_argi + 1];
                long strings_base = (long)(base + (unsigned long long)(SP_EXEC_MAX_ARGS * 8));
                unsigned long long sc = (unsigned long long)strings_base;
                unsigned long long end = base + SP_EXEC_SCRATCH_BELOW_SP;
                unsigned long long arr_ptrs[SP_EXEC_MAX_ARGS];
                int na = 0;
                const char *head2[3];
                int nh = 0;
                head2[nh++] = cand;             /* interp (guest path) */
                if (obuf[0]) head2[nh++] = obuf; /* optional shebang arg */
                head2[nh++] = guest;            /* script path */
                for (int i = 0; i < nh && na < SP_EXEC_MAX_ARGS - 1; i++) {
                    size_t sl = strlen(head2[i]);
                    if (sc + sl + 16 >= end) return;
                    if (poke_str(pid, sc, head2[i], sl) != 0) return;
                    arr_ptrs[na++] = sc;
                    sc += ((unsigned long long)sl + 8) & ~7ULL;
                }
                for (int i = 1; na < SP_EXEC_MAX_ARGS - 1; i++) {
                    unsigned long long pa = (unsigned long long)peek_u64(pid, orig_argv + (unsigned long long)i * 8);
                    if (pa == 0 || pa == (unsigned long long)-1) break;
                    char abuf[SP_PATH_MAX];
                    if (peek_str(pid, pa, abuf, sizeof(abuf)) < 0) break;
                    size_t sl = strlen(abuf);
                    if (sc + sl + 16 >= end) break;
                    if (poke_str(pid, sc, abuf, sl) != 0) break;
                    arr_ptrs[na++] = sc;
                    sc += ((unsigned long long)sl + 8) & ~7ULL;
                }
                unsigned long long arr = base;
                for (int i = 0; i <= na; i++)
                    if (ptrace(PTRACE_POKEDATA, pid, (void *)(arr + (unsigned long long)i * 8),
                               (void *)(i == na ? 0 : arr_ptrs[i])) == -1) return;
                rex.regs[path_argi] = arr_ptrs[0];   /* exec target = interp guest path… but kernel needs HOST */
                /* interp is dynamic: exec target must be the loader, not cand.
                 * Recurse through the dynamic rewriter with the synthetic argv. */
                rex.regs[path_argi + 1] = arr;
                /* pretend the program is the interp: rewriter expects
                 * host_prog to EXEC, argv[0] = interp */
                if (sp_rewrite_exec_to_loader(t, pid, &rex, hcan, cand, path_argi, 1))
                    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovex);
                return;
            }
            /* static INTERP under a static parent: not wired (deepest
             * corner: scripts whose shebang points at a static binary).
             * Honest no-op: kernel returns ENOEXEC on the script, which
             * matches the behavior one gets without translation anyway. */
            return;
        }
        /* static target (or unknown): plain single-string path translation */
        int changed = translate_reg_path(t, pid, &rex, path_argi, "execve");
        if (changed)
            ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovex);
        return;
    }

    for (size_t i = 0; i < sizeof(SP_PATH_RULES)/sizeof(*SP_PATH_RULES); i++) {
        const sp_path_rule *rule = &SP_PATH_RULES[i];
        if (rule->sysno != sysno) continue;
        if (rule->dirfd_argi >= 0) {
            /* dirfd = regs[rule->dirfd_argi]; only AT_FDCWD translates
             * (real dirfds resolve in kernel space against an fd whose
             * path we don't mirror). GSI already filed regs; we re-fetch
             * lazily only when a rule matched. */
            struct user_pt_regs rchk;
            struct iovec iovchk = { &rchk, sizeof(rchk) };
            if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
            if ((int)rchk.regs[rule->dirfd_argi] != AT_FDCWD_VAL) return;

            int changed = translate_reg_path(t, pid, &rchk, rule->path_argi, rule->name);
            if (changed)
                ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovchk);
        } else {
            struct user_pt_regs rchk;
            struct iovec iovchk = { &rchk, sizeof(rchk) };
            if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
            if (translate_reg_path(t, pid, &rchk, rule->path_argi, rule->name))
                ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovchk);
        }
        return; /* one rule per sysno */
    }
}

int main(int argc, char **argv) {
    const char *rootfs = getenv("SPROUT_ROOTFS");
    if (!rootfs) {
        fprintf(stderr, "usage: SPROUT_ROOTFS=/path %s -- <guest-cmd> [args]\n", argv[0]);
        return 2;
    }
    if (argc < 3 || strcmp(argv[1], "--") != 0) {
        fprintf(stderr, "usage: %s -- <guest-cmd> [args]\n", argv[0]);
        return 2;
    }
    g_debug = getenv("SPROUT_DEBUG") != NULL;
    sp_config_load(&g_cfg);
    /* Loader-chain context (LaunchPlan::supervisor passes these in). */
    g_loader = getenv("SPROUT_LOADER");
    g_libpath = getenv("SPROUT_LIBRARY_PATH");
    g_guestpreload = getenv("SPROUT_GUEST_PRELOAD");
    { const char *k = getenv("SPROUT_LIBC"); g_libc_kind = (k && strcmp(k, "musl") == 0) ? SP_LIBC_MUSL : 0; }
    g_rootfs = getenv("SPROUT_ROOTFS");

    /* Host env first; also empty bionic LD_* so the supervisor itself
     * doesn't get tangled in bionic dynamic-linker state. */
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("LINKER");

    /* Guest-specific: if SPROUT_GUEST_PRELOAD is set, that value (only) is
     * what the guest child will see as LD_PRELOAD. This lets the sprout CLI
     * decouple supervisor-binary linking from guest-library linking. */
    const char *guest_preload = getenv("SPROUT_GUEST_PRELOAD");
    /* then clear ourselves */
    unsetenv("LD_PRELOAD");

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) _exit(127);
        if (guest_preload) setenv("LD_PRELOAD", guest_preload, /*override=*/1);
        execve(argv[2], &argv[2], environ);
        _exit(127);
    }

    if (waitpid(child, NULL, 0) < 0) { perror("waitpid"); return 1; }
    if (ptrace(PTRACE_SETOPTIONS, child, 0,
               PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
               PTRACE_O_TRACEEXEC | PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) < 0) {
        perror("PTRACE_SETOPTIONS"); return 1;
    }
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0) { perror("PTRACE_SYSCALL"); return 1; }

    int status = 0;
    for (;;) {
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            perror("waitpid"); break;
        }
        if (WIFEXITED(status)) {
            if (w == child) return WEXITSTATUS(status);
            goto cont;
        }
        if (WIFSIGNALED(status)) {
            if (w == child) {
                if (g_debug) {
                    struct user_pt_regs r;
                    struct iovec iov = { &r, sizeof(r) };
                    if (ptrace(PTRACE_GETREGSET, w, (void *)NT_PRSTATUS, &iov) == 0) {
                        fprintf(stderr, "[ptrace] died sig=%d pc=%llx x8=%llu\n",
                                WTERMSIG(status), (unsigned long long)r.pc, r.regs[8]);
                        /* Symbolize: which mapping contains pc? */
                        char mapfile[64];
                        snprintf(mapfile, sizeof(mapfile), "/proc/%d/maps", w);
                        FILE *mf = fopen(mapfile, "r");
                        if (mf) {
                            char line[1024];
                            unsigned long long pc = (unsigned long long)r.pc;
                            while (fgets(line, sizeof(line), mf)) {
                                unsigned long long lo, hi;
                                if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && pc >= lo && pc < hi) {
                                    unsigned long long off;
                                    char path[512] = "?";
                                    if (sscanf(line, "%*x-%*x %*s %llx %*s %*s %511[^\n]",
                                               &off, path) < 1) off = 0;
                                    fprintf(stderr, "[ptrace] pc module: %s +%llx\n", path, pc - lo + off);
                                    break;
                                }
                            }
                            fclose(mf);
                        }
                    }
                }
                return 128 + WTERMSIG(status);
            }
            goto cont;
        }
        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        if (g_debug && sig != (SIGTRAP | 0x80) && sig != SIGTRAP)
            fprintf(stderr, "[ptrace] %d stopped sig=%d\n", w, sig);
        tracee_t *t = find_or_add(w);
        if (!t) goto cont;

        /* Safety net: SECCOMP_RET_TRAP delivers SIGSYS as a signal-stop
         * *before* the syscall executes (registers untouched). If the
         * trapped syscall is on our emulate-OK whitelist, swallow the
         * signal and forge x0=0; the guest believes the call succeeded. */
        if (sig == SIGSYS) {
            struct user_pt_regs r;
            struct iovec iov = { &r, sizeof(r) };
            int emulated = 0;
            if (ptrace(PTRACE_GETREGSET, w, (void *)NT_PRSTATUS, &iov) == 0) {
                sp_syscall_view v = sp_view_syscall(w, &r);
                if (g_debug)
                    fprintf(stderr, "[ptrace] %d SIGSYS stop eNR=%lld x8=%llu gsi_nr=%ld pc=%llx\n",
                            w, (long long)r.regs[8], r.regs[8], v.nr,
                            (unsigned long long)r.pc);
                int use_musl_extra = (t->static_kind == 3) || (g_libc_kind == SP_LIBC_MUSL);
                for (size_t i = 0; i < sizeof(SP_EMULATE_BASE)/sizeof(*SP_EMULATE_BASE) && !emulated; i++) {
                    if ((long)r.regs[8] == SP_EMULATE_BASE[i]) emulated = 1;
                }
                if (!emulated && use_musl_extra) {
                    for (size_t i = 0; i < sizeof(SP_EMULATE_MUSL_EXTRA)/sizeof(*SP_EMULATE_MUSL_EXTRA); i++) {
                        if ((long)r.regs[8] == SP_EMULATE_MUSL_EXTRA[i]) { emulated = 1; break; }
                    }
                }
                if (emulated) {
                    r.regs[0] = 0;
                    ptrace(PTRACE_SETREGSET, w, (void *)NT_PRSTATUS, &iov);
                    if (g_debug)
                        fprintf(stderr, "[ptrace] %d SIGSYS swallowed: sysno=%llu emulated ok\n",
                                w, r.regs[8]);
                }
            }
            /* swallow or deliver */
            ptrace(PTRACE_SYSCALL, w, 0, emulated ? (void *)0 : (void *)(long)sig);
            continue;
        }

        if (sig == (SIGTRAP | 0x80)) {  /* syscall-stop */
            struct user_pt_regs r;
            struct iovec iov = { &r, sizeof(r) };
            if (ptrace(PTRACE_GETREGSET, w, (void *)NT_PRSTATUS, &iov) != 0) goto cont;
            sp_syscall_view v = sp_view_syscall(w, &r);
            if (v.ok) {                /* GSI ENTRY */
                t->sysno = v.nr;
                apply_policy_entry(t, w, v.nr, v.args[0], v.args[1]);
            } else {                   /* no GSI: use x8 + local in_sys toggle */
                if (!t->in_sys) {
                    t->sysno = v.nr;
                    apply_policy_entry(t, w, v.nr, v.args[0], v.args[1]);
                    t->in_sys = 1;
                } else {
                    if (t->got_robust_list) { set_ret_0(w); t->got_robust_list = 0; }
                    t->in_sys = 0;
                }
            }
            goto cont;
        }
        /* PTRACE_EVENT stops arrive as plain SIGTRAP with an event code in
         * the high 16 bits of status. PTRACE_EVENT_EXEC is where we learn
         * what image the tracee now runs: classify static vs dynamic so the
         * entry policy knows whether IT has to translate paths. */
        if (sig == SIGTRAP) {
            unsigned int ev = (unsigned int)status >> 16;
            if (ev == (unsigned int)PTRACE_EVENT_EXEC) {
                int kind = classify_tracee_image(w);
                if (kind >= 0) {
                    t->static_kind = kind;
                    if (g_debug)
                        SP_TRACE("[%d] exec event: image is %s\n", w,
                                 sp_kind_name(kind));
                }
            }
            goto cont;
        }
        ptrace(PTRACE_SYSCALL, w, 0, (void *)(long)sig);
        continue;
    cont:
        ptrace(PTRACE_SYSCALL, w, 0, 0);
    }
    return 0;
}
