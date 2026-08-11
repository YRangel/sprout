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
static const long SP_EMULATE_OK[] = { SYS_set_robust_list /*99*/, 293 /*rseq*/ };

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

/* Classify the image a tracee just exec'd: static ELF or not.
 * Reads /proc/<pid>/exe's program headers; PT_INTERP presence means a
 * dynamic loader (preload path); ET_EXEC without PT_INTERP, or static-PIE
 * ET_DYN without PT_INTERP, are "static" for policy purposes. */
static int classify_tracee_image(pid_t pid) {
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
static int translate_reg_path(tracee_t *t, pid_t pid, struct user_pt_regs *r, int argi,
                              const char *name) {
    (void)t;
    unsigned long long ptr = r->regs[argi];
    if (ptr == 0 || ptr >= 0x800000000000ULL) return 0;
    char guest[SP_PATH_MAX];
    if (peek_str(pid, ptr, guest, sizeof(guest)) < 0) return 0;
    if (guest[0] != '/') return 0;
    char host[SP_PATH_MAX];
    if (!sp_translate(&g_cfg, guest, host)) return 0;
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
                SP_TRACE("[%d] lazily classified as %s\n", pid,
                         kind ? "STATIC (supervisor translates)" : "dynamic (preload governs)");
        }
    }

    /* Path translation for STATIC tracees only — dynamic processes have
     * the LD_PRELOAD interposer (this supervisor is a last resort). */
    if (t->static_kind != 1) return;

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
                sp_syscall_view v = sp_view_syscall(w, &r); if (g_debug) fprintf(stderr, "[sprt20] %d sys-stop e_ld=%lld x0=%llu x8=%llu\n", w, (long long)r.regs[8], r.regs[0], r.regs[8]);
                if (g_debug)
                    fprintf(stderr, "[ptrace] %d SIGSYS stop eNR=%lld x8=%llu gsi_nr=%ld pc=%llx\n",
                            w, (long long)r.regs[8], r.regs[8], v.nr,
                            (unsigned long long)r.pc);
                for (size_t i = 0; i < sizeof(SP_EMULATE_OK)/sizeof(*SP_EMULATE_OK); i++) {
                    if ((long)r.regs[8] == SP_EMULATE_OK[i]) {
                        r.regs[0] = 0;
                        ptrace(PTRACE_SETREGSET, w, (void *)NT_PRSTATUS, &iov);
                        if (g_debug)
                            fprintf(stderr, "[ptrace] %d SIGSYS swallowed: sysno=%llu emulated ok\n",
                                    w, r.regs[8]);
                        emulated = 1;
                        break;
                    }
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
                                 kind ? "STATIC (supervisor translates)" : "dynamic (preload governs)");
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
