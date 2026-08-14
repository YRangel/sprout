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
#include <fcntl.h> /* O_* — host sys/chains pull this in transitively, guest glibc doesn't; was the only reason guest-side -Wall compiles errored on O_* */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
/* gnu-libc (ubuntu-24.04-arm CI runner) does NOT pull `struct user_pt_regs`
 * through <sys/ptrace.h> on aarch64 — bionic does, which is why this buried
 * glibc-only compile failure survived until release automation (`storage
 * size of 'rr' isn't known` at 5 sites). <asm/ptrace.h> carries the type
 * on gnu-libc and is absent-only on unusual libc sets, hence __has_include. */
#if defined(__aarch64__) && defined(__has_include)
#  if __has_include(<asm/ptrace.h>)
#    include <asm/ptrace.h>
#  endif
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/stat.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <poll.h>


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
/* SIGSYS emulation table {sysno, fabricated x0}. Success-emulation for
 * the flag-set family (glibc/startup probes), -ENOSYS for anything that
 * callers must FALL BACK from honestly (io_uring: libuv probes support by
 * calling it — success would make the guest take a broken fast path). */
typedef struct { long sys; long ret; } sp_emul_rule;
static const sp_emul_rule SP_EMULATE_BASE[] = {
    { 99,  0 },    /* set_robust_list */
    { 293, 0 },    /* rseq */
    { 425, -38 },  /* io_uring_setup    -> -ENOSYS */
    { 426, -38 },  /* io_uring_enter    -> -ENOSYS */
    { 427, -38 },  /* io_uring_register -> -ENOSYS */
    /* SysV IPC: Android netsys kills the whole family for untrusted apps
     * (msg*186-189, sem*190/193, shm*194-197). Real IPC can't be faked,
     * and no proot-distro guest ever had it — surface -ENOSYS so X/MIT-SHM
     * users (libxcb XShmPixmap) and friends take their no-shm fallback
     * instead of dying. Observed 2026-08-13: xfce4-session family killed
     * by SIGSYS nr=194 (shmget) the MOMENT xfwm/panel/desktop came up,
     * session mapping 0 windows; exact signal consumer = libxcb.
     * Numbers verified against asm-generic/unistd.h. */
    { 186, -38 },  /* msgget   */
    { 187, -38 },  /* msgctl   */
    { 188, -38 },  /* msgrcv   */
    { 189, -38 },  /* msgsnd   */
    { 190, -38 },  /* semget   */
    { 191, -38 },  /* semctl   */
    { 192, -38 },  /* semtimedop */
    { 193, -38 },  /* semop    */
    { 194, -38 },  /* shmget   */
    { 195, -38 },  /* shmctl   */
    { 196, -38 },  /* shmat    */
    { 197, -38 },  /* shmdt    */
    { 439, -38 },  /* faccessat2 raw callers -> -ENOSYS (libc falls back) */
    { 452, -38 },  /* fchmodat2 (GNU tar >= 1.35) -> -ENOSYS (fallback to fchmodat) */
    /* accept(202) deliberately NOT here: Android policy layers trigger
     * SIGSYS on it under our supervisor chain (tmux server accept is the
     * case). It must be PIVOTED to accept4(242) at the SIGSYS stop
     * (x8 rewrite, no ENOSYS): glibc accept(fd,addr,len) == accept4(fd,addr,len,0);
     * serving via the listener NEVER fires because the ANDROID policy TRAP
     * for nr 202 has precedence over our USER_NOTIF. */
    { 159, -1 },   /* setgroups — Android TRAPs; EPERM truth, fakeroot 0 */
    { 143, -1 },   /* setuid  */
    { 144, -1 },   /* setreuid */
    { 145, -1 },   /* setfsuid */
    { 146, -1 },   /* setgid */
    { 147, -1 },   /* setregid */
    { 149, -1 },   /* setresuid */
    { 151, -1 },   /* setresgid */
    { 152, -1 },   /* setfsgid */
};
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

/* Ptrace state per tracee. Firefox+Xfce sessions can transiently exceed
 * 512 traced PIDs/TIDs (procs + CLONE_THREAD stops); 4096 sits inline with
 * proot's own ceilings (sizeof(tracee_t) is small). */
#define SP_MAX_TRACEES 4096
typedef struct {
    pid_t pid;
    int   sysno;
    int   in_sys;              /* 1 while inside a syscall (fallback state) */
    unsigned long long arg0;
    unsigned long long argN;
    int   got_robust_list;
    /* pending reverse-AF_UNIX translation: armed at ENTRY for
     * getsockname/recvfrom/recvmsg, consumed at the matching EXIT stop. */
    long  rev_sysno;               /* 0 = none */
    unsigned long long rev_addr;   /* sockaddr* (or msghdr* for recvmsg 212) */
    unsigned long long rev_lenp;   /* socklen_t* for 204/207 */
    /* -1 = not yet classified at exec event, 0 = dynamic (preload governs),
     * 1 = static: no LD_PRELOAD possible, supervisor must translate. */
    int   static_kind;
    /* 1 = interposed image (guest libc + LD_PRELOAD inside): the interposer
     * translates paths; the supervisor only emulates SIGSYS victims and
     * watches exec events. SHADOW tracees free-run via PTRACE_CONT — no
     * per-syscall stop, so glibc apps keep fast-path perf. */
    int   shadow;
} tracee_t;

static tracee_t g_tracees[SP_MAX_TRACEES];
static sp_config_t g_cfg;
static int g_shadow = 0;               /* SPROUT_SHADOW: root image starts shadowed */

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

static int sp_serve_script_exec(pid_t pid, struct user_pt_regs *r,
                                int path_argi, const char *guest, const char *host);

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
        int skip_n = 0;
        for (;;) {
            if (envc + skip_n >= SP_EXEC_MAX_ARGS) break;
            unsigned long long sp_ = (unsigned long long)peek_u64(pid, orig_envp + (unsigned long long)(envc + skip_n) * 8);
            if (sp_ == 0 || sp_ == (unsigned long long)-1) break;
            char *tmp = malloc(SP_PATH_MAX);
            if (!tmp) return 0;
            long nn = peek_str(pid, sp_, tmp, SP_PATH_MAX);
            if (nn < 0) { free(tmp); break; }
            /* statics→dynamic: children stay supervisor-translated through
             * the tree-wide ptrace watch; the inherited LD_PRELOAD /
             * LD_LIBRARY_PATH strings name HOST paths inside the child and
             * only print 'cannot be preloaded' noise (and confuse tools
             * that parse ld.so stderr). Strip sprout-authored entries. */
            if (strstr(tmp, "libsprout-core") || strstr(tmp, "libc-sanitized")
                || strstr(tmp, "ldso-sanitized") || strstr(tmp, "musl-shadow-lib")) {
                free(tmp); skip_n++; continue;
            }
            env_strs[envc] = tmp;
            envc++; (void)skip_n;
        }
    }

    /* compose new argv: loader --argv0 a0 [--inhibit-cache] --library-path lp hostprog rest...
     * (--inhibit-cache is glibc-only; musl ldso would reject unknown opts) */

