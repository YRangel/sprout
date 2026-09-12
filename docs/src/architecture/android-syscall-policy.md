# Android baseline syscall policy — measured map (2026-09-11)

Full raw-syscall scan (nrs 0–460) on phone1 (HyperOS SDK36, kernel
6.12.23-android16, untrusted_app context, `Seccomp_filters: 1` baseline
inherited from the shell lineage). Method: one static process per nr,
`syscall(nr, -1 × 6)`, SIGSYS caught in-process; code 1 = TRAP, 2 =
ENOSYS, 4 = EPERM, 0 = executed. Raw scanner: `nr1.c` (session artifact).

## Trapped (SIGSYS) — the complete set

| Class | nrs | sprout answer |
|---|---|---|
| mount/privilege | 39 umount2, 40 mount, 41 pivot_root, 51 chroot, 58 vhangup, 89 acct, 104 kexec_load, 105 init_module, 106 delete_module, 116 syslog, 142 reboot, 161 sethostname, 162 setdomainname, 170 settimeofday, 171 adjtimex, 224 swapon, 225 swapoff, 273 finit_module | forge **-EPERM** (callers survive) |
| identity | 143–146, 149, 151, 152 set*id, 159 setgroups | musl: fake 0; glibc: **-EPERM** |
| SysV IPC | 186–197 | -ENOSYS (fallback by design) |
| POSIX mqueue | 180–185 | -ENOSYS |
| keyring | 217 add_key, 218 request_key, 219 keyctl | -ENOSYS |
| legacy accept | 202 | **pivot → accept4(242)** (re-execute) |
| kcmp | 272 | -ENOSYS |
| time64 family | 400–423 | -ENOSYS (32-bit variants exist) |
| modern misc | 424–466 minus exceptions below | -ENOSYS (probed fallback) |
| openat2 | 437 | entry-translate + pivot to openat |
| mempolicy | 234–239 | TRAP (unhandled → stub forges -ENOSYS) |
| fanotify | 262, 263 | TRAP (stub: -ENOSYS) |
| ≥288 blanket | most of 288–460 | arg-conditional, see caveat |

**NOT trapped** (usable raw): memfd_create 279, bpf 280, userfaultfd 282,
perf_event_open 241, pidfd_open 434, pidfd_getfd 438, pidfd_send_signal
424?? (see caveat), getpid **172**, gettid 178, statx 291 (see caveat).

## Caveats

- **nr numbering is asm-generic (arm64), NOT x86_64**: 39 is `umount2`,
  not getpid; getpid is **172**. (A test bug using the x86_64 number for
  getpid cost an hour of phantom-TRAP chasing — recorded so nobody else
  pays it.)
- **Arg-conditional rules exist**: statx(291) TRAPs with `mask=-1` but
  executes with a valid mask; other ≥288 nrs likely behave similarly.
  The scan's `-1 × 6` args over-report TRAP for such nrs.
- **seccomp itself works**: prctl(PR_SET_SECCOMP), seccomp(2)+NEW_LISTENER
  and NOTIF_ADDFD are all ALLOWED for untrusted_app on this kernel
  (contrary to an older probe note — corrected 2026-09-11). pidfd_open/
  pidfd_getfd also work.
- The in-guest stub handler answers trapped nrs per the tables above;
  anything untabled gets **-ENOSYS + a one-line `sgsys-nr=` diagnostic**
  (was: death-by-re-execution before the 2026-09-11 fix).

## seccomp-TRAP frame pc convention (kernel-variant!)

The rt_sigframe pc for a TRAPped svc may point **AT** the svc
(re-execution semantics; Android-16 of 2026-08, the #74 assumption) or
**ALREADY PAST** it (6.12.23-android16, measured). Advancing pc blindly
skips the next instruction and corrupts the stream (two EMUs for one
syscall, then SIGBUS at pc=0x32 — the 2026-09-11 cascade). The stub
detects the convention by reading the instruction word (`svc #0` =
0xd4000001) and advances only when the frame points AT the svc
(`stub_frame_skip_svc`). The accept(202)→accept4 pivot instead *rewinds*
a past-svc pc so the rewritten svc re-executes.
