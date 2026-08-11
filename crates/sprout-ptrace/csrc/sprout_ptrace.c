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
    unsigned long long arg0;   /* original x0 on entry */
    unsigned long long argN;   /* original xN on entry */
    int   got_robust_list;     /* we replaced set_robust_list */
} tracee_t;

static tracee_t g_tracees[SP_MAX_TRACEES];
static sp_config_t g_cfg;

static tracee_t *find_or_add(pid_t pid) {
    for (int i = 0; i < SP_MAX_TRACEES; i++)
        if (g_tracees[i].pid == pid) return &g_tracees[i];
    for (int i = 0; i < SP_MAX_TRACEES; i++)
        if (g_tracees[i].pid == 0) {
            g_tracees[i].pid = pid;
            return &g_tracees[i];
        }
    return NULL;
}

/* helpers */

static int set_sysno_getpid(pid_t pid) {
    struct user_pt_regs r;
    struct iovec iov = { &r, sizeof(r) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
    r.regs[8] = SYS_getpid;
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

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

/* Query the current syscall via PTRACE_GET_SYSCALL_INFO. */
static int get_sysno_and_nr(pid_t pid, long *sysno, unsigned long long args[6]) {
    struct {
        unsigned char op;
        unsigned char pad[3];
        unsigned int arch;
        unsigned long long ip, sp;
        union {
            struct { unsigned long long nr; unsigned long long args[6]; } entry;
            struct { long long rval; unsigned char is_error; } exit_;
        } u;
    } info;
    long n = ptrace(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info), &info);
    if (n < 0) return -1;
    if (info.op == 1) {              /* ENTRY */
        *sysno = (long)info.u.entry.nr;
        for (int i = 0; i < 6; i++) args[i] = info.u.entry.args[i];
        return 1;
    }
    if (info.op == 2) {              /* EXIT */
        return 2;
    }
    return 0;
}

/* On aarch64, GET_SYSCALL_INFO arguments: for ENTRY the six args are at
 * indices 0..5 and syscall nr at 6. arg0 is in x0. */
static void apply_policy_entry(tracee_t *t, pid_t pid,
                                long sysno, unsigned long long x0, unsigned long long x1) {
    switch (sysno) {
    case SYS_set_robust_list:
        if (set_sysno_getpid(pid) != 0) break;
        t->got_robust_list = 1;
        if (g_debug) SP_TRACE("[%d] set_robust_list → getpid\n", pid);
        break;

    case SYS_openat: case SYS_openat2: case SYS_newfstatat:
    case SYS_faccessat: case SYS_faccessat2: case SYS_readlinkat:
    case SYS_statx:
        if ((int)x0 != AT_FDCWD_VAL) break;
        if (x1 == 0 || x1 >= 0x800000000000ULL) break;
        goto translate_path;
    case SYS_execve: case SYS_execveat:
        if (sysno == SYS_execveat && (int)x0 != AT_FDCWD_VAL) break;
        if (x0 == 0 || x0 >= 0x800000000000ULL) break;
        x1 = x0; /* path is in x0 for execve */
        goto translate_path;
    translate_path:;
        char guest[SP_PATH_MAX];
        if (peek_str(pid, x1, guest, sizeof(guest)) < 0) break;
        if (guest[0] != '/') break;
        char host[SP_PATH_MAX];
        if (!sp_translate(&g_cfg, guest, host)) break;
        if (poke_str(pid, x1, host, strlen(host)) != 0) break;
        if (g_debug) SP_TRACE("[%d] %ld %s → %s\n", pid, sysno, guest, host);
        break;

    default:
        break;
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
                    if (ptrace(PTRACE_GETREGSET, w, (void *)NT_PRSTATUS, &iov) == 0)
                        fprintf(stderr, "[ptrace] died sig=%d pc=%llx x8=%llu\n",
                                WTERMSIG(status), (unsigned long long)r.pc, r.regs[8]);
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

        if (sig == (SIGTRAP | 0x80)) {  /* syscall-stop */
            long sysno = -1;
            unsigned long long args6[6] = {0};
            int kind = get_sysno_and_nr(w, &sysno, args6);
            if (kind == 1) {           /* ENTRY */
                t->sysno = sysno;
                apply_policy_entry(t, w, sysno, args6[0], args6[1]);
            } else if (kind == 2) {    /* EXIT */
                if (t->got_robust_list) {
                    set_ret_0(w);
                    t->got_robust_list = 0;
                }
            }
            goto cont;
        }
        if (sig == SIGTRAP) goto cont;   /* exec/clone/fork: keep walking */
        ptrace(PTRACE_SYSCALL, w, 0, (void *)(long)sig);
        continue;
    cont:
        ptrace(PTRACE_SYSCALL, w, 0, 0);
    }
    return 0;
}