int musl = g_libc_kind == SP_LIBC_MUSL;
    int fixed_nbase = musl ? 6 : 7;
    /* entry COUNT (excluding NULL): N chain items + orig args[1..] */
    int new_argc = fixed_nbase + (argc - 1);
    if (new_argc > SP_EXEC_MAX_ARGS - 1) return 0;

    /* envp append/update set. NOTE: no LD_PRELOAD/LD_LIBRARY_PATH here:
     * the new process stays supervisor-translated through the tree-wide
     * ptrace watch, so the interposer string is pure noise in this lane
     * (and its host path prints 'cannot be preloaded' in the guest ld.so). */
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
        inject_env_dbg[0] = env_loader;
        inject_env_dbg[1] = env_libpath;
        inject_env_dbg[2] = "LD_DEBUG=libs";
        inject_env_dbg[3] = env_rootfs;
        inject_env_dbg[4] = env_path;
        inject_env_dbg[5] = "LD_BIND_NOW=1";
        inject_env = inject_env_dbg;
    } else {
        static const char *ie[6] = { NULL };
        ie[0] = env_loader; ie[1] = env_libpath; ie[2] = env_rootfs;
        ie[3] = env_path; ie[4] = "LD_BIND_NOW=1";
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

/* pipeline churn spawned past SP_MAX_TRACEES used to soft-fail silently:
 * find_or_add returned NULL and every later event was blindly CONT'd —
 * untranslated execs then died at rc 127/1 with zero messages. Reap the
 * slot the instant a tracee exits. */
static void sp_tracee_free(pid_t pid) {
    for (int i = 0; i < SP_MAX_TRACEES; i++)
        if (g_tracees[i].pid == pid) { g_tracees[i].pid = 0; return; }
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
static int classify_tracee_image_uncached(pid_t pid);
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
    /* Cache keyed on the exec'd file's (dev,ino): spawn-heavy guests
     * (busybox pipelines) classify the SAME binary hundreds of times;
     * each miss is ~6-10 syscalls of /proc reads + ELF poking. */
    static struct { dev_t dev; ino_t ino; int kind; } cache[32];
    char exe[64];
    snprintf(exe, sizeof(exe), "/proc/%d/exe", pid);
    struct stat est;
    if (stat(exe, &est) == 0) {
        size_t oldest = 0; long oldest_n = 0;
        static long tick = 0;
        tick++;
        for (size_t i = 0; i < sizeof(cache)/sizeof(cache[0]); i++) {
            if (cache[i].ino == est.st_ino && cache[i].dev == est.st_dev)
                return cache[i].kind;
        }
        (void)oldest; (void)oldest_n;
        int kind = classify_tracee_image_uncached(pid);
        cache[tick % 32].dev = est.st_dev;
        cache[tick % 32].ino = est.st_ino;
        cache[tick % 32].kind = kind;
        return kind;
    }
    return classify_tracee_image_uncached(pid);
}

static int classify_tracee_image_uncached(pid_t pid) {
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
    /* per-argument scratch slot: two-pathname syscalls (linkat/renameat)
     * translate BOTH paths and the strings must coexist at kernel time. */
    unsigned long long scratch = (unsigned long long)r->sp - SP_SCRATCH_BELOW_SP
                                 - ((unsigned long long)argi * SP_SCRATCH_CAP);
    /* probe mapped-ness of the scratch page first */
    errno = 0;
    if (ptrace(PTRACE_PEEKDATA, pid, (void *)scratch, NULL) == -1 && errno) return 0;
    if (poke_str(pid, scratch, host, hl) != 0) return 0;
    r->regs[argi] = scratch;
    if (g_debug) SP_TRACE("[%d] %s arg%d %s -> %s (scratch@sp-16k)\n", pid, name, argi, guest, host);
    return 1;
}

static void sp_dbg_cwd(const char *tag, pid_t pid) {
    char cw[SP_PATH_MAX];
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/cwd", pid);
    ssize_t n = readlink(link, cw, sizeof(cw) - 1);
    if (n > 0) { cw[n] = 0; SP_TRACE("[cwd] %s pid=%d cwd=%s\n", tag, pid, cw); }
}

typedef struct {
    int ok;             /* 1 if GSI is available and op == ENTRY */
    int op;             /* 1=ENTRY 2=EXIT 0=unknown (GSI absent/failed) */
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
    if (rc > 0) v.op = info.op;
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
 * STATIC tracees: {sysno, dirfd_argi, path_argi, dirfd2_argi, path2_argi}.
 * dirfd_argi == -1 means there is no dirfd (path arg is standalone, e.g.
 * execve/chdir). A real dirfd (>= 0) skips translation: the fd was opened
 * previously and the kernel resolves relative to it. Two-pathname syscalls
 * (linkat/renameat/renameat2) translate BOTH path args, each gated on its
 * own dirfd. */
typedef struct { long sysno; int dirfd_argi; int path_argi; int dirfd2_argi; int path2_argi; const char *name; } sp_path_rule;
static const sp_path_rule SP_PATH_RULES[] = {
    { SYS_openat,      0, 1, -1, -1, "openat"      },
    { SYS_openat2,     0, 1, -1, -1, "openat2"     },
    { SYS_newfstatat,  0, 1, -1, -1, "newfstatat"  },
    { SYS_faccessat,   0, 1, -1, -1, "faccessat"   },
    { SYS_faccessat2,  0, 1, -1, -1, "faccessat2"  },
    { SYS_readlinkat,  0, 1, -1, -1, "readlinkat"  },
    { SYS_statx,       0, 1, -1, -1, "statx"       },
    { 34 /*mkdirat*/,  0, 1, -1, -1, "mkdirat"     },
    { 35 /*unlinkat*/, 0, 1, -1, -1, "unlinkat"    },
    { 33 /*mknodat*/,  0, 1, -1, -1, "mknodat"     },
    { 53 /*fchmodat*/, 0, 1, -1, -1, "fchmodat"    },
    { 54 /*fchownat*/, 0, 1, -1, -1, "fchownat"    },
    { 88 /*utimensat*/,0, 1, -1, -1, "utimensat"   },
    /* two-arg semantics: only the linkpath translates (target written
     * literally); oldpath gating on its own dirfd, same for newpath. */
    { 36 /*symlinkat*/,   1, 2, -1, -1, "symlinkat.linkpath" },
    { 37 /*linkat*/,       0, 1, 2, 3, "linkat"            },
    { 38 /*renameat*/,     0, 1, 2, 3, "renameat"          },
    { 276/*renameat2*/,    0, 1, 2, 3, "renameat2"         },
    { 49 /*chdir*/,       -1, 0, -1, -1, "chdir"     },
    { SYS_execve,     -1, 0, -1, -1, "execve"      },
    { SYS_execveat,    0, 1, -1, -1, "execveat"    },
};

/* ---- AF_UNIX pathname translation (ADR-0010) -------------------------- */
/* Forward direction only in v1: bind/connect/sendto/sendmsg. Reverse
 * (getsockname/recvfrom) for supervisor tracees is a documented gap;
 * streamed workloads (X11 stream need connect only) work anyway.
 * Shared scratch stack below SP (same arena translate_reg_path uses):
 * path content and sockaddr struct are placed at distinct offsets. */
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>

/* Try translating the sockaddr_un at tracee memory `sa` (len bytes).
 * On success: build the translated sockaddr_un in the scratch area at
 * scratch_off below SP, update regs SA/LEN, return 1. Passthrough: 0. */
static int translate_reg_sockaddr(pid_t pid, struct user_pt_regs *r,
                                  int sa_argi, int len_argi, const char *name,
                                  unsigned long long scratch_off) {
    unsigned long long sa = r->regs[sa_argi];
    unsigned long long len = r->regs[len_argi];
    if (sa == 0 || sa >= 0x800000000000ULL) return 0;
    if (len < 4 || len > 256) return 0;
    unsigned short fam = 0;
    errno = 0;
    long w = ptrace(PTRACE_PEEKDATA, pid, (void *)sa, NULL);
    if (w == -1 && errno) return 0;
    fam = (unsigned short)(w & 0xffff);
    if (fam != AF_UNIX) return 0;
    size_t off = offsetof(struct sockaddr_un, sun_path);
    char guest[108];
    size_t cap = (size_t)(len > off ? len - off : 0);
    if (cap > sizeof(guest) - 1) cap = sizeof(guest) - 1;
    if (cap == 0) return 0;
    ssize_t n = peek_str(pid, sa + off, guest, sizeof(guest));
    if (n <= 0) return 0;
    (void)cap;
    if (guest[0] == '\0') return 0; /* abstract: kernel-only namespace */
    if (guest_absolutize(pid, guest) != 0) return 0;
    char host[SP_PATH_MAX];
    if (!sp_translate(&g_cfg, guest, host)) return 0;
    size_t hl = strlen(host);
    if (hl + off + 1 > 108) return 0; /* doesn't fit sun_path; honest pass */
    if (hl >= SP_SCRATCH_CAP) return 0;
    struct sockaddr_un out;
    memset(&out, 0, sizeof(out));
    out.sun_family = AF_UNIX;
    memcpy(out.sun_path, host, hl + 1);
    long scratch = (long)(r->sp - scratch_off);
    errno = 0;
    if (ptrace(PTRACE_PEEKDATA, pid, (void *)scratch, NULL) == -1 && errno) return 0;
    if (poke_str(pid, scratch, (const char *)&out, off + hl + 1) != 0) return 0;
    r->regs[sa_argi] = (unsigned long long)scratch;
    r->regs[len_argi] = (unsigned long long)(off + hl + 1);
    if (g_debug) SP_TRACE("[%d] %s sockaddr_un %s -> %s\n", pid, name, guest, host);
    return 1;
}

typedef struct { long sysno; int sa_argi; int len_argi; const char *name; } sp_sock_rule;
static const sp_sock_rule SP_SOCK_RULES[] = {
    { 200 /*SYS_bind*/,    1, 2, "bind"    },
    { 203 /*SYS_connect*/, 1, 2, "connect" },
    { 206 /*SYS_sendto*/,  4, 5, "sendto"  },
};

/* sendmsg(211): msghdr.msg_name may carry a pathname sockaddr. Layout on
 * aarch64 LP64: msg_name(0..8), msg_namelen(8..12), then iov/control/... */
static int translate_reg_sendmsg(pid_t pid, struct user_pt_regs *r) {
    unsigned long long mptr = r->regs[1];
    if (mptr == 0 || mptr >= 0x800000000000ULL) return 0;
    errno = 0;
    long q0 = ptrace(PTRACE_PEEKDATA, pid, (void *)mptr, NULL);
    if (q0 == -1 && errno) return 0;
    unsigned long long name = (unsigned long long)q0;
    long q1 = ptrace(PTRACE_PEEKDATA, pid, (void *)(mptr + 8), NULL);
    if (q1 == -1 && errno) return 0;
    unsigned int namelen = (unsigned int)(q1 & 0xffffffffu);
    if (name == 0 || namelen < 4 || namelen > 256) return 0;
    /* reuse the sockaddr recognizer: family+path at `name` */
    unsigned short fam = (unsigned short)(ptrace(PTRACE_PEEKDATA, pid, (void *)name, NULL) & 0xffffu);
    if (fam != AF_UNIX) return 0;
    char guest[108];
    ssize_t n = peek_str(pid, name + offsetof(struct sockaddr_un, sun_path), guest, sizeof(guest));
    if (n <= 0 || guest[0] == '\0') return 0;
    if (guest_absolutize(pid, guest) != 0) return 0;
    char host[SP_PATH_MAX];
    if (!sp_translate(&g_cfg, guest, host)) return 0;
    size_t off = offsetof(struct sockaddr_un, sun_path);
    size_t hl = strlen(host);
    if (hl + off + 1 > 108) return 0;
    struct sockaddr_un out;
    memset(&out, 0, sizeof(out));
    out.sun_family = AF_UNIX;
    memcpy(out.sun_path, host, hl + 1);
    /* scratch plan: sockaddr_un at [base), msghdr copy at [base+128) */
    unsigned long long base = r->sp - 16000;
    errno = 0;
    if (ptrace(PTRACE_PEEKDATA, pid, (void *)base, NULL) == -1 && errno) return 0;
    if (poke_str(pid, base, (const char *)&out, off + hl) != 0) return 0;
    /* copy the 56-byte msghdr body */
    for (size_t o = 0; o < 56; o += sizeof(long)) {
        long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(mptr + o), NULL);
        if (w == -1 && errno) return 0;
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(base + 128 + o), (void *)w) == -1) return 0;
    }
    if (ptrace(PTRACE_POKEDATA, pid, (void *)(base + 128), (void *)(long)base) == -1) return 0;
    long newlen = (long)(off + hl + 1);
    /* msg_namelen: 32-bit field at +8; preserve upper 32 bits of its word */
    long q2 = ptrace(PTRACE_PEEKDATA, pid, (void *)(base + 128 + 8), NULL);
    if (q2 == -1 && errno) return 0;
    q2 = (q2 & ~0xffffffffLL) | (long)newlen;
    if (ptrace(PTRACE_POKEDATA, pid, (void *)(base + 128 + 8), (void *)q2) == -1) return 0;
    r->regs[1] = base + 128;
    if (g_debug) SP_TRACE("[%d] sendmsg msg_name %s -> %s\n", pid, guest, host);
    return 1;
}

/* Reverse-translate an AF_UNIX pathname in tracee memory AFTER the kernel
 * filled it. Pathname only shrinks (prefix is stripped), safe in place.
 * mp=0: addr is a bare sockaddr* with its socklen in the tracee at lenp;
 * mp=1: addr is an msghdr* (getsockname-style name+namelen at +0/+8). */
static void reverse_pending_addr(pid_t pid, tracee_t *t) {
    unsigned long long addr, lenp = 0;
    int mp = 0;
    if (t->rev_sysno == 206) {} /* not tracked */
    if (t->rev_sysno == 204 || t->rev_sysno == 207) { addr = t->rev_addr; lenp = t->rev_lenp; }
    else if (t->rev_sysno == 212) { addr = t->rev_addr; mp = 1; }
    else { t->rev_sysno = 0; return; }
    struct user_pt_regs r;
    struct iovec iov = { &r, sizeof(r) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) { t->rev_sysno = 0; return; }
    if ((long long)r.regs[0] < 0) { t->rev_sysno = 0; return; } /* syscall failed */
    if (mp) {
        errno = 0;
        long q0 = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
        if (q0 == -1 && errno) { t->rev_sysno = 0; return; }
        long q1 = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + 8), NULL);
        if (q1 == -1 && errno) { t->rev_sysno = 0; return; }
        addr = (unsigned long long)q0;
        lenp = t->rev_addr + 8;
    }
    if (addr == 0) { t->rev_sysno = 0; return; }
    errno = 0;
    long f = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
    if (f == -1 && errno) { t->rev_sysno = 0; return; }
    if ((unsigned short)(f & 0xffffu) != AF_UNIX) { t->rev_sysno = 0; return; }
    size_t off = offsetof(struct sockaddr_un, sun_path);
    char host[120];
    ssize_t n = peek_str(pid, addr + off, host, sizeof(host));
    if (n <= 0 || host[0] == '\0') { t->rev_sysno = 0; return; }
    char guest[SP_PATH_MAX];
    size_t gl = sp_reverse(&g_cfg, host, guest, sizeof(guest));
    if (!gl) { t->rev_sysno = 0; return; }
    /* sister discipline (ADR-0010): pathname reverse only *shrinks*. */
    if (poke_str(pid, addr + off, guest, gl) != 0) { t->rev_sysno = 0; return; }
    if (lenp) {
        errno = 0;
        long q2 = ptrace(PTRACE_PEEKDATA, pid, (void *)lenp, NULL);
        if (!(q2 == -1 && errno)) {
            unsigned long long newlen = (unsigned long long)(off + gl + 1);
            q2 = (q2 & ~0xffffffffLL) | (long)newlen;
            ptrace(PTRACE_POKEDATA, pid, (void *)lenp, (void *)q2);
        }
    }
    if (g_debug) SP_TRACE("[%d] %s reverse sun_path %s -> %s\n", pid,
                          t->rev_sysno == 204 ? "getsockname" : t->rev_sysno == 207 ? "recvfrom" : "recvmsg",
                          host, guest);
    t->rev_sysno = 0;
}

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
            /* H3: script-from-static — single shared serve (the same
             * composition the stub lane's lazy-attach vehicle uses):
             * [interp, opt?, script, rest] in scratch, loader chain for
             * dynamic interps, direct kernel exec for static interps. */
            if (sp_serve_script_exec(pid, &rex, path_argi, guest, host))
                ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovex);
            return;
        }
        /* static target (or unknown): plain single-string path translation */
        int changed = translate_reg_path(t, pid, &rex, path_argi, "execve");
        if (changed)
            ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovex);
        return;
    }

    /* AF_UNIX sockaddr arguments (forward) for supervisor-governed tracees */
    for (size_t i = 0; i < sizeof(SP_SOCK_RULES)/sizeof(*SP_SOCK_RULES); i++) {
        const sp_sock_rule *rule = &SP_SOCK_RULES[i];
        if (rule->sysno != sysno) continue;
        struct user_pt_regs rchk;
        struct iovec iovchk = { &rchk, sizeof(rchk) };
        if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
        /* place sock struct below the string arena (path poke lives in the
         * last-3KiB zone; sockets get a separate 256B corner) */
        if (translate_reg_sockaddr(pid, &rchk, rule->sa_argi, rule->len_argi,
                                   rule->name, SP_SCRATCH_BELOW_SP - 256))
            ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovchk);
        return;
    }

    /* sendmsg: msg_name through the 56-byte msghdr struct */
    if (sysno == 211 /*SYS_sendmsg*/) {
        struct user_pt_regs rchk;
        struct iovec iovchk = { &rchk, sizeof(rchk) };
        if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
        if (translate_reg_sendmsg(pid, &rchk))
            ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovchk);
        return;
    }

    /* reverse-capable entries: the kernel fills the sockaddr on EXIT; arm
     * pending state and consume it at the matching exit stop. */
    if (sysno == 204 /*getsockname*/ || sysno == 207 /*recvfrom*/) {
        struct user_pt_regs rchk;
        struct iovec iovchk = { &rchk, sizeof(rchk) };
        if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
        if (sysno == 204) { t->rev_addr = rchk.regs[1]; t->rev_lenp = rchk.regs[2]; }
        else              { t->rev_addr = rchk.regs[4]; t->rev_lenp = rchk.regs[5]; }
        t->rev_sysno = (t->rev_addr && t->rev_lenp) ? sysno : 0;
        /* no forward rewrite needed; fall through to default continuation */
    } else if (sysno == 212 /*recvmsg*/) {
        struct user_pt_regs rchk;
        struct iovec iovchk = { &rchk, sizeof(rchk) };
        if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
        t->rev_addr = rchk.regs[1];
        t->rev_sysno = t->rev_addr ? sysno : 0;
    }

    struct user_pt_regs rchk;
    struct iovec iovchk = { &rchk, sizeof(rchk) };
    int regs_valid = 0, regs_dirty = 0;
    for (size_t i = 0; i < sizeof(SP_PATH_RULES)/sizeof(*SP_PATH_RULES); i++) {
        const sp_path_rule *rule = &SP_PATH_RULES[i];
        if (rule->sysno != sysno) continue;
        if (!regs_valid) {
            if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iovchk) != 0) return;
            regs_valid = 1;
        }
        /* first pathname clause: dirfd-AT_FDCWD gated (a real dirfd
         * resolves in kernel space against an fd we don't mirror) */
        if (rule->dirfd_argi < 0 || (int)rchk.regs[rule->dirfd_argi] == AT_FDCWD_VAL) {
            sp_dbg_cwd("gov", pid);
            if (translate_reg_path(t, pid, &rchk, rule->path_argi, rule->name))
                regs_dirty = 1;
        }
        /* optional second pathname (linkat/renameat/renameat2 newpath) */
        if (rule->path2_argi >= 0 &&
            (rule->dirfd2_argi < 0 || (int)rchk.regs[rule->dirfd2_argi] == AT_FDCWD_VAL)) {
            if (translate_reg_path(t, pid, &rchk, rule->path2_argi, rule->name))
                regs_dirty = 1;
        }
    }
    if (regs_dirty) ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iovchk);
}


/* statx(2): bionic lacks the symbol; the supervisor itself never goes
 * through our interposer, so call the veneer directly. */
static int sp_statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *buf) {
    return (int)syscall(291, dirfd, path, flags, mask, buf);
}

/* ================= Seccomp user_notify fast path (ADR-0013) =============
 * When kernels allow it (5.0+ for NEW_LISTENER, 5.14+ for ADDFD), the
 * supervisor translates paths WITHOUT syscall-stop every call:
 *   - children carry a seccomp filter trapping only the hot translate set;
 *   - the supervisor listens on the filter's notify fd, reads args via
 *     process_vm_readv, performs the translated open/stat itself and gives
 *     the child the result (ADDFD injection for fd-returns;
 *     process_vm_writev for buf-returns);
 *   - execve/execveat/chdir stay ptrace-owned via SECCOMP_RET_TRACE
 *     (PTRACE_EVENT_SECCOMP single stop), io_uring-like Android-killed
 *     syscalls stay SIGSYS-signal-stops (ptrace swallows, see SP_EMULATE).
 * glibc-dynamic + LD_PRELOAD children do NOT install the filter: the
 * interposer already translates at PLT level and paying notify dispatch
 * would only double their work. The filter is installed in the fork()ed
 * child right before execve; the listener fd hops to the supervisor via
 * SCM_RIGHTS over a pre-fork socketpair (no pidfd requirement). */

#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE 1
#endif
// (seccomp_notif_addfd lives in bionic's linux/seccomp.h)
#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV (1UL << 5)
#endif
#ifndef SECCOMP_GET_NOTIF_SIZES
#define SECCOMP_GET_NOTIF_SIZES 3
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x40000000
#endif
#ifndef SECCOMP_IOCTL_NOTIF_ID_VALID
#define SECCOMP_IOCTL_NOTIF_ID_VALID 0x40082102
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static int g_notify = 0;           /* 1 = install+serve the filter */
static int g_notify_fd = -1;       /* listener fd owned by supervisor */
static int g_notify_addfd = -1;    /* ADDFD ioctl availability (live-probed) */

#define SP_OFT(...) do { if (g_debug) fprintf(stderr, "[notify] " __VA_ARGS__); } while(0)

/* syscall traps: hot translation set for aarch64 */
#define AUDIT_ARCH_AARCH64_OWN 0xC00000B7u

static long sp_seccomp(unsigned int op, unsigned int flags, void *args) {
    return syscall(277 /*__NR_seccomp (aarch64)*/, op, flags, args);
}

static int sp_notify_install(void) {
    /* Serviced entries only: ops whose answer is a VALUE we can give back
     * (fd injection / errno code), never a memory write into the tracee.
     * stat-family (79/291/78) stay ptrace-served for statics: writing
     * structs into the guest's stack across sanitization + glibc
     * internals proved heap-corrupting under Debug-vs-noDebug race
     * conditions; the self-pipe event loop makes those stops cheap. */
    static const int traps[] = {
        56 /*openat*/, 437 /*openat2*/,
        48 /*faccessat*/,
        34 /*mkdirat*/, 35 /*unlinkat*/, 33 /*mknodat*/,
        53 /*fchmodat*/, 54 /*fchownat*/, 55 /*fchown*/, 88 /*utimensat*/,
        36 /*symlinkat*/, 37 /*linkat*/, 38 /*renameat*/,
        276 /*renameat2*/,
        /* accept(202)/accept4(242) deliberately NOT trapped: Android TRAPs
         * the legacy accept(2) nr for the whole process (Xiaomi-HyperOS
         * device observed); accept4 is allowed. We therefore PIVOT the
         * x8 register 202->242 at the SIGSYS stop/sigaction instead of
         * serving via the listener. */
        /* stat-family (79/291/78) DELIBERATELY ABSENT here: this filter is
         * the SHARED one put on TRACEME-lane statics + shadow dynamic
         * guests — dynamic guests' stats are already interposer-
         * translated and statics' stats are ptrace-served; trapping them
         * into the notify listener cost musl `find /usr` ~4x (measured
         * 328ms vs 81ms). The notify-statics lane (ADR-0016) needs them
         * and has them — in the stub's OWN filter, not this one. */
        200 /*bind*/, 203 /*connect*/, 206 /*sendto*/, 211 /*sendmsg*/,
    };
    const int ntr = (int)(sizeof(traps)/sizeof(traps[0]));
    /* layout:
       [0]   LD nr
       [1..ntr] JEQ traps[i-1], jt = (ntr+2 - i), jf = 0      (on match -> UN)
       [ntr+1] RET ALLOW
       [ntr+2] RET USER_NOTIF
       arch check: LD arch / JEQ aarch64 1,0 / KILL */
    struct sock_filter prog[4 + 32 + 2];
    int p = 0;
    prog[p++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                             offsetof(struct seccomp_data, arch));
    prog[p++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                             AUDIT_ARCH_AARCH64_OWN, 1, 0);
    prog[p++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL);
    prog[p++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                             offsetof(struct seccomp_data, nr));
    for (int i = 0; i < ntr; i++) {
        /* instruction index of this JEQ = 4 + i (0-based within prog) */
        int here = 4 + i;
        int target = 4 + ntr + 1;             /* USER_NOTIF index */
        unsigned char jt = (unsigned char)(target - (here + 1));
        prog[p++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                 (unsigned)traps[i], jt, 0);
    }
    prog[p++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    prog[p++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF);
    struct sock_fprog fp = { (unsigned short)p, prog };
    /* plain NEW_LISTENER: TSYNC needs every thread sync-capable (EINVAL on
     * this box), WAIT_KILLABLE_RECV is unavailable here. Fork-inherited in
     * a freshly-spawned single-thread child anyway. */
    long fd = sp_seccomp(SECCOMP_SET_MODE_FILTER,
                         SECCOMP_FILTER_FLAG_NEW_LISTENER, &fp);
    return (int)fd;
}

/* ioctl availability probe: fake-id -ADDFD returns EINVAL, ENOSYS means kernel
 * lacks ADDFD (pre-5.14). */
static int sp_notify_probe_addfd(void) {
    if (g_notify_fd < 0) return 0;
    struct seccomp_notif_addfd af;
    memset(&af, 0, sizeof(af));
    af.id = ~0ULL; af.srcfd = -1;
    int rc = ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_ADDFD, &af);
    if (rc < 0 && errno == ENOSYS) return 0;
    return 1;
}

static ssize_t sp_vm_read(pid_t pid, void *buf, size_t n, unsigned long long addr) {
    struct iovec lw = { buf, n }, rw = { (void *)addr, n };
    ssize_t rc = process_vm_readv(pid, &lw, 1, &rw, 1, 0);
    if (rc < 0 && errno == ENOSYS) {
        /* kernel without process_vm_readv: last-resort via ptrace peek */
        return -1;
    }
    return rc;
}
static ssize_t sp_vm_write(pid_t pid, const void *buf, size_t n, unsigned long long addr) {
    struct iovec lw = { (void *)buf, n }, rw = { (void *)addr, n };
    return process_vm_writev(pid, &lw, 1, &rw, 1, 0);
}

static int sp_notify_read_str(pid_t pid, unsigned long long addr,
                              char *dst, size_t cap) {
    if (addr == 0 || addr >= 0x800000000000ULL) return -1;
    size_t i = 0;
    while (i < cap) {
        char chunk[256];
        size_t want = (cap - i) < sizeof(chunk) ? (cap - i) : sizeof(chunk);
        ssize_t rc = sp_vm_read(pid, chunk, want, addr + i);
        if (rc <= 0) return -1;
        for (size_t k = 0; k < (size_t)rc; k++) {
            dst[i++] = chunk[k];
            if (chunk[k] == '\0') return (int)i;
        }
    }
    dst[cap - 1] = '\0';
    return (int)i;
}

static int sp_notify_continue(struct seccomp_notif_resp *resp) {
    resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    resp->error = 0; resp->val = 0;
    return 0;
}

/* fd-injection path: translate then open in SUPERVISOR, then ADDFD into the
 * tracee's table so the child sees the right local fd. */
static void sp_notify_reply_open(unsigned long long id,
                                 struct seccomp_notif_resp *resp,
                                 const char *path, int flags, mode_t mode) {
    int lf = open(path, flags, mode);
    if (lf < 0) {
        resp->error = -errno; resp->val = 0; resp->flags = 0;
        SP_OFT("openat reply err %d for %s\n", errno, path);
        return;
    }
    struct seccomp_notif_addfd af;
    memset(&af, 0, sizeof(af));
    af.id = id; af.flags = 0; af.srcfd = lf;
    af.newfd_flags = (flags & O_CLOEXEC) ? O_CLOEXEC : 0;
    long nfd = ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_ADDFD, &af);
    close(lf);
    if (nfd < 0) {
        resp->error = -errno; resp->val = 0; resp->flags = 0;
        SP_OFT("ADDFD fail %d\n", errno);
        return;
    }
    resp->error = 0; resp->val = (uint64_t)nfd; resp->flags = 0;
    SP_OFT("openat -> host fd=%ld childval %s\n", nfd, path);
}

/* Resolve a notify-provided guest path into a host path:
 *   absolute -> rootfs/bind translation (normal sp_translate)
 *   relative -> join against the tracee's REAL cwd (read /proc/PID/cwd):
 *               the tracee's cwd is already a host path because the
 *               supervisor's chdir/execve always ran through translation.
 * Returns 1 when host[] holds a usable path. */
static int sp_notify_hostpath(pid_t pid, const char *guest, char *host, size_t cap) {
    if (!guest || !host || cap == 0) return 0;
    if (guest[0] == '/') return sp_translate(&g_cfg, guest, host) ? 1 : 0;
    char cw[SP_PATH_MAX];
    {
        char pdir[64];
        snprintf(pdir, sizeof(pdir), "/proc/%d/cwd", pid);
        ssize_t cl = readlink(pdir, cw, sizeof(cw) - 1);
        if (cl <= 0) return 0;
        cw[cl] = '\0';
    }
    int n = snprintf(host, cap, "%s/%s", cw, guest);
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

/* ---- AF_UNIX notify-serve via pidfd_getfd (ADR-0013 M3) ----------------
 * bind/connect/sendto/sendmsg carry a sockaddr_un the kernel must see in
 * HOST form, but the operations must affect the TRACEE's socket, not a
 * supervisor one. pidfd_getfd duplicates the tracee's socket fd into the
 * supervisor's table *referring to the same struct file*, so operating on
 * the duplicate is semantically identical to the tracee doing it itself.
 * Requires kernel >=5.6 (pidfd_getfd); probed lazily once, falling back
 * to CONTINUE (ptrace scratch path covers governed tracees; dynamic
 * guests already translated in the interposer). */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#define SP_PIDFD_CACHE 16
#define SP_SOCK_IO_CAP 32768
static pid_t g_pidfd_pid[SP_PIDFD_CACHE];
static int   g_pidfd_fd[SP_PIDFD_CACHE];
static int   g_pidfd_avail = -1; /* -1 unprobed, 0 unavailable, 1 ok */

static int sp_pidfd_for(pid_t pid) {
    if (g_pidfd_avail == 0) return -1;
    for (int i = 0; i < SP_PIDFD_CACHE; i++)
        if (g_pidfd_pid[i] == pid && g_pidfd_fd[i] >= 0) return g_pidfd_fd[i];
    int pf = (int)syscall(SYS_pidfd_open, pid, 0u);
    if (pf < 0) { g_pidfd_avail = 0; return -1; }
    g_pidfd_avail = 1;
    for (int i = 0; i < SP_PIDFD_CACHE; i++) {
        if (g_pidfd_pid[i] == 0 || g_pidfd_fd[i] < 0) {
            g_pidfd_pid[i] = pid; g_pidfd_fd[i] = pf; return pf;
        }
    }
    /* full: evict slot 0 (cheap LRU) */
    close(g_pidfd_fd[0]);
    g_pidfd_pid[0] = pid; g_pidfd_fd[0] = pf;
    return pf;
}

static int sp_notify_sock_dup(pid_t pid, int childfd) {
    int pf = sp_pidfd_for(pid);
    if (pf < 0) return -1;
    return (int)syscall(SYS_pidfd_getfd, pf, childfd, 0u);
}

/* Read the tracee's sockaddr at (addr,alen); if it is a translatable
 * AF_UNIX *pathname* (absolute, non-abstract), produce the host form in
 * `out` and return its length; else 0. */
static socklen_t sp_notify_unix_to_host(pid_t pid, unsigned long long addr,
                                        unsigned long long alen,
                                        struct sockaddr_un *out) {
    if (alen > sizeof(struct sockaddr_un) || alen < 3) return 0;
    unsigned char buf[sizeof(struct sockaddr_un)];
    memset(buf, 0, sizeof(buf));
    if (sp_vm_read(pid, buf, alen, addr) != (ssize_t)alen) return 0;
    struct sockaddr_un *su = (struct sockaddr_un *)buf;
    if (su->sun_family != AF_UNIX) return 0;
    if (su->sun_path[0] != '/') return 0; /* abstract or unnamed: host is fine */
    char guest[SP_PATH_MAX], host[SP_PATH_MAX];
    size_t gl = strnlen(su->sun_path, sizeof(su->sun_path));
    if (gl == 0 || gl >= sizeof(guest)) return 0;
    memcpy(guest, su->sun_path, gl + 1);
    if (!sp_notify_hostpath(pid, guest, host, sizeof host)) return 0;
    size_t hl = strlen(host);
    if (hl >= sizeof(out->sun_path)) return 0;
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    memcpy(out->sun_path, host, hl + 1);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + hl + 1);
}

/* ---- iovec gather for sendto/sendmsg notify-serve ---- */
/* SELinux denies hardlinks on /data/data/.../files. A SYMLINK fallback is
 * fatal for the link(tmp -> target)+unlink(tmp) journal pattern (observed:
 * git "not a valid object"); materialize the link target as a byte-for-byte
 * copy instead — unlink on the source then harms nothing. */
/* proot-shape .l2s fallback: rename content to a hidden rootfs/.l2s file,
 * src+dst become symlinks. Required for link-then-write patterns (glibc
 * locale-archive: tmp is LINKED before its records are written via fd). */
static int sp_link_fallback_l2s(const char *src, const char *dst) {
    if (!g_rootfs) return -1;
    static unsigned long l2s_n = 0;
    char hid[SP_PATH_MAX], tmp[SP_PATH_MAX];
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    struct stat lst;
    if (lstat(src, &lst) != 0) return -1;
    if (S_ISLNK(lst.st_mode)) {
        ssize_t rn = readlink(src, tmp, sizeof(tmp) - 1);
        if (rn <= 0) return -1;
        tmp[rn] = 0;
        if (strstr(tmp, "/.l2s/.l2s.")) {
            snprintf(hid, sizeof hid, "%s", tmp);
            goto have_hidden;
        }
    }
    snprintf(hid, sizeof hid, "%s/.l2s/.l2s.%s.%lx%lx", g_rootfs, base,
             (unsigned long)getpid(), (unsigned long)++l2s_n);
    if (mkdir((snprintf(tmp, sizeof tmp, "%s/.l2s", g_rootfs), tmp), 0700) != 0 && errno != EEXIST)
        return -1;
    if (rename(src, hid) != 0) return -1;
    if (symlink(hid, src) != 0) { rename(hid, src); return -1; }
have_hidden:
    unlink(dst);
    if (symlink(hid, dst) != 0) return -1;
    return 0;
}

static int sp_link_fallback_copy(const char *src, const char *dst) {
    int si = open(src, O_RDONLY);
    if (si < 0) return -1;
    struct stat sst;
    if (fstat(si, &sst) != 0) { close(si); errno = EIO; return -1; }
    int di = open(dst, O_WRONLY | O_CREAT | O_EXCL, sst.st_mode & 07777);
    if (di < 0) { close(si); return -1; }
    char buf[65536];
    ssize_t n;
    while ((n = read(si, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(di, buf + off, (size_t)(n - off));
            if (w < 0) { close(si); close(di); unlink(dst); return -1; }
            off += w;
        }
    }
    fsync(di);
    close(si);
    int rc = close(di);
    if (n < 0 || rc != 0) { unlink(dst); errno = EIO; return -1; }
    return 0;
}

/* (ADR-0016 T3) execve notifications are the ONLY register-level work
 * the pure-notify lane cannot do: rewriting x0 (pathptr) — and for a
 * PT_INTERP target the full argv/envp surgery — needs ptrace. So we
 * borrow it for one syscall's lifetime: ATTACH -> wait stop -> GETREGSET
 * -> rewrite (regs + memory) -> SETREGSET -> DETACH. The tracee stays
 * seccomp-parked the whole time; the caller's CONTINUE response is what
 * actually resumes the (now-rewritten) execve. Attach on a parked-
 * notify task is legal; the request outlives the attach/detach pair.
 * Return 1 on successful rewrite, 0 otherwise. */
static int sp_notify_lazy_exec_rewrite(pid_t pid, const char *guest,
                                       const char *host, int cls) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        if (g_debug) SP_TRACE("[notify] lazy-attach %d failed errno=%d\n", pid, errno);
        return 0;
    }
    int stt = 0;
    if (waitpid(pid, &stt, __WALL) < 0 || !WIFSTOPPED(stt)) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 0;
    }
    struct user_pt_regs r;
    struct iovec iov = { &r, sizeof(r) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 0;
    }
    int ok = 0;
    if (cls == 2) {
        /* cls==2 (script): compose [interp, opt?, script, rest] and route
         * through the same loader chain — mirrors the legacy lane's
         * block at the execve-at-51 site (the stub lane has no
         * interposer, so native CONT just lands the script on the host
         * kernel's shebang resolution: wrong libc world, observed
         * ENOENT vs the host's /bin/bash wiring). */
        ok = sp_serve_script_exec(pid, &r, 0, guest, host);
    } else if (cls == 0) {
        /* static→dynamic: full loader-chain surgery (peek/poke work now
         * that we're attached). rewrite writes regs; SETREGSET pushes. */
        ok = sp_rewrite_exec_to_loader(NULL, pid, &r, host, guest, /*path_argi=*/0, /*depth=*/0);
    } else {
        /* static→static with a longer host path: park the host string in
         * the below-SP scratch arena and point x0 there. */
        unsigned long long base = (unsigned long long)r.sp - SP_EXEC_SCRATCH_BELOW_SP;
        errno = 0;
        if (ptrace(PTRACE_PEEKDATA, pid, (void *)base, NULL) != -1 || errno == 0) {
            if (poke_str(pid, base, host, strlen(host)) == 0) {
                r.regs[0] = base;
                ok = 1;
            }
        }
    }
    if (ok) ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    if (g_debug) SP_TRACE("[notify] lazy-attach %d cls=%d rewrite=%s host=%s\n",
                          pid, cls, ok ? "OK" : "fail", host);
    return ok;
}

/* H3 (single serve-core): the ONE implementation of "script exec from
 * a static process". Both supervisors (legacy TRACEME lane at execve-stop
 * and the stub lane's lazy-attach vehicle) compose the SAME chain:
 *   argv = [interp-guest, opt?, script-guest, rest...]
 * then split by interp kind: dynamic -> sp_rewrite_exec_to_loader,
 * static -> kernel execve of the interp host path directly.
 * The in-guest parallel lives in sprout_preload.c (chains inside the
 * victim address space, unavoidable). Requires pid attached + regs
 * fetched. path_argi: 0=execve, 1=execveat.  Returns: 1 = regs
 * rewritten (caller SETREGSETs), 0 = honest bail. */
static int sp_serve_script_exec(pid_t pid, struct user_pt_regs *r,
                                int path_argi, const char *guest, const char *host) {
    char ibuf[SP_PATH_MAX], obuf[SP_PATH_MAX];
    ibuf[0] = '\0'; obuf[0] = '\0';
    /* own the shebang parse (kernel one-token tail semantics):
     * classify_host_file(host, interp, opt) fills guest-spelled heads. */
    (void)classify_host_file(host, ibuf, obuf);
    if (!ibuf[0]) return 0; /* not a shebang after all */

    /* resolve the interpreter (guest spelling); absolute straight, else
     * walk SPROUT_GUEST_PATH (same rule set as the legacy lane). */
    char cand[SP_PATH_MAX];
    int found = 0;
    if (ibuf[0] == '/' || strchr(ibuf, '/') != NULL) {
        snprintf(cand, sizeof(cand), "%s", ibuf);
        char hc0[SP_PATH_MAX];
        const char *h0 = sp_translate(&g_cfg, cand, hc0) ? hc0 : cand;
        found = (access(h0, X_OK) == 0);
    } else {
        const char *gp = getenv("SPROUT_GUEST_PATH");
        if (!gp) gp = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        char pbuf[4096];
        snprintf(pbuf, sizeof(pbuf), "%s", gp);
        for (char *d = strtok(pbuf, ":"); d && !found; d = strtok(NULL, ":")) {
            char cc[SP_PATH_MAX];
            int n = snprintf(cc, sizeof(cc), "%s/%s", *d ? d : ".", ibuf);
            if (n < 0 || (size_t)n >= sizeof(cc)) continue;
            char hc[SP_PATH_MAX];
            const char *h = sp_translate(&g_cfg, cc, hc) ? hc : cc;
            if (access(h, X_OK) == 0) { snprintf(cand, sizeof(cand), "%s", cc); found = 1; }
        }
    }
    if (!found) return 0;
    char hcan[SP_PATH_MAX];
    if (!sp_translate(&g_cfg, cand, hcan)) return 0;
    char ib2[SP_PATH_MAX], ob2[SP_PATH_MAX]; ib2[0] = '\0'; ob2[0] = '\0';
    int cls2 = classify_host_file(hcan, ib2, ob2);
    if (cls2 == 2) return 0; /* nested scripts: depth-honest bail */

    /* compose scratch: [arr pointers][strings]: [cand, opt?, guest, rest..] */
    unsigned long long base = (unsigned long long)r->sp - SP_EXEC_SCRATCH_BELOW_SP;
    errno = 0;
    if (ptrace(PTRACE_PEEKDATA, pid, (void *)base, NULL) == -1 && errno) return 0;
    unsigned long long orig_argv = r->regs[path_argi + 1];
    unsigned long long orig_envp = r->regs[path_argi + 2];
    unsigned long long sc = base + (unsigned long long)(SP_EXEC_MAX_ARGS * 8);
    unsigned long long end = base + SP_EXEC_SCRATCH_BELOW_SP;
    unsigned long long arr_ptrs[SP_EXEC_MAX_ARGS];
    int na = 0;
    const char *head[3]; int nh = 0;
    head[nh++] = cand;
    if (obuf[0]) head[nh++] = obuf;
    head[nh++] = guest;
    for (int i = 0; i < nh && na < SP_EXEC_MAX_ARGS - 1; i++) {
        size_t sl = strlen(head[i]);
        if (sc + sl + 16 >= end) return 0;
        if (poke_str(pid, sc, head[i], sl) != 0) return 0;
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
    arr_ptrs[na] = 0;
    for (int i = 0; i <= na; i++) {
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(base + (unsigned long long)i * 8),
                   (void *)arr_ptrs[i]) != 0) return 0;
    }
    r->regs[path_argi + 1] = base;   /* synthetic argv array */
    r->regs[path_argi + 2] = orig_envp;
    if (cls2 == 0) {
        /* dynamic interp -> full loader chain around [interp, ...] */
        return sp_rewrite_exec_to_loader(NULL, pid, r, hcan, cand, path_argi, 1);
    }
    /* static interp (legacy lane used to bail here — ENOEXEC matched
     * proot only in the no-interp case; wiring it is strictly better
     * than leaving the deepest corner dark): point the pathname arg at
     * the interp HOST path, kernel execs it natively. */
    size_t hl = strlen(hcan);
    if (sc + hl + 16 >= end) return 0;
    if (poke_str(pid, sc, hcan, hl) != 0) return 0;
    r->regs[path_argi] = sc;
    return 1; /* caller: SETREGSET + CONTINUE */
}

/* ------------------------------------------------------------------ */
/* proot-parity fakes for /proc entries Android locks for untrusted apps */
/* ------------------------------------------------------------------ */
/* /proc/stat and /proc/loadavg are EACCES for any uid on new Android
 * even though htop-like tools expect them. Serve a synthetic file the
 * way proot does: small parseable content, served via a temporary fd. */
static int sp_fake_proc_cpu_count(void) {
    char b[64];
    int f = open("/sys/devices/system/cpu/possible", O_RDONLY);
    if (f < 0) f = open("/sys/devices/system/cpu/present", O_RDONLY);
    if (f < 0) return 1;
    ssize_t n = read(f, b, sizeof(b) - 1);
    close(f);
    if (n <= 0) return 1;
    b[n] = 0;
    /* format: "0-7" or "0,2,4-5" — count by ranges */
    int total = 0;
    char *p = b;
    while (*p) {
        int lo = -1, hi = -1;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (!*p) break;
        lo = (int)strtol(p, &p, 10);
        hi = lo;
        if (*p == '-') { p++; hi = (int)strtol(p, &p, 10); }
        if (hi >= lo) total += hi - lo + 1;
    }
    return total > 0 ? total : 1;
}

static int sp_fake_proc_serve(unsigned long long id, const char *which,
                              struct seccomp_notif_resp *resp) {
    int is_stat = strcmp(which, "/proc/stat") == 0;
    char tmp_t[SP_PATH_MAX];
    /* NB: TMPDIR in this process is the GUEST's (/tmp policy from the
     * launch plan) — for supervisor-side temp files it must stay a
     * HOST-writable dir; try TMPDIR first, fall back. */
    const char *cands[2] = { getenv("TMPDIR"), "/data/data/com.termux/files/usr/tmp" };
    int f = -1;
    for (int i = 0; i < 2 && f < 0; i++) {
        if (!cands[i]) continue;
        snprintf(tmp_t, sizeof(tmp_t), "%s/sprout-fakeproc-XXXXXX", cands[i]);
        f = mkstemp(tmp_t);
    }
    if (f < 0) { SP_OFT("[notify] fakeproc mkstemp errno=%d\n", errno); return 0; }
    unlink(tmp_t);
    char buf[4096];
    int n = 0;
    if (is_stat) {
        int ncpu = sp_fake_proc_cpu_count();
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "cpu 0 0 0 0 0 0 0 0 0 0\n");
        for (int i = 0; i < ncpu && n < (int)sizeof(buf) - 96; i++)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "cpu%d 0 0 0 0 0 0 0 0 0 0\n", i);
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
            "intr 0\nctxt 0\nbtime 0\nprocesses 0\nprocs_running 0\nprocs_blocked 0\nsoftirq 0 0 0 0 0 0 0 0 0 0\n");
    } else { /* /proc/loadavg */
        n = snprintf(buf, sizeof(buf), "0.00 0.00 0.00 1/512 12345\n");
    }
    if (write(f, buf, (size_t)n) != n || lseek(f, 0, SEEK_SET) < 0) { close(f); return 0; }
    struct seccomp_notif_addfd af; memset(&af, 0, sizeof(af));
    af.id = id; af.flags = 0; af.srcfd = f;
    long nfd = ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_ADDFD, &af);
    close(f);
    if (nfd < 0) { SP_OFT("[notify] fakeproc ADDFD errno=%d\n", errno); return 0; }
    SP_OFT("[notify] fakeproc %s -> fd=%ld\n", which, nfd);
    resp->error = 0; resp->val = (uint64_t)nfd; resp->flags = 0;
    return 1;
}

static void sp_notify_serve_one(pid_t pid, unsigned long long nr,
                                unsigned long long *args,
                                unsigned long long id,
                                struct seccomp_notif_resp *resp) {
    char guest[SP_PATH_MAX], host[SP_PATH_MAX];
    resp->flags = 0;
    switch (nr) {
    case 221: { /* execve(path,argv,envp) — lazy-attach rewrite (ADR-0016 T3) */
        if (sp_notify_read_str(pid, args[0], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (guest[0] != '/') { sp_notify_continue(resp); return; } /* relative: tracee cwd is host-real */
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        char interp[SP_PATH_MAX]; interp[0] = '\0';
        char opt[SP_PATH_MAX]; opt[0] = '\0';
        int cls = classify_host_file(host, interp, opt);
        if (cls == 1 || cls == 0) {
            /* static→static: in-place string edit when it fits; otherwise
             * and for static→dynamic (cls==0, the PT_INTERP chain) the
             * pointer structure needs registers → lazy ATTACH vehicle. */
            size_t gl = strlen(guest), hl = strlen(host);
            if (cls == 1 && strcmp(host, guest) == 0) { sp_notify_continue(resp); return; }
            if (cls == 1 && hl <= gl) {
                if (sp_vm_write(pid, host, hl + 1, args[0]) == (ssize_t)(hl + 1)) {
                    sp_notify_continue(resp); return;
                }
            }
            if (sp_notify_lazy_exec_rewrite(pid, guest, host, cls) == 1) {
                sp_notify_continue(resp); return;
            }
            /* rewrite failed: ENOSYS beats a half-translated native exec
             * that would start an ungoverned loader. */
            resp->error = -ENOSYS; resp->flags = 0; resp->val = 0; return;
        }
        if (cls == 2) {
            /* script: same rewrite machinery via the lazy-attach vehicle
             * (stub lane has no interposer — native CONT would land the
             * shebang on the host kernel: wrong libc world / ENOENT). */
            if (sp_notify_lazy_exec_rewrite(pid, guest, host, cls) == 1) {
                sp_notify_continue(resp); return;
            }
            resp->error = -ENOENT; resp->flags = 0; resp->val = 0; return;
        }
        /* -1 (unknown): native CONT. */
        sp_notify_continue(resp);
        return;
    }
    case 56: { /* openat(dirfd,path,flags,mode) */
        int dirfd = (int)args[0];
        int flags = (int)args[2];
        if (dirfd != -100 /*AT_FDCWD*/) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        /* RELATIVE fast path: the tracee's cwd is already host-real, so a
         * native retry (CONT) lands on exactly the file we'd serve via
         * readlink+ADDFD — without the supervisor round-trip. */
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        /* proot-parity fakes: Android locks /proc/stat + /proc/loadavg
         * for untrusted apps even when guests expect them — synthesize. */
        if (g_debug && guest[0] == '/' && guest[1] == 'p') SP_TRACE("[notify] openat56 guest='%s'\n", guest);
        if ((strcmp(guest, "/proc/stat") == 0 || strcmp(guest, "/proc/loadavg") == 0)
            && sp_fake_proc_serve(id, guest, resp) == 1) return;
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        sp_notify_reply_open(id, resp, host, flags, (mode_t)args[3]);
        return; }
    case 437: { /* openat2(dirfd,path,how*,size) */
        int dirfd = (int)args[0];
        size_t hsz = (size_t)args[3];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (hsz != 24) { sp_notify_continue(resp); return; }
        struct open_how { unsigned long long flags, mode, resolve; } how;
        if (sp_vm_read(pid, &how, sizeof(how), args[2]) != sizeof(how)) { sp_notify_continue(resp); return; }
        if (how.resolve) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        int lf = open(host, (int)how.flags, (mode_t)how.mode);
        if (lf < 0) { resp->error = -errno; return; }
        /* ADDFD not embedded in how; reuse same ioctl */
        struct seccomp_notif_addfd af;
        memset(&af, 0, sizeof(af));
        af.id = id; af.flags = SECCOMP_ADDFD_FLAG_SETFD; af.srcfd = lf;
        long nfd = ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_ADDFD, &af);
        close(lf);
        if (nfd < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = (uint64_t)nfd;
        return; }
    case 79: { if (getenv("SPROT_NOTIFY_NO_NOSTAT")) { sp_notify_continue(resp); return; } /* newfstatat(dirfd,path,st*,flags) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        int sflags = (int)args[3];
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        struct stat st;
        int rc = (sflags & 0x100 /*AT_SYMLINK_NOFOLLOW*/) ? lstat(host, &st) : stat(host, &st);
        if (rc < 0) { resp->error = -errno; return; }
        if (sp_vm_write(pid, &st, sizeof(st), args[2]) != sizeof(st)) { sp_notify_continue(resp); return; }
        resp->error = 0; resp->val = 0;
        SP_OFT("fstatat ok %s\n", host);
        return; }
    case 48: { /* faccessat(dirfd,path,mode,flags) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        /* relative: native retry sees the same cwd; "" hits bionic SIGSYS
         * and the musl EMULATE table swallows it the same way either way */
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        int rc = access(host, (int)args[2]);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0;
        return; }
    case 291: { if (getenv("SPROT_NOTIFY_NO_NOSTATX")) { sp_notify_continue(resp); return; } /* statx(dirfd,path,flags,mask,buf) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        struct statx sx;
        memset(&sx, 0, sizeof(sx));
        int rc = sp_statx(-100, host, (int)args[2], (unsigned)args[3], &sx);
        if (rc < 0) { resp->error = -errno; return; }
        if (sp_vm_write(pid, &sx, sizeof(sx), args[4]) != sizeof(sx)) { sp_notify_continue(resp); return; }
        resp->error = 0; resp->val = 0;
        return; }
    case 78: { if (getenv("SPROT_NOTIFY_NO_NOREADLINK")) { sp_notify_continue(resp); return; } /* readlinkat(dirfd,path,buf,size) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        char lnk[SP_PATH_MAX];
        ssize_t n = readlink(host, lnk, sizeof(lnk) - 1);
        if (n < 0) { resp->error = -errno; return; }
        if ((size_t)n > (size_t)args[3]) n = (ssize_t)args[3];
        if (n > 0 && sp_vm_write(pid, lnk, (size_t)n, args[2]) != n) { sp_notify_continue(resp); return; }
        resp->error = 0; resp->val = (uint64_t)n;
        return; }
    /* mutation family: supervisor performs with translated paths */
    case 34: { /* mkdirat(dirfd,path,mode) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        int rc = mkdir(host, (mode_t)args[2]);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0;
        return; }
    case 35: { /* unlinkat(dirfd,path,flags) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        int rc = (args[2] & 0x200 /*AT_REMOVEDIR*/) ? rmdir(host) : unlink(host);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0;
        return; }
    case 53: { /* fchmodat(dirfd,path,mode,flags) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        /* args[3] observed WILD (7fe1… junk) in some guests — glibc
         * __chmod(fchmodat) reaches the trap with x3 un-zeroed: the kernel
         * tolerates any flags value ≠ AT_-mask, but OUR forwarded bionic
         * fchmodat ALSO fails EINVAL. Sanitize: forward only legal AT_ bits. */
        int fch_flags = (int)args[3] & 0x100 /*AT_SYMLINK_NOFOLLOW only legal bit*/;
        int rc = fchmodat(-100, host, (mode_t)args[2], fch_flags);
        if (rc < 0) {
            fprintf(stderr, "[notify] fchmodat serve pid=%d '%s' mode=%04o flags=%llx -> %d (%d)\n",
                    pid, host, (unsigned)args[2] & 07777, (unsigned long long)args[3], rc, errno);
            resp->error = -errno; return;
        }
        resp->error = 0; resp->val = 0; return; }
    case 54: { /* fchownat(dirfd,path,u,g,flags) */
        /* proot -0 parity: under FAKEROOT the guest believes it is uid=0, so
         * the interposer fakes chown-family success (sprout_preload.c ~1051).
         * The notify lane MUST mirror that: calling the real fchownat here
         * relays the kernel EPERM for uid=10372 and breaks tar/dpkg/apt
         * (which check the result and abort extraction). */
        if (getenv("SPROUT_FAKEROOT")) { resp->error = 0; resp->val = 0; return; }
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        int rc = fchownat(-100, host, (uid_t)args[2], (gid_t)args[3], (int)args[4]);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0; return; }
    case 55: { /* fchown(fd,u,g) — FAKEROOT: fake success (proot -0 parity;
                 * see case 54). No path translation needed. */
        if (getenv("SPROUT_FAKEROOT")) { resp->error = 0; resp->val = 0; return; }
        sp_notify_continue(resp); return; }
    case 88: { /* utimensat(dirfd,path,times,flags) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (args[1] == 0) { sp_notify_continue(resp); return; } /* NULL path: fd-based futimens */
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        struct timespec ts[2];
        const struct timespec *tsp = NULL;
        if (args[2] != 0) {
            if (sp_vm_read(pid, ts, sizeof(ts), args[2]) != (ssize_t)sizeof(ts)) { sp_notify_continue(resp); return; }
            tsp = ts;
        }
        int rc = utimensat(-100, host, tsp, (int)args[3]);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0;
        SP_OFT("utimensat ok %s\n", host);
        return; }
    case 33: { /* mknodat(dirfd,path,mode,dev) */
        int dirfd = (int)args[0];
        if (dirfd != -100) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[1], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, guest, host, sizeof host)) { sp_notify_continue(resp); return; }
        int rc = mknod(host, (mode_t)args[2], (dev_t)args[3]);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0; return; }
    case 36: { if (getenv("SPROT_NOTIFY_NO_LINKSY")) { sp_notify_continue(resp); return; } /* symlinkat(target, newdirfd, linkpath) */
        int newdirfd = (int)args[1];
        if (newdirfd != -100) { sp_notify_continue(resp); return; }
        char target[SP_PATH_MAX];
        if (sp_notify_read_str(pid, args[0], target, sizeof(target)) <= 0) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[2], guest, sizeof(guest)) <= 0) { sp_notify_continue(resp); return; }
        if (guest[0] != '/') { sp_notify_continue(resp); return; }
        char lhost[SP_PATH_MAX];
        if (!sp_notify_hostpath(pid, guest, lhost, sizeof lhost)) { sp_notify_continue(resp); return; }
        int rc = symlink(target, lhost);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0; return; }
    case 37: { if (getenv("SPROT_NOTIFY_NO_LINKSY")) { sp_notify_continue(resp); return; } /* linkat(olddirfd, oldpath, newdirfd, newpath, flags) */
        int od = (int)args[0], nd = (int)args[2];
        if (od != -100 || nd != -100) { sp_notify_continue(resp); return; }
        char oldp[SP_PATH_MAX], oh[SP_PATH_MAX], nh[SP_PATH_MAX];
        if (sp_notify_read_str(pid, args[1], oldp, sizeof(oldp)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, oldp, oh, sizeof oh)) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[3], oldp, sizeof(oldp)) <= 0) { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, oldp, nh, sizeof nh)) { sp_notify_continue(resp); return; }
        int rc = linkat(-100, oh, -100, nh, (int)args[4]);
        /* SELinux denies hardlinks under /data/data/.../files: copy first
         * (journal pattern safety), symlink as last resort (proot-shaped
         * --link2symlink surface-answer). */
        if (rc < 0 && (errno == EPERM || errno == EACCES) && getenv("SPROUT_LINK2SYMLINK")) {
            if (sp_link_fallback_l2s(oh, nh) == 0 || sp_link_fallback_copy(oh, nh) == 0) rc = 0;
            else rc = symlinkat(oh, -100, nh);
        }
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0; return; }
    case 38: { /* renameat(olddirfd, oldpath, newdirfd, newpath) */
        int od = (int)args[0], nd = (int)args[2];
        if (od != -100 || nd != -100) { sp_notify_continue(resp); return; }
        char rp[SP_PATH_MAX], oh[SP_PATH_MAX], nh[SP_PATH_MAX];
        if (sp_notify_read_str(pid, args[1], rp, sizeof(rp)) <= 0) { sp_notify_continue(resp); return; }
        if (rp[0] != '/') { sp_notify_continue(resp); return; }   /* relative: native cwd is host-real */
        if (!sp_notify_hostpath(pid, rp, oh, sizeof oh)) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[3], rp, sizeof(rp)) <= 0) { sp_notify_continue(resp); return; }
        if (rp[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, rp, nh, sizeof nh)) { sp_notify_continue(resp); return; }
        int rc = rename(oh, nh);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0; return; }
    case 276: { /* renameat2(olddirfd, oldpath, newdirfd, newpath, flags) */
        int od = (int)args[0], nd = (int)args[2];
        if (od != -100 || nd != -100) { sp_notify_continue(resp); return; }
        char rp[SP_PATH_MAX], oh[SP_PATH_MAX], nh[SP_PATH_MAX];
        if (sp_notify_read_str(pid, args[1], rp, sizeof(rp)) <= 0) { sp_notify_continue(resp); return; }
        if (rp[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, rp, oh, sizeof oh)) { sp_notify_continue(resp); return; }
        if (sp_notify_read_str(pid, args[3], rp, sizeof(rp)) <= 0) { sp_notify_continue(resp); return; }
        if (rp[0] != '/') { sp_notify_continue(resp); return; }
        if (!sp_notify_hostpath(pid, rp, nh, sizeof nh)) { sp_notify_continue(resp); return; }
        int rc = (int)syscall(276, -100, oh, -100, nh, (unsigned)args[4]);
        if (rc < 0) { resp->error = -errno; return; }
        resp->error = 0; resp->val = 0; return; }
    /* remaining: non-AT_FDCWD variants fall through (CONTINUE) */

    case 200: /* bind(fd,addr,alen) */
    case 203: { /* connect(fd,addr,alen) */
        struct sockaddr_un hsa;
        socklen_t hlen = sp_notify_unix_to_host(pid, args[1], args[2], &hsa);
        if (hlen == 0) { sp_notify_continue(resp); return; }
        int d = sp_notify_sock_dup(pid, (int)args[0]);
        if (d < 0) { sp_notify_continue(resp); return; }
        int rc = (nr == 200)
               ? bind(d, (struct sockaddr *)&hsa, hlen)
               : connect(d, (struct sockaddr *)&hsa, hlen);
        int e = errno;
        close(d);
        if (rc < 0) { resp->error = -e; return; }
        resp->error = 0; resp->val = 0;
        return; }
    case 206: { /* sendto(fd,buf,len,flags,addr,alen) */
        if (args[4] == 0) { sp_notify_continue(resp); return; }
        struct sockaddr_un hsa;
        socklen_t hlen = sp_notify_unix_to_host(pid, args[4], args[5], &hsa);
        if (hlen == 0) { sp_notify_continue(resp); return; }
        unsigned long long len = args[2];
        if (len > SP_SOCK_IO_CAP) { sp_notify_continue(resp); return; }
        char *buf = malloc(len ? len : 1);
        if (!buf) { sp_notify_continue(resp); return; }
        if (len && sp_vm_read(pid, buf, len, args[1]) != (ssize_t)len) {
            free(buf); sp_notify_continue(resp); return;
        }
        int d = sp_notify_sock_dup(pid, (int)args[0]);
        if (d < 0) { free(buf); sp_notify_continue(resp); return; }
        ssize_t rc = sendto(d, buf, len, (int)args[3],
                            (struct sockaddr *)&hsa, hlen);
        int e = errno;
        close(d); free(buf);
        if (rc < 0) { resp->error = -e; return; }
        resp->error = 0; resp->val = (long long)rc;
        return; }
    case 211: { /* sendmsg(fd,msg,flags) */
        struct sp_umsg { unsigned long long name, pad0; unsigned long long iov, iovlen, ctrl, ctrllen, flags; } mh;
        if (sp_vm_read(pid, &mh, sizeof(mh), args[1]) != sizeof(mh)) { sp_notify_continue(resp); return; }
        if (!mh.name) { sp_notify_continue(resp); return; }
        if (mh.ctrl && mh.ctrllen) { sp_notify_continue(resp); return; } /* SCM creds: leave to kernel path */
        struct sockaddr_un hsa;
        socklen_t hlen;
        {
            /* msg_namelen sits in the low 32 bits of the second u64 slot */
            unsigned long long namelen = mh.pad0 & 0xffffffffull;
            hlen = sp_notify_unix_to_host(pid, mh.name, namelen, &hsa);
        }
        if (hlen == 0) { sp_notify_continue(resp); return; }
        if (mh.iovlen > 16) { sp_notify_continue(resp); return; }
        struct iovec iov[16];
        unsigned long long total = 0;
        char *pool = NULL, *p = NULL;
        if (mh.iovlen) {
            struct { unsigned long long base, len; } iv[16];
            if (sp_vm_read(pid, iv, mh.iovlen * sizeof(iv[0]), mh.iov) != (ssize_t)(mh.iovlen * sizeof(iv[0]))) { sp_notify_continue(resp); return; }
            for (unsigned long long k = 0; k < mh.iovlen; k++) {
                total += iv[k].len;
                if (total > SP_SOCK_IO_CAP) { sp_notify_continue(resp); return; }
            }
        }
        pool = malloc(total ? total : 1);
        if (!pool) { sp_notify_continue(resp); return; }
        p = pool;
        if (mh.iovlen) {
            struct { unsigned long long base, len; } iv[16];
            if (sp_vm_read(pid, iv, mh.iovlen * sizeof(iv[0]), mh.iov) != (ssize_t)(mh.iovlen * sizeof(iv[0]))) { free(pool); sp_notify_continue(resp); return; }
            for (unsigned long long k = 0; k < mh.iovlen; k++) {
                if (iv[k].len && sp_vm_read(pid, p, iv[k].len, iv[k].base) != (ssize_t)iv[k].len) { free(pool); sp_notify_continue(resp); return; }
                iov[k].iov_base = p; iov[k].iov_len = iv[k].len;
                p += iv[k].len;
            }
        }
        int d = sp_notify_sock_dup(pid, (int)args[0]);
        if (d < 0) { free(pool); sp_notify_continue(resp); return; }
        struct msghdr sm;
        memset(&sm, 0, sizeof(sm));
        sm.msg_name = &hsa; sm.msg_namelen = hlen;
        sm.msg_iov = iov; sm.msg_iovlen = mh.iovlen;
        ssize_t rc = sendmsg(d, &sm, (int)args[2]);
        int e = errno;
        close(d); free(pool);
        if (rc < 0) { resp->error = -e; return; }
        resp->error = 0; resp->val = (long long)rc;
        return; }
    default:
        sp_notify_continue(resp);
        return;
    }
}

static unsigned long long g_notify_n = 0, g_notify_serv = 0, g_notify_cont = 0, g_notify_err = 0, g_notify_poller = 0;

static void sp_notify_pump(void);
int sp_notify_parent_recv(int sockfd, pid_t child);
void sp_notify_child_install_and_send(int sockfd);

/* (ADR-0016) Notify-statics lane runtime: fork+exec sprout-stub with
 * NO ptrace, then serve its filter's user-notify requests and reap. */
static int run_notify_statics(const char *stub, int argc, char **argv) {
    int np[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, np) != 0) { perror("ns socketpair"); return -1; }
    pid_t child = fork();
    if (child < 0) { perror("ns fork"); close(np[0]); close(np[1]); return -1; }
    if (child == 0) {
        close(np[0]);
        /* The STUB installs the user-notify filter itself ONLY after it
         * has mapped the guest (install_last-before-jump): its loader
         * phase must stay unfiltered, and a stub-side filter survives its
         * mmap+jump because no real exec resets filters. The supervisor
         * steals the listener over this same socketpair (fdnum + ACK
         * protocol, identical to sp_notify_child_install_and_send). */
        unsetenv("SPROUT_NS_NOFILTER");
        char dec[16];
        snprintf(dec, sizeof(dec), "%d", np[1]);
        setenv("SPROUT_STUB_SOCK", dec, 1);
        /* stub contract: [guest0, target_host, args...]. argv[2] is the
         * guest program (host path + argv0 spelling, same as the ptrace
         * lane's execve(argv[2], &argv[2], ...)). */
        int cnt = argc - 1; /* items after the supervisor's '--' plus dup */
        char **nargv = (char **)calloc((size_t)cnt + 1, sizeof(char *));
        if (!nargv) _exit(127);
        nargv[0] = argv[2];
        nargv[1] = argv[2];
        for (int i = 3; i < argc; i++) nargv[i - 1] = argv[i];
        nargv[cnt] = NULL;
        if (g_debug) { int arc = access(stub, X_OK);
          char db[1024]; int dl = 0;
          dl += snprintf(db + dl, 1000 - dl, "[child] argc=%d cnt=%d access=%d errno=%d ", argc, cnt, arc, errno);
          for (int i = 0; i <= cnt && dl < 900; i++) dl += nargv[i] ? snprintf(db + dl, 1000 - dl, "[%d]=%s ", i, nargv[i]) : 0;
          dl += snprintf(db + dl, 1000 - dl, "| stub=%s\n", stub);
          (void)!write(2, db, (size_t)dl); }
        execve(stub, nargv, environ);
        { char eb[128]; int el = snprintf(eb, sizeof eb, "[notify] stub execve('%s') failed errno=%d\n", stub, errno); if (el > (int)sizeof eb) el = (int)sizeof eb; (void)!write(2, eb, (size_t)el); }
        _exit(127);
    }
    close(np[1]);
    g_notify_fd = sp_notify_parent_recv(np[0], child);
    if (g_notify_fd < 0) {
        fprintf(stderr, "[notify] statics listener handshake failed\n");
        return -1;
    }
    g_notify_addfd = sp_notify_probe_addfd();
    if (g_debug)
        fprintf(stderr, "[notify] statics listener=%d addfd=%d child=%d\n",
                g_notify_fd, g_notify_addfd, child);


    int status = 0, exited = 0;
    for (;;) {
        struct pollfd pfd = { g_notify_fd, POLLIN, 0 };
        int prc = poll(&pfd, 1, 128);
        if (pfd.revents & POLLIN) sp_notify_pump();
        else if (g_debug && prc == 0) fprintf(stderr, "[notify] poll tick (no events) revents=%x\n", pfd.revents);
        for (;;) {
            int st;
            /* LEAD with the stub child: it's long-lived — reaping it
             * FIRST pulls state in before short siblings flood the pool. */
            pid_t w = waitpid(child, &st, WNOHANG);
            if (w <= 0) w = waitpid(-1, &st, WNOHANG);
            if (w <= 0) break;
            if (w == child) { status = st; exited = 1; }
            if (g_debug) fprintf(stderr, "[notify] wait4 pid=%d status=%x (%s rc=%d sig=%d)\n",
                w, st, WIFEXITED(st) ? "EXITED" : (WIFSIGNALED(st) ? "SIGNALED" : "STOPPED"),
                WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : -1);
        }
        if (exited) {
            for (;;) {
                struct pollfd q = { g_notify_fd, POLLIN, 0 };
                if (poll(&q, 1, 0) <= 0 || !(q.revents & POLLIN)) break;
                sp_notify_pump();
            }
            if (WIFSIGNALED(status) && (g_debug || getenv("SPROUT_DEBUG"))) {
                /* postmortem: what did the child die doing? (statics lane
                 * had silent SIGBUS reports from scripts/static execs) */
                if (ptrace(PTRACE_ATTACH, child, NULL, NULL) == 0) {
                    int pst;
                    waitpid(child, &pst, __WALL);
                    struct user_pt_regs rr;
                    struct iovec riov = { &rr, sizeof(rr) };
                    if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &riov) == 0)
                        fprintf(stderr, "[notify] postmortem pid=%d sig=%d pc=0x%llx sp=0x%llx x0=%llu x8=%llu\n",
                                child, WTERMSIG(status), rr.pc, rr.sp, rr.regs[0], rr.regs[8]);
                    siginfo_t si;
                    if (ptrace(PTRACE_GETSIGINFO, child, NULL, &si) == 0)
                        fprintf(stderr, "[notify] siginfo: addr=%p code=%d\n", si.si_addr, si.si_code);
                    ptrace(PTRACE_DETACH, child, NULL, NULL);
                }
            }
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 1;
        }
    }
}

/* SIGCHLD -> self-pipe: ptrace stops deliver SIGCHLD; this lets one poll()
 * wake on both ptrace events and user-notify requests. */
static int g_spipe[2] = { -1, -1 };
static void sp_sigchld_wake(int s) { (void)s; char b = 1; if (g_spipe[1] >= 0) (void)write(g_spipe[1], &b, 1); }
static void sp_sigchld_init(void) {
    if (pipe2(g_spipe, O_NONBLOCK | O_CLOEXEC) == 0) {
        struct sigaction sa; memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sp_sigchld_wake; sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;   /* CAUTION: NO SA_NOCLDSTOP — stops must wake too */
        sigaction(SIGCHLD, &sa, NULL);
    }
}

static void sp_notify_pump(void) {
    struct seccomp_notif req;
    memset(&req, 0, sizeof(req));
    if (ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_RECV, &req) < 0) { g_notify_err++; return; }
    g_notify_n++;
    /* H1 TOCTOU guard #1: between RECV and serve-begin, the traced task
     * may have exited (or its filter been replaced) — ID_VALID answers
     * 'is this notification id still outstanding'. A dead id makes every
     * subsequent process_vm_read + serve decision a no-target lie, and on
     * SEND the kernel would match a pid-REUSED request. Skip it all. */
    if (ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_ID_VALID, &req.id) != 0) {
        g_notify_err++;
        SP_OFT("recv id=%llu stale at entry — dropped\n", (unsigned long long)req.id);
        return;
    }
    struct seccomp_notif_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.id = req.id;
    resp.val = 0; resp.error = 0; resp.flags = 0;
    pid_t tid = req.pid;
    SP_OFT("recv pid=%d nr=%llu args=[%llx,%llx,%llx,%llx]\n",
           tid, (unsigned long long)req.data.nr,
           (unsigned long long)req.data.args[0], (unsigned long long)req.data.args[1],
           (unsigned long long)req.data.args[2], (unsigned long long)req.data.args[3]);
    sp_notify_serve_one(tid, req.data.nr, req.data.args, req.id, &resp);
    if (resp.flags & SECCOMP_USER_NOTIF_FLAG_CONTINUE) g_notify_cont++; else g_notify_serv++;
    /* H1 TOCTOU guard #2: serve_one read tracee memory + maybe wrote a
     * scratch arena; if the task died mid-serve the response now carries
     * a decoded answer for a vanished request (and SEND could mis-address
     * a pid reused by the kernel since). Re-validate before SEND. The
     * remaining CONTINUE-string-mutation window (a sibling thread edits
     * the pathname between our read and the kernel retry) is inherent to
     * the CONT-retry model — proot has the identical race; both are
     * documented, and fd-result serves over ADDFD are unaffected. */
    if (ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_ID_VALID, &req.id) != 0) {
        g_notify_err++;
        SP_OFT("recv id=%llu stale after serve — response dropped\n", (unsigned long long)req.id);
        return;
    }
    ioctl(g_notify_fd, SECCOMP_IOCTL_NOTIF_SEND, &resp);
}

void sp_notify_child_install_and_send(int sockfd) {
    { struct seccomp_notif_sizes szp; memset(&szp, 0, sizeof(szp));
      if (sp_seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &szp) < 0) {
        SP_OFT("child: GET_NOTIF_SIZES errno=%d\n", errno);
        close(sockfd);
        return;
      }
      if (szp.seccomp_notif < 64) { SP_OFT("child: sizes too small\n"); close(sockfd); return; } }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) { SP_OFT("child: NNP errno=%d\n", errno); close(sockfd); return; }
    int fd = sp_notify_install();
    if (fd < 0) { SP_OFT("child: install errno=%d (%s)\n", errno, strerror(errno)); close(sockfd); return; }
    /* pass the listener's fd NUMBER to the supervisor over a plain
     * write(2) — sendmsg(2) would be caught by our own freshly-installed
     * filter (ADR-0013 M3 traps bind/connect/sendto/sendmsg), deadlocking
     * child (trapped, awaiting response) against parent (awaiting the
     * SCM message that never comes). The parent reconstructs a usable
     * descriptor with pidfd_getfd() (we are its ptracer; permitted).
     * The fd stays non-CLOEXEC so it survives the upcoming execve(): the
     * notify queue must outlive the installing task. */
    unsigned char nb[4] = {
        (unsigned char)(fd & 0xff), (unsigned char)((fd >> 8) & 0xff),
        (unsigned char)((fd >> 16) & 0xff), (unsigned char)((fd >> 24) & 0xff) };
    if (write(sockfd, nb, 4) != 4) { SP_OFT("child: fdnum write failed\n"); close(sockfd); return; }
    /* rendezvous: block until the supervisor finished pidfd_getfd (or
     * gave up). Without this a short-lived tracee can execve+exit before
     * the parent steals the listener, racing notify into a fallback-or-
     * broken mixed mode. */
    unsigned char ack;
    (void)read(sockfd, &ack, 1);
    close(sockfd);
}

int sp_notify_parent_recv(int sockfd, pid_t child) {
    unsigned char nb[4];
    ssize_t rc = read(sockfd, nb, 4);
    if (rc != 4) { close(sockfd); return -1; }
    int cfd = (int)(nb[0] | (nb[1] << 8) | (nb[2] << 16) | ((unsigned)nb[3] << 24));
    int pf = (int)syscall(SYS_pidfd_open, child, 0u);
    if (pf < 0) return -1;
    int fd = (int)syscall(SYS_pidfd_getfd, pf, cfd, 0u);
    close(pf);
    /* ACK the child either way: it drops to ptrace-only on fd<0 and the
     * child must be released even in the failure case. */
    { unsigned char ack = 0x41; (void)!write(sockfd, &ack, 1); }
    close(sockfd);
    if (fd < 0) return -1;
    return fd;
}

int sp_notify_probe(void) {
    struct seccomp_notif_sizes sz;
    memset(&sz, 0, sizeof(sz));
    if (sp_seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sz) < 0) return 0;
    return (sz.seccomp_notif >= 64) ? 1 : 0;
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
    /* musl-dynamic (kind 3) gets the shadow treatment too: its ldso-chain
     * interposer covers the PLT set exactly like glibc's, so per-syscall
     * PTRACE_SYSCALL is pure waste in pipe-flood profiles (bench: musl
     * cmdsubst-pipe 0.76x -> target >=1x). SPROUT_MUSL_NOSHADOW reverts. */
    g_shadow = getenv("SPROUT_SHADOW") != NULL &&
               (g_libc_kind != SP_LIBC_MUSL || !getenv("SPROUT_MUSL_NOSHADOW"));
    /* Interposed grandchildren of this supervisor (preload chain) learn
     * from SPROUT_SUPERVISED that static execs need NO fresh sprout-ptrace
     * chain — exec them directly and the ptrace estate reclassifies on
     * PTRACE_EVENT_EXEC into full translation mode. */
    setenv("SPROUT_SUPERVISED", "1", 1);

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

    /* user-notify selection: env opt-out (SPROUT_USER_NOTIFY=0), else
     * auto-probe. glibc-dynamic+LD_PRELOAD stays shadow (interposer
     * already translates); statics + musl + Go own the notify filter when
     * available. */
    {   const char *u = getenv("SPROUT_USER_NOTIFY");
        if (u && strcmp(u, "0") == 0) g_notify = 0;
        else                        g_notify = sp_notify_probe();
    }
    if (g_debug)
        fprintf(stderr, "[notify] mode=%s\n", g_notify ? "user-notify" : "ptrace-only");

    /* (ADR-0016) notify-statics lane: kind 1/2 guests (static, Go-static)
     * launch through the freestanding sprout-stub instead of
     * TRACEME+ptrace: the stub's in-guest SIGSYS emulation replaces the
     * ptrace signal-swallow and its own filter takes the role of serving
     * path ops through user-notify. ptrace survives ONLY as a lazy-attach
     * register-rewrite vehicle for execve (T3). SPROUT_NOTIFY_STATICS=0
     * forces the classic lane. */
    {   const char *gk = getenv("SPROUT_GUEST_KIND");
        int gkind = gk ? atoi(gk) : 0;
        const char *nso = getenv("SPROUT_NOTIFY_STATICS");
        /* 2026-08-12-L: stub lane RE-ENABLED as default. #74 root-caused
         * + fixed: the in-stub SIGSYS emulator used glibc-userland
         * ucontext offsets (sigset 128), 8 bytes early vs the kernel's
         * rt_sigframe (sigset 8) — the fake-success path did SP+=4
         * instead of PC+=4, returning threads with a misaligned sp;
         * glibc-statics then SIGBUS'd mid-__tls_init_tp (fa==pc in
         * the RW-anon stack). Fixed offsets measured empirically on
         * Android-16's frame; verified: step/onecall/exec_script/
         * sp_asm + 28MB Go-static cloudflared + both batteries 81/81
         * green under SPROUT_NOTIFY_STATICS=1. =0 forces the classic
         * ptrace lane (kept as the escape hatch). */
        int want_ns = g_notify && (gkind == 1 || gkind == 2) && !(nso && strcmp(nso, "0") == 0);
        if (g_debug) fprintf(stderr, "[notify] lane-probe: g_notify=%d gkind=%d want=%d\n", g_notify, gkind, want_ns);
        if (want_ns) {
            char stub[SP_PATH_MAX];
            const char *se = getenv("SPROUT_STUB_PATH");
            if (se && *se) snprintf(stub, sizeof(stub), "%s", se);
            else {
                const char *slash = strrchr(argv[0], '/');
                int dl = slash ? (int)(slash - argv[0]) : 0;
                snprintf(stub, sizeof(stub), "%.*s/sprout-stub", dl, argv[0]);
            }
            if (access(stub, X_OK) == 0) {
                if (g_debug) fprintf(stderr, "[notify] stub usable: %s\n", stub);
                int nrc = run_notify_statics(stub, argc, argv);
                if (nrc >= 0) return nrc;   /* lane terminal */
                if (g_debug) fprintf(stderr, "[notify] statics lane unavailable -> ptrace\n");
            } else if (g_debug) {
                fprintf(stderr, "[notify] sprout-stub not found (%s) -> ptrace\n", stub);
            }
        }
    }

    int notify_pair[2] = { -1, -1 };
    if (g_notify) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, notify_pair) != 0) {
            perror("notify socketpair"); g_notify = 0;
        }
    }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        if (g_notify) {
            close(notify_pair[0]);
            sp_notify_child_install_and_send(notify_pair[1]);
        }
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            const char mom[] = "[ptrace] TRACEME failed\n"; (void)!write(2, mom, sizeof mom - 1);
            _exit(127);
        }
        if (guest_preload) setenv("LD_PRELOAD", guest_preload, /*override=*/1);
        if (g_debug) { char mb[160]; int ml = snprintf(mb, sizeof mb, "[ptrace] child pre-exec target='%s'\n", argv[2]); (void)!write(2, mb, (size_t)ml); }
        execve(argv[2], &argv[2], environ);
        { char eb[128]; int el = snprintf(eb, sizeof eb, "[ptrace] child execve('%s') failed: errno=%d (%s)\n", argv[2], errno, strerror(errno)); if (el > (int)sizeof eb) el = (int)sizeof eb; (void)!write(2, eb, (size_t)el); }
        _exit(127);
    }
    if (g_notify) {
        close(notify_pair[1]);
        g_notify_fd = sp_notify_parent_recv(notify_pair[0], child);
        if (g_notify_fd < 0) {
            g_notify = 0;
            if (g_debug) fprintf(stderr, "[notify] listener handshake failed -> ptrace-only\n");
        } else {
            g_notify_addfd = sp_notify_probe_addfd();
            if (g_debug) fprintf(stderr, "[notify] listener=%d addfd=%d\n", g_notify_fd, g_notify_addfd);
        }
    }

    /* First-stop acquisition (see ADR-0013): the child can already be
     * blocked inside a user-notify request *before* its ptrace stop
     * materializes (notify waits in the syscall; ptrace stops only surface
     * on the return-to-userland path). waitpid-blocking would deadlock:
     * pump and wait together until the initial exec stop shows. The raw
     * waitpid SWALLOWS the first stop (never policy-dispatched). */
    int st0 = -1;
    if (g_notify && g_notify_fd >= 0) {
        for (int iter = 0; iter < 4000 && st0 < 0; iter++) {
            struct pollfd pfd = { g_notify_fd, POLLIN, 0 };
            (void)poll(&pfd, 1, 2);
            if (pfd.revents & POLLIN) sp_notify_pump();
            pid_t w0 = waitpid(child, &st0, WNOHANG);
            if (w0 == child) break;
            if (w0 < 0) { perror("waitpid"); return 1; }
        }
    } else if (waitpid(child, &st0, 0) < 0) { perror("waitpid"); return 1; }
    /* Fast-exit race: the main child may already be gone (short-lived
     * first exec failing under timing distortion, or instant-exit paths);
     * waitpid then reports an EXIT instead of a stop. Never call
     * SETOPTIONS on a dead tracee — propagate its status. */
    if (st0 >= 0 && WIFEXITED(st0)) {
        if (g_debug) fprintf(stderr, "[ptrace] main child exited before attach: rc=%d\n", WEXITSTATUS(st0));
        return WEXITSTATUS(st0);
    }
    if (st0 >= 0 && WIFSIGNALED(st0))
        return 128 + WTERMSIG(st0);
    if (ptrace(PTRACE_SETOPTIONS, child, 0,
               PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
               PTRACE_O_TRACEEXEC | PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) < 0) {
        if (errno == ESRCH) {
            /* lost the race after all: reap whatever status is left */
            int st1 = 0;
            if (waitpid(child, &st1, WNOHANG) == child) {
                if (WIFEXITED(st1)) return WEXITSTATUS(st1);
                if (WIFSIGNALED(st1)) return 128 + WTERMSIG(st1);
            }
            return 0;
        }
        perror("PTRACE_SETOPTIONS"); return 1;
    }
    { tracee_t *rt = find_or_add(child); if (rt && g_shadow) rt->shadow = 1; }
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0) { perror("PTRACE_SYSCALL"); return 1; }

    int status = 0;
    /* Event-loop discipline: BOTH ptrace stops and user-notify requests
     * must be serviced at stop-rate, not poll-tick rate. SIGCHLD wakes a
     * self-pipe (ptrace stops deliver SIGCHLD), so one poll() covers both
     * sources and wait4 drains *all* queued events per wake. The old 200ms
     * poll cadence turned N ptrace stops into N*200ms of stall. */
    sp_sigchld_init();
    for (;;) {
        struct pollfd pfds[2];
        int nfd = 0;
        int ni = -1, si = -1;
        if (g_notify) { ni = nfd; pfds[nfd].fd = g_notify_fd; pfds[nfd].events = POLLIN; nfd++; }
        if (g_spipe[0] >= 0) { si = nfd; pfds[nfd].fd = g_spipe[0]; pfds[nfd].events = POLLIN; nfd++; }
        if (nfd > 0) (void)poll(pfds, nfd, 500);
        if (ni >= 0 && (pfds[ni].revents & POLLIN)) {
            sp_notify_pump();
            for (;;) {
                struct pollfd q = { g_notify_fd, POLLIN, 0 };
                if (poll(&q, 1, 0) <= 0 || !(q.revents & POLLIN)) break;
                sp_notify_pump();
            }
        }
        if (si >= 0 && (pfds[si].revents & POLLIN)) { char buf[64]; while (read(g_spipe[0], buf, sizeof buf) > 0) ; }
        int nevents = 0; (void)nevents;
        for (;;) {
            pid_t w = waitpid(-1, &status, WNOHANG);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == ECHILD) goto done;
                perror("waitpid"); goto done;
            }
            if (w == 0) break;
            nevents++;
            SP_OFT("wait4 pid=%d status=%x\n", w, status);
        if (WIFEXITED(status)) {
            if (w == child) {
                if (g_debug) fprintf(stderr, "[ptrace] MAIN-CHILD %d EXITED rc=%d (status=0x%x)\n", w, WEXITSTATUS(status), status);
                if (g_notify_n || g_notify_poller)
                    SP_OFT("notify stats: recv=%llu served=%llu cont=%llu recverr=%llu poller=%llu\n",
                           g_notify_n, g_notify_serv, g_notify_cont, g_notify_err, g_notify_poller);
                return WEXITSTATUS(status);
            }
            sp_tracee_free(w);
            goto cont;
        }
        if (WIFSIGNALED(status) && w == child) {
            if (g_debug) fprintf(stderr, "[ptrace] MAIN-CHILD %d SIGNALED sig=%d (status=0x%x)\n", w, WTERMSIG(status), status);
            return 128 + WTERMSIG(status);
        }
        if (WIFSIGNALED(status)) {
            if (w != child) sp_tracee_free(w);
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
        if (g_debug) {
            struct user_pt_regs rx; struct iovec iovx = { &rx, sizeof(rx) };
            if (ptrace(PTRACE_GETREGSET, w, (void *)NT_PRSTATUS, &iovx) == 0) {
                SP_OFT("stop pid=%d sig=%d eNR=%lld x0=%llx sp=%llx pc=%llx\n",
                       w, sig, (long long)rx.regs[8], rx.regs[0], rx.sp, (unsigned long long)rx.pc);
                if (sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
                    char mapfile[64];
                    snprintf(mapfile, sizeof(mapfile), "/proc/%d/maps", w);
                    FILE *mf = fopen(mapfile, "r");
                    if (mf) {
                        char line[1024];
                        unsigned long long pc = (unsigned long long)rx.pc;
                        while (fgets(line, sizeof(line), mf)) {
                            unsigned long long lo, hi;
                            if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && pc >= lo && pc < hi) {
                                unsigned long long off; char mod[512] = "?";
                                if (sscanf(line, "%*x-%*x %*s %llx %*s %*s %511[^\n]",
                                           &off, mod) < 1) off = 0;
                                SP_OFT("crash mod: %s +%llx\n", mod, pc - lo + off);
                                break;
                            }
                        }
                        fclose(mf);
                    }
                }
            }
        }
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
                long emul_ret = 0;
                for (size_t i = 0; i < sizeof(SP_EMULATE_BASE)/sizeof(*SP_EMULATE_BASE) && !emulated; i++) {
                    if ((long)r.regs[8] == SP_EMULATE_BASE[i].sys) { emulated = 1; emul_ret = SP_EMULATE_BASE[i].ret; }
                }
                if (!emulated && use_musl_extra) {
                    for (size_t i = 0; i < sizeof(SP_EMULATE_MUSL_EXTRA)/sizeof(*SP_EMULATE_MUSL_EXTRA); i++) {
                        if ((long)r.regs[8] == SP_EMULATE_MUSL_EXTRA[i]) { emulated = 1; emul_ret = 0; break; }
                    }
                }
                /* fakeroot parity with proot: under -0 an identity or
                 * group-set procedure is a no-op success. */
                if (emulated && (long)r.regs[8] >= 143 && (long)r.regs[8] <= 152 && getenv("SPROUT_FAKEROOT"))
                    emul_ret = 0;
                if (emulated && (long)r.regs[8] == 159 && getenv("SPROUT_FAKEROOT"))
                    emul_ret = 0;
                if (!emulated && (long)r.regs[8] == 202 /*accept*/) {
                    /* ANDROID policy TRAPs legacy accept(2) on this class
                     * of process (Xiaomi HyperOS observed; tmux-server's
                     * libevent died here as pure SIGSYS), while accept4(242)
                     * is allowed. Pivot x8 202->242, clear flags x3=0, and
                     * re-execute the very same svc (pc untouched). glibc's
                     * accept(fd,addr,len) == accept4(fd,addr,len,0). */
                    emulated = 1;
                    r.regs[8] = 242;
                    r.regs[3] = 0;
                    ptrace(PTRACE_SETREGSET, w, (void *)NT_PRSTATUS, &iov);
                    if (g_debug)
                        fprintf(stderr, "[ptrace] %d SIGSYS pivot: accept→accept4 (x8=242) pc=%llx\n",
                                w, (unsigned long long)r.pc);
                    ptrace(t->shadow ? PTRACE_CONT : PTRACE_SYSCALL, w, 0, NULL);
                    continue;
                }
                if (emulated) {
                    r.regs[0] = (unsigned long long)(long long)emul_ret;
                    ptrace(PTRACE_SETREGSET, w, (void *)NT_PRSTATUS, &iov);
                    if (g_debug)
                        fprintf(stderr, "[ptrace] %d SIGSYS swallowed: sysno=%llu emulated -> %ld (fakeroot=%s)\n",
                                w, r.regs[8], emul_ret, getenv("SPROUT_FAKEROOT") ? getenv("SPROUT_FAKEROOT") : "-");
                }
            }
            /* swallow or deliver */
            ptrace(t->shadow ? PTRACE_CONT : PTRACE_SYSCALL, w, 0, emulated ? (void *)0 : (void *)(long)sig);
            continue;
        }

        if (sig == (SIGTRAP | 0x80)) {  /* syscall-stop */
            if (t->shadow) goto cont;   /* shadow tracees: no syscall stops; defensive */
            struct user_pt_regs r;
            struct iovec iov = { &r, sizeof(r) };
            if (ptrace(PTRACE_GETREGSET, w, (void *)NT_PRSTATUS, &iov) != 0) goto cont;
            sp_syscall_view v = sp_view_syscall(w, &r);
            if (v.op == 2) {           /* GSI EXIT stop: no entry-policy */
                /* SELinux-denied hardlink (EPERM/EACCES on /data/data/.../files):
                 * answer with a journal-safe content copy (parity with the
                 * interposer + notify fallbacks). */
                if (v.nr == 37 /*linkat*/ && getenv("SPROUT_LINK2SYMLINK") &&
                    ((long)r.regs[0] == -1 /*-EPERM*/ || (long)r.regs[0] == -13 /*-EACCES*/)) {
                    char oh[SP_PATH_MAX], nh[SP_PATH_MAX];
                    if (peek_str(w, r.regs[1], oh, sizeof(oh)) > 0 &&
                        peek_str(w, r.regs[3], nh, sizeof(nh)) > 0 &&
                        (sp_link_fallback_l2s(oh, nh) == 0 || sp_link_fallback_copy(oh, nh) == 0)) {
                        r.regs[0] = 0;
                        ptrace(PTRACE_SETREGSET, w, (void *)NT_PRSTATUS, &iov);
                    }
                }
                if (t->rev_sysno) reverse_pending_addr(w, t);
                goto cont;
            }
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
                    if (t->rev_sysno) reverse_pending_addr(w, t);
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
                    /* re-evaluate shadow for the new image: only glibc-
                     * dynamic-interposed (kind 0) stays shadowed; statics
                     * and Go need full syscall translation again. Musl
                     * never shadows (ADR-0009: supervisor-only). */
                    t->shadow = g_shadow && (kind == 0 || (kind == 3 && g_libc_kind == SP_LIBC_MUSL));
                    if (g_debug)
                        SP_TRACE("[%d] exec event: image is %s shadow=%d\n", w,
                                 sp_kind_name(kind), t->shadow);
                }
            } else if (ev == (unsigned int)PTRACE_EVENT_FORK ||
                       ev == (unsigned int)PTRACE_EVENT_VFORK ||
                       ev == (unsigned int)PTRACE_EVENT_CLONE) {
                unsigned long long np = 0;
                if (ptrace(PTRACE_GETEVENTMSG, w, 0, &np) == 0 && np) {
                    tracee_t *c = find_or_add((pid_t)np);
                    if (c) c->shadow = t->shadow;   /* inherits; exec-stop re-classifies */
                }
            }
            goto cont;
        }
        /* never re-inject SIGSTOP: clone-stops deliver SIGSTOP as the
         * *initial* stop reason; forwarding it back freezes the child. */
        ptrace(PTRACE_SYSCALL /* EXP: shadow same-mode */, w, 0,
               sig == SIGSTOP ? (void *)0 : (void *)(long)sig);
        continue;
    cont:
        /* NULL-t not possible via loop flow (w had an event) EXCEPT the
         * SP_MAX_TRACEES-exhausted path at 2635 — observed as a SIGSEGV at
         * NULL+0x44 (si_addr == offsetof(tracee_t, shadow)) taking the
         * supervisor down and PTRACE_O_EXITKILL mass-killing every guest
         * (2026-08-13 firefox-session crash). Resume syscall-mode as the
         * safe default (unclassified tracee must keep its syscall stops for
         * the entry policy/classification to ever work — CONT would let a
         * static image execute an untranslated open). A slower extra tracee
         * beats a dead tree. */
        ptrace(t && t->shadow ? PTRACE_CONT : PTRACE_SYSCALL, w, 0, 0);
        } /* end drain loop */
        /* outer loop repeats: repoll both sources */
        continue;
        done: break;
    }
    return 0;
}
