//! `sprout` — rootless glibc Linux runtime for Android.
//!
//! A drop-in replacement for `proot`'s CLI. v0.1 delivers the LD_PRELOAD
//! fast path for dynamic guest binaries; `execve` chaining lands in v0.2
//! and the automatic ptrace fallback for static/Go binaries in v0.3
//! (see docs/src/adr/).

use std::ffi::OsString;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use sprout_core::{
    classify, elf_meta, Binding, Error, GuestClass, LaunchPlan, LibcFlavor, Rootfs, Strategy,
};

/// Rootless glibc Linux userspace for Android (proot-compatible CLI).
///
/// Runs guest binaries through `LD_PRELOAD` path translation with an
/// automatic ptrace fallback for static/Go binaries (v0.3).
#[derive(Debug, Parser)]
#[command(
    name = "sprout",
    version,
    long_version = concat!(env!("CARGO_PKG_VERSION"), "\nCopyright (c) 2026 sprout contributors\nLicense: MIT OR Apache-2.0 (dual).\nGitHub: https://github.com/YRangel/sprout"),
    about,
    long_about = None,
    // Android terminal-width detection is flaky: on some Termux/termios
    // configs clap falls back to unlimited width, emits ultra-long unwrapped
    // help lines, and the tty wraps them such that continuation lines
    // interleave with the next option's text — reading as if help printed
    // twice side-by-side. Pin a sane working width so wrapping is computed
    // per-block, not per-terminal-guess. 100 < real width still wraps big
    // phones fine and matches the long --qemu/--kill-on-exit strings below.
    max_term_width = 100,
    after_help = "RULE #1: sprout options go BEFORE the guest command. Separate with -- when needed:\n  sprout -r ROOTFS --user=0:0 -- /bin/sh -c 'ls -la /root'\nEXAMPLES:\n  sprout -r ~/roots/debian --user=0:0 -- /bin/bash\n  sprout -r ~/roots/debian --shared-tmp --termux-x11 --user=0:0 -- startxfce4\n  sprout -r ~/roots/debian -- ./x86_64-app       (box64 auto-detected; -q /path/to/box64 overrides)\nPROOT COMPAT: -i/--change-id, -0/--root-id, -L, -k/--kernel-release, -q/--qemu,\n  -b/--bind, -w/--cwd, -p/-P/--redirect-ports/--fix-low-ports (--port-mapping),\n  --shared-tmp, --termux-x11, --sysvipc, --ashmem-memfd, --mixed-syscall (no-ops),\n  --link2symlink/--no-link2symlink.\nUnknown flags fail loudly — NEVER silently passed to the guest."
)]
struct Cli {
    /// Guest root directory (the "fake chroot"). Required.
    #[arg(short = 'r', long = "rootfs", value_name = "PATH")]
    rootfs: PathBuf,

    /// Set guest working directory (default: target user's home).
    #[arg(short = 'w', long = "cwd", value_name = "GUEST_DIR")]
    cwd: Option<String>,

    /// Bind host path into the guest; repeatable. `-b host` or `-b host:guest`.
    #[arg(short = 'b', long = "bind", value_name = "HOST[:GUEST]")]
    binds: Vec<String>,

    /// Fake uid/gid 0 — this is the DEFAULT.
    ///
    /// proot parity: guest sees root unless --no-fakeroot.
    #[arg(
        short = '0',
        long = "root-id",
        default_value_t = true,
        overrides_with = "no_fakeroot"
    )]
    root_id: bool,

    /// Run as REAL host uid/gid (no fakery).
    ///
    /// Identity syscalls/get*id become kernel-truthful (mostly EPERM for
    /// anything privileged).
    #[arg(long = "no-fakeroot", default_value_t = false)]
    no_fakeroot: bool,

    /// Bind host $PREFIX/tmp to guest /tmp (X11/audio sockets).
    ///
    /// proot-distro --shared-tmp parity: guest sees the live host socket dir
    /// so X11/Wayland/VirGL/virpipe/ssh-agent transact with the Termux X
    /// server like proot-distro login --shared-tmp.
    #[arg(long = "shared-tmp", default_value_t = false)]
    shared_tmp: bool,

    /// Termux-X11 preset: export DISPLAY=:0 + PULSE_SERVER to the guest.
    ///
    /// Off by default — sprout never invents X11/audio env (only inherits what
    /// the caller set). Pair with --shared-tmp so the guest can reach the X
    /// socket in $PREFIX/tmp; without it sprout warns but continues (DISPLAY
    /// with no socket is a footgun for clients that don't need it).
    #[arg(long = "termux-x11", default_value_t = false)]
    termux_x11: bool,

    /// Convert guest hardlinks to symlinks — this is the DEFAULT.
    ///
    /// proot-distro parity: SELinux denies hardlinks under
    /// /data/data/.../files.
    #[arg(
        long = "link2symlink",
        default_value_t = true,
        overrides_with = "no_link2symlink"
    )]
    link2symlink: bool,

    /// Disable the default hardlink→symlink fallback.
    #[arg(long = "no-link2symlink", default_value_t = false)]
    no_link2symlink: bool,

    /// Keep host $HOME in the guest.
    ///
    /// Default (proot parity) is HOME=/root.
    #[arg(long = "host-home")]
    host_home: bool,

    /// Fake-login as guest user instead of root.
    ///
    /// `-u NAME`, `-u UID`, `-u NAME:GROUP`, `-u UID:GID` (proot -i /
    /// proot-distro --user parity). Resolved against guest /etc/passwd +
    /// /etc/group. Fake-id family, ownership spoof, SO_PEERCRED, HOME/SHELL/
    /// USER/LOGNAME and the default cwd all anchor to that user. Kernel truth
    /// unchanged (the app uid) — fake-id, not privilege change.
    #[arg(
        short = 'u',
        long = "user",
        value_name = "USER[:GROUP]",
        conflicts_with = "no_fakeroot"
    )]
    user: Option<String>,

    /// proot `-i/--change-id` compat alias — identical contract to
    /// `-u/--user` (faked uid[:gid]; proot-distro muscle memory).
    #[arg(
        short = 'i',
        long = "change-id",
        value_name = "USER[:GROUP]",
        conflicts_with = "user"
    )]
    change_id: Option<String>,

    /// proot `-L` obsolete-flag acceptance (parses, prints a one-line note).
    ///
    /// proot's `-L` re-pinned dynamic loader search paths, long obsolete there
    /// too. sprout's loader chain always resolves the guest's real ld.so, so
    /// there is nothing to fix.
    #[arg(short = 'L', long = "loader-fix", default_value_t = false)]
    loader_fix: bool,

    /// proot `--mixed-syscall` acceptance no-op (always-on semantics in sprout).
    ///
    /// Printed as a plan-time note; sprout's preload interposer handles glibc
    /// wrappers natively and supervisor translation is per-tracee anyway.
    #[arg(long = "mixed-syscall", default_value_t = false)]
    mixed_syscall: bool,

    /// Append host $PREFIX/bin to the guest PATH (default: clean guest-only).
    #[arg(long = "host-path")]
    host_path: bool,

    /// Emulator for x86/x86_64 execs (default: /usr/local/bin/box64).
    ///
    /// proot `-q`/`--qemu` parity: x86_64 (and i386 via box32) execs are
    /// rewritten to run through the given guest-side emulator (ADR-0018
    /// userspace binfmt adapter). Per-arch overrides:
    /// SPROUT_BINFMT_X86_64 / SPROUT_BINFMT_I386. SPROUT_BINFMT_ALWAYS=1 wraps
    /// even native aarch64 ELFs (proot -q's wrap-everything semantics).
    #[arg(short = 'q', long = "qemu", value_name = "PATH")]
    qemu: Option<String>,

    /// Interception strategy override (auto = detect from guest ELF).
    #[arg(
        long = "fallback",
        value_name = "preload|ptrace",
        default_value = "auto"
    )]
    fallback: String,

    /// Show the resolved launch plan without executing.
    #[arg(long = "dry-run", default_value_t = false)]
    dry_run: bool,

    /// Log path translations to stderr (proot parity, level-capable).
    #[arg(short = 'v', long = "verbose", num_args = 0..=1,
          default_missing_value = "1", value_name = "LEVEL")]
    verbose: Option<u32>,

    /// Command and arguments, guest-spelled.
    #[arg(value_name = "COMMAND [ARGS...]", trailing_var_arg = true)]
    cmd: Vec<OsString>,

    /// Kill every process tagged to this session when the command exits.
    ///
    /// proot --kill-on-exit parity. Mechanism: tag the child env
    /// (SPROUT_KILL_TAG=<pid>); at exit sweep /proc for that tag and SIGKILL.
    /// Inherit-only: processes launched OUTSIDE our session (e.g. a steam
    /// daemon forked from a DIFFERENT sprout run) are not swept.
    #[arg(long = "kill-on-exit", default_value_t = false)]
    kill_on_exit: bool,

    /// Spoof `uname -r` to RELEASE (guest-visible kernel version).
    #[arg(short = 'k', long = "kernel-release", value_name = "RELEASE")]
    kernel_release: Option<String>,

    /// Remap privileged ports (<1024) to BASE+port (default BASE=1024).
    ///
    /// proot -p parity. Rewritten in the preload bind wrapper for
    /// AF_INET/AF_INET6 only. Connect/sendto stay untouched. Aliases for
    /// proot-distro muscle memory: -P / --redirect-ports / --fix-low-ports.
    /// BASE must be >1024; leaves headroom up to BASE+1023 for the guest's
    /// highest privileged port.
    #[arg(
        short = 'p',
        long = "port-mapping",
        alias = "redirect-ports",
        visible_alias = "fix-low-ports",
        short_alias = 'P',
        num_args = 0..=1,
        default_missing_value = "1024",
        value_name = "BASE"
    )]
    port_mapping: Option<u16>,

    /// proot --sysvipc acceptance no-op (SysV emulation is always-on; ADR-0018).
    ///
    /// Exists so proot-distro-style command lines parse cleanly. sprout's
    /// sysvipc emulation is permanent on x86/box64 lanes (the kernel never
    /// carries CONFIG_SYSVIPC); override via SPROUT_SYSVIPC_OFF=1.
    #[arg(long = "sysvipc", default_value_t = false)]
    sysvipc_compat: bool,

    /// proot --ashmem-memfd parity: memfd_create falls back to /dev/ashmem on ENOSYS.
    ///
    /// (ASHMEM_SET_NAME + SET_SIZE ioctls; st_size patched from lseek-end
    /// for the tracked fd ring since ashmem can't carry a size in fstat).
    #[arg(long = "ashmem-memfd", default_value_t = false)]
    ashmem_memfd: bool,
}

extern "C" {
    fn umask(mask: u32) -> u32;
}

/* Host-arch SysV-IPC shim lane: aarch64-host gclib emulators (FEX,
 * qemu-user, box64/box32) re-issue guest SysV IPC through named host-libc
 * calls, so an arm64 shim prepended into the emulator's own process env
 * intercepts them (order matters for symbol resolution). Basename detection
 * mirrored in the preload csrc's matching exec-hook helper. */
fn host_emu_sysvipc_shim(bn: &str) -> bool {
    bn.starts_with("FEX")
        || bn.starts_with("qemu-")
        || bn.starts_with("box64")
        || bn.starts_with("box32")
}

fn run() -> Result<u8, Error> {
    let cli = Cli::parse();

    // Android/Termux apps run with umask 077; a Linux guest that believes
    // it is uid=0 expects the distro default 022. proot-distro hides this
    // because it enters via a login shell whose profile re-sets umask; we
    // exec guests directly, so the host umask leaks through (observed:
    // gcc's output binary became 0700 -> `test -x conftest` in autoconf
    // fails -> configure marks every libc function missing). Guest distro
    // profiles still override; override ours via SPROUT_KEEP_UMASK=1.
    if std::env::var_os("SPROUT_KEEP_UMASK").is_none() {
        unsafe { umask(0o022) };
    }

    let mut rootfs = Rootfs::new(cli.rootfs)?;
    rootfs.cwd = cli.cwd;
    rootfs.fakeroot = cli.root_id && !cli.no_fakeroot;
    rootfs.link2symlink = cli.link2symlink && !cli.no_link2symlink;
    rootfs.host_home = cli.host_home;
    rootfs.host_path = cli.host_path;
    rootfs.qemu = cli.qemu;
    if cli.loader_fix {
        eprintln!("sprout: note: '-L' is obsolete — the guest ld.so is always resolved correctly (accepted for proot compatibility).");
    }
    if cli.mixed_syscall {
        eprintln!("sprout: note: '--mixed-syscall' is a no-op — the preload interposer handles glibc wrappers natively (accepted for proot compatibility).");
    }
    if let Some(spec) = cli.user.as_ref().or(cli.change_id.as_ref()) {
        let (uid, gid, name, home, shell) = rootfs.resolve_user(spec)?;
        /* `--user` implies the fake-id machinery at a non-root anchor
         * (proot -i works identically: id-family faked, kernel untouched). */
        rootfs.fakeroot = true;
        rootfs.fake_uid = Some(uid);
        rootfs.fake_gid = Some(gid);
        rootfs.user_name = Some(name);
        rootfs.user_home = Some(home);
        rootfs.user_shell = Some(shell);
    }
    for spec in &cli.binds {
        rootfs.bindings.push(Binding::parse(spec)?);
    }
    if cli.shared_tmp {
        // host $PREFIX/tmp → guest /tmp (proot-distro --shared-tmp)
        let tmp = std::env::var("PREFIX")
            .map(|p| format!("{}/tmp", p))
            .unwrap_or_else(|_| std::env::var("TMPDIR").unwrap_or_else(|_| "/tmp".to_string()));
        rootfs
            .bindings
            .push(Binding::parse(&format!("{}:/tmp", tmp))?);
    }
    rootfs.termux_x11 = cli.termux_x11;
    if cli.termux_x11 && !rootfs.bindings.iter().any(|b| b.guest == *"/tmp") {
        eprintln!(
            "sprout: note: --termux-x11 without --shared-tmp: the guest cannot\n\
             reach the Termux-X11 socket ($PREFIX/tmp/.X11-unix) — X clients\n\
             will fail unless you provide the socket some other way."
        );
    }
    // Implicit bind: guest /dev/shm → $ROOTFS/tmp (proot-distro parity;
    // proot-distro binds its rootfs's own /tmp as /dev/shm, giving python's
    // multiprocessing SemLock, postgres, etc. a writable POSIX-shm dir even
    // though Android hosts have no /dev/shm). Pushed LAST so an explicit
    // -b …:/dev/shm of equal length still wins (ties = insertion order).
    rootfs.bindings.push(Binding::parse(&format!(
        "{}:/dev/shm",
        rootfs.root.join("tmp").display()
    ))?);

    if cli.cmd.is_empty() {
        eprintln!("sprout: COMMAND is required (try 'sprout -r <rootfs> --help')");
        std::process::exit(2);
    }
    let verbose = cli.verbose.unwrap_or(0) > 0;
    let mut program_name = cli.cmd[0].to_string_lossy().into_owned();
    let mut program_host = rootfs.find_program(&program_name)?;
    /* Symlink-forest guests (Alpine: every applet -> /bin/busybox) must be
     * classified/read through the ROOTFS-relative resolution; fs::File::open
     * on the raw host path would chase the link against the HOST root,
     * where /bin/busybox doesn't exist. Derive the guest-absolute path from
     * the (rootfs-prefixed) resolution find_program returned. */
    let guest_abs = program_name
        .starts_with('/')
        .then(|| std::path::PathBuf::from(&program_name))
        .or_else(|| {
            program_host
                .strip_prefix(&rootfs.root)
                .ok()
                .map(|rel| std::path::Path::new("/").join(rel))
        });
    let classify_path = guest_abs
        .as_ref()
        .and_then(|g| rootfs.guest_real(g))
        .unwrap_or_else(|| program_host.clone());
    /* Rootfs-internal absolute symlinks (Alpine busybox foresterie) must
     * be handed to the loader RESOLVED: the musl/glibc loaders open the
     * program path on the HOST side, where chasing 'bin/sh -> /bin/busybox'
     * lands on the host root and ENOENTs. argv[0] (guest spelling) still
     * carries the applet identity forwarded by the chain. */
    if classify_path != program_host {
        program_host = classify_path.clone();
    }

    // Full argv passed to the plan (argv[0] included, matching kernel exec):
    // for shebang scripts this becomes [interp, opt?, script, orig args...].
    let mut full_cmd: Vec<OsString> = cli.cmd.clone();

    let mut class = classify(&classify_path)?;
    /* ADR-0017: the ELF identity the binfmt gate sniffs — for native execs
     * this is classify_path; for scripts it must be the SHEBANG interpreter
     * (the script itself is NotElf, the interpreter is what actually runs). */
    let mut sniff_target = classify_path.clone();

    // Shebang (#!): exec the script's interpreter instead (kernel semantics:
    // argv = [interp, opt-arg?, script, orig args...]). The interpreter must
    // itself be a guest binary on PATH (supports '#!/usr/bin/env X' too).
    if matches!(class, GuestClass::NotElf) {
        if let Some((interp, opt)) = parse_shebang(&classify_path) {
            let interp_host = rootfs
                .find_program(&interp)
                .map_err(|_| Error::UnsupportedElf {
                    program: program_name.clone(),
                    class: class.clone(),
                })?;
            let interp_class_path = std::path::Path::new(&interp)
                .is_absolute()
                .then(|| rootfs.guest_real(std::path::Path::new(&interp)))
                .flatten()
                .or_else(|| {
                    interp_host
                        .strip_prefix(&rootfs.root)
                        .ok()
                        .map(|rel| std::path::Path::new("/").join(rel))
                        .and_then(|g| rootfs.guest_real(&g))
                })
                .unwrap_or_else(|| interp_host.clone());
            let interp_class = classify(&interp_class_path)?;
            /* kernel argv semantics: argv[script-slot] must be a PATH the
             * interp can open — for a PATH-resolved invocation the user's
             * bare name ('startxfce4') is NOT openable from the child's cwd,
             * so plug in the guest-absolute resolution instead
             * ('/usr/bin/startxfce4'). Explicit / and ./ spellings pass
             * through unchanged (cwd-relative semantics are the user's). */
            let script_guest = guest_abs
                .as_ref()
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_else(|| program_name.clone());
            let orig_args: Vec<OsString> = cli.cmd[1..].to_vec();
            let mut rebuilt: Vec<OsString> = vec![interp.clone().into()];
            if let Some(o) = opt {
                rebuilt.push(o.into());
            }
            rebuilt.push(script_guest.into());
            rebuilt.extend(orig_args);
            full_cmd = rebuilt;
            program_host = interp_host;
            program_name = interp;
            class = interp_class;
            sniff_target = interp_class_path;
        }
    }
    // ---- ADR-0018: launcher-side binfmt wrap (proot -q parity at program 0) ----
    // The preload-lane gate (sprout_preload.c) only reaches CHILD execve's
    // from already-interposed processes; the very first exec issued by the
    // CLI sits outside its reach. Same sniffing contract here, using the
    // final resolved class + host path.
    let mut emu_sysvipc_shim = false;
    {
        let emu64_cfg = || {
            std::env::var("SPROUT_BINFMT_X86_64")
                .ok()
                .filter(|s| !s.is_empty())
                .or_else(|| rootfs.qemu.clone())
                .unwrap_or_else(|| "/usr/local/bin/box64".to_string())
        };
        let always = std::env::var("SPROUT_BINFMT_ALWAYS")
            .map(|v| v == "1")
            .unwrap_or(false);
        let wrap_meta = elf_meta(&sniff_target)?;
        let wrap_emu = match wrap_meta {
            Some((2, 62)) => Some(emu64_cfg()),
            Some((1, 3)) => Some(
                std::env::var("SPROUT_BINFMT_I386")
                    .ok()
                    .filter(|s| !s.is_empty())
                    .unwrap_or_else(emu64_cfg),
            ),
            Some((_, 183)) if always => Some(emu64_cfg()),
            _ => None,
        };
        if let Some(emu) = wrap_emu {
            /* Never wrap the emulator's own exec (it re-surfaces here with
             * the rewritten argv): compare resolved host paths. */
            /* Guest resolution first; an absolute path existing only on
             * the HOST (emulator installed into Termux outside the rootfs)
             * is taken as-is, checked last so guest defaults keep priority. */
            let emu_host = {
                let guest = rootfs
                    .find_program(&emu)
                    .unwrap_or_else(|_| rootfs.to_host(std::path::Path::new(&emu)));
                if guest.is_file()
                    || !std::path::Path::new(&emu).is_absolute()
                    || !std::path::Path::new(&emu).is_file()
                {
                    guest
                } else {
                    std::path::PathBuf::from(&emu)
                }
            };
            if emu_host != sniff_target {
                if !emu_host.is_file() {
                    return Err(Error::BinfmtNoEmulator {
                        program: program_name.clone(),
                        emu: emu.clone(),
                    });
                }
                /* Host-bionic emulator lane (box64/qemu-user installed into
                 * TERMUX, not the rootfs): the glibc loader-chain plan cannot
                 * carry a bionic image — bionic's libc.so lookup trips over
                 * the guest's glibc libdirs injected via LD_LIBRARY_PATH and
                 * dies with 'invalid ELF header'. Direct-spawn instead with a
                 * HOST env contract: scrub LD_PRELOAD/LD_LIBRARY_PATH and
                 * give the arch libdirs in HOST spellings. */
                if let Ok(GuestClass::Dynamic { interp }) = classify(&emu_host) {
                    if interp.starts_with("/system/bin/linker") {
                        let root_host = rootfs.to_host(std::path::Path::new("/"));
                        let mut cmd_args: Vec<std::ffi::OsString> = Vec::new();
                        cmd_args.push(sniff_target.clone().into_os_string());
                        cmd_args.extend(full_cmd.iter().skip(1).cloned());
                        let mut cmd = std::process::Command::new(&emu_host);
                        cmd.args(&cmd_args);
                        /* forward host env minus the glibc rows box64 chokes on */
                        for (k, v) in std::env::vars_os() {
                            if k == "LD_PRELOAD" || k == "LD_LIBRARY_PATH" {
                                continue;
                            }
                            cmd.env(&k, &v);
                        }
                        if std::env::var_os("BOX64_LD_LIBRARY_PATH").is_none() {
                            let libs: Vec<std::ffi::OsString> = [
                                "/usr/lib/x86_64-linux-gnu",
                                "/lib/x86_64-linux-gnu",
                                "/usr/x86_64-linux-gnu/lib",
                            ]
                            .iter()
                            .filter_map(|g| {
                                let h = root_host.join(&g[1..]);
                                h.is_dir().then(|| h.into_os_string())
                            })
                            .collect();
                            if !libs.is_empty() {
                                let joined = std::env::join_paths(
                                    libs.iter()
                                        .map(std::path::PathBuf::from)
                                        .collect::<Vec<_>>(),
                                );
                                if let Ok(j) = joined {
                                    cmd.env("BOX64_LD_LIBRARY_PATH", j);
                                }
                            }
                        }
                        let rc = cmd.status().map_err(|e| {
                            Error::BadBinding(format!("exec {}: {e}", emu_host.display()))
                        })?;
                        return Ok(if rc.success() {
                            0
                        } else {
                            rc.code().unwrap_or(128).clamp(0, 255) as u8
                        });
                    }
                }
                /* Kernel argv semantics preserved: prepend the emulator to
                 * the ALREADY-resolved argv — [emu, interp?, script?, args]
                 * for scripts, [emu, target, args] for direct ELFs. */
                /* ADR-0018: inject sysvipc shim for the killed-syscall
                 * family (semget/shmget ENOSYS on Android stock GKI). Guest-
                 * arch .so lives inside the rootfs so *every* wrapping box64
                 * process in any session maps the same backing dir.
                 * Empirical box64 v0.4.3: BOX64_LD_PRELOAD applies to x86_64
                 * guests AND to the box32 persona of i386 guests (there is no
                 * BOX32_LD_PRELOAD in this build); pick the shim matching the
                 * sniffed machine. */
                let shim = if wrap_meta.map(|(_, m)| m) == Some(3) {
                    "/usr/lib/sprout-sysvipc/i386/libsprout-sysvipc.so"
                } else {
                    "/usr/lib/sprout-sysvipc/x86_64/libsprout-sysvipc.so"
                };
                /* ADR-0018: the sysvipc shim is for DYNAMIC guest ELFs; a
                 * static x86 exec image (raw _start, no PT_INTERP) has no
                 * PLT path to it and nothing to gain from the DSO — skip
                 * injecting to keep that lane free of the loader-chain pass. */
                if std::env::var_os("BOX64_LD_PRELOAD").is_none()
                    && !matches!(class, GuestClass::Static)
                    && rootfs.guest_real(std::path::Path::new(shim)).is_some()
                {
                    std::env::set_var("BOX64_LD_PRELOAD", shim);
                }

                if emu.starts_with('/') {
                    let bn = emu.rsplit('/').next().unwrap_or(emu.as_str());
                    if host_emu_sysvipc_shim(bn) {
                        emu_sysvipc_shim = rootfs
                            .guest_real(std::path::Path::new(
                                "/usr/lib/sprout-sysvipc/arm64/libsprout-sysvipc.so",
                            ))
                            .is_some()
                            && std::env::var_os("SPROUT_SYSVIPC_EMU_OFF").is_none();
                    }
                }
                if emu_sysvipc_shim {
                    std::env::set_var(
                        "SPROUT_EMU_SYSVIPC_PATH",
                        "/usr/lib/sprout-sysvipc/arm64/libsprout-sysvipc.so",
                    );
                }
                let mut rebuilt: Vec<OsString> = vec![emu.clone().into()];
                rebuilt.extend(full_cmd.iter().cloned());
                full_cmd = rebuilt;
                program_host = emu_host.clone();
                program_name = emu;
                class = classify(&emu_host)?;
            }
        }
    }
    let strategy = match cli.fallback.as_str() {
        "auto" => Strategy::for_elf(&class).ok_or(Error::UnsupportedElf {
            program: program_name.clone(),
            class: class.clone(),
        })?,
        "preload" => Strategy::Preload,
        "ptrace" => Strategy::Ptrace,
        other => {
            return Err(Error::BadBinding(format!(
                "--fallback must be auto|preload|ptrace, got '{other}'"
            )))
        }
    };

    match strategy {
        Strategy::Ptrace => {}
        Strategy::Preload => {
            if let GuestClass::Static = class {
                return Err(Error::StaticNeedsPtrace {
                    program: program_name.clone(),
                });
            }
        }
    }

    let plan = if strategy == Strategy::Ptrace || rootfs.libc_flavor() == LibcFlavor::Musl {
        /* Last-resort path (ADR-0002): supervisor translates syscall args
         * for static / preload-incapable images, and rewrites static→dynamic
         * exec into the sanitized loader chain. MUSL guests (v0.4) take this
         * route for EVERY image (ADR-0009): musl's whole-libc-in-ldso plus
         * busybox's suid-drop can't be covered by ldso sanitization alone. */
        let preload_so = if rootfs.libc_flavor() == LibcFlavor::Musl {
            sprout_preload::core_library_musl_path().ok_or(Error::PreloadNotFound)?
        } else {
            sprout_preload::core_library_path().ok_or(Error::PreloadNotFound)?
        };
        let supervisor = sprout_ptrace::supervisor_path().ok_or(Error::PtraceUnimplemented)?;
        let cache_dir = cache_dir();
        if rootfs.libc_flavor() == LibcFlavor::Musl {
            match class {
                /* Static musl: direct exec under supervisor (translates) */
                GuestClass::Static => {
                    let mut plan = LaunchPlan::supervisor(
                        &rootfs,
                        supervisor,
                        program_host,
                        &program_name,
                        &cli.cmd[1..],
                        verbose,
                        preload_so,
                        &cache_dir,
                    )?;
                    /* ADR-0016: kind=1 lets the supervisor pick the
                     * notify-statics lane (sprout-stub, no steady-state
                     * ptrace) when the kernel supports it. */
                    plan.env.push(("SPROUT_GUEST_KIND".into(), "1".into()));
                    plan
                }
                /* Dynamic musl: loader-chain launch wrapped in supervisor (kind 3) */
                _ => {
                    let pre = LaunchPlan::preload(
                        &rootfs,
                        program_host,
                        &full_cmd,
                        preload_so,
                        verbose,
                        &cache_dir,
                    )?;
                    LaunchPlan::supervise(pre, supervisor)
                }
            }
        } else {
            match class {
                /* Dynamic Go: libc-linked for cgo, but io walks raw syscalls.
                 * Interposer never sees them → supervisor translates. It still
                 * launches via the sanitized loader chain (PT_INTERP present),
                 * so wrap the preload plan under the supervisor. */
                GuestClass::GoDynamic { .. } => {
                    let pre = LaunchPlan::preload(
                        &rootfs,
                        program_host,
                        &full_cmd,
                        preload_so,
                        verbose,
                        &cache_dir,
                    )?;
                    LaunchPlan::supervise(pre, supervisor)
                }
                /* Dynamic ELF under --fallback ptrace: the raw binary cannot be
                 * exec'd natively (host kernel rejects an ENOENT PT_INTERP).
                 * Mirror the GoDynamic arm: preload-plan wrapped under the
                 * supervisor, so the loader chain runs with ptrace semantics. */
                GuestClass::Dynamic { .. } => {
                    let pre = LaunchPlan::preload(
                        &rootfs,
                        program_host,
                        &full_cmd,
                        preload_so,
                        verbose,
                        &cache_dir,
                    )?;
                    LaunchPlan::supervise(pre, supervisor)
                }
                _ => {
                    let mut plan = LaunchPlan::supervisor(
                        &rootfs,
                        supervisor,
                        program_host,
                        &program_name,
                        &cli.cmd[1..],
                        verbose,
                        preload_so,
                        &cache_dir,
                    )?;
                    /* ADR-0016: kind=1 lets the supervisor pick the
                     * notify-statics lane (sprout-stub, no steady-state
                     * ptrace) when the kernel supports it. Go statics also
                     * take the Static class (no PT_INTERP): the supervisor
                     * distinguishes 1/2 itself if ever needed. */
                    if matches!(class, GuestClass::Static) {
                        plan.env.push(("SPROUT_GUEST_KIND".into(), "1".into()));
                    }
                    plan
                }
            }
        }
    } else {
        let preload_so = if rootfs.libc_flavor() == LibcFlavor::Musl {
            sprout_preload::core_library_musl_path().ok_or(Error::PreloadNotFound)?
        } else {
            sprout_preload::core_library_path().ok_or(Error::PreloadNotFound)?
        };
        let cache_dir = cache_dir();
        let pre = LaunchPlan::preload(
            &rootfs,
            program_host,
            &full_cmd,
            preload_so,
            verbose,
            &cache_dir,
        )?;
        /* SHADOW SUPERVISION (ADR-0012): interposed glibc processes make
         * raw syscalls that PLT can never see (libuv's io_uring_setup,
         * V8/JSC internals, ...). A ptrace supervisor MUST watch for
         * SECCOMP SIGSYS and emulate a truthful fallback, or the guest
         * dies outright. Shadow tracees free-run via PTRACE_CONT: the only
         * stops are signals + exec events, so fast-path perf is preserved.
         * Supervise() strips LD_* for the host supervisor binary and
         * re-injects the guest stack via SPROUT_GUEST_PRELOAD. */
        let shadow_off = std::env::var("SPROUT_NO_SHADOW").is_ok();
        if !shadow_off {
            if let Some(supervisor) = sprout_ptrace::supervisor_path() {
                let mut plan = LaunchPlan::supervise(pre, supervisor);
                plan.env.push(("SPROUT_SHADOW".into(), "1".into()));
                plan
            } else {
                pre
            }
        } else {
            pre
        }
    };

    let mut plan = plan;
    /* Static binaries launched from INSIDE the preload interposer must
     * route through the supervisor (the interposer can't seccomp-emulate
     * them). Advertising the supervisor's own path via env lets the C
     * exec-chain exec it directly (SP_ELF_STATIC arm of sp_execve_chain). */
    if !plan.env.iter().any(|(k, _)| k == "SPROUT_PTRACE") {
        if let Some(px) = sprout_ptrace::supervisor_path() {
            plan.env
                .push(("SPROUT_PTRACE".into(), px.display().to_string()));
        }
    }

    /* proot-flag parity env (ADR-0019). The interposer reads these; the
     * env is inherited through the whole exec chain, so descendants see
     * them without further flag plumbing. */
    // Kernel-release spoofing: explicit -k wins; otherwise brand the guest's
    // view as sprout's own ("Sprout-Android-<host uname -r>"), so tools like
    // fastfetch/neofetch print us instead of the raw HyperOS build string.
    // Empty SPROUT_KERNEL_RELEASE in the env = opt-out (host string passes).
    let rel = cli
        .kernel_release
        .clone()
        .or_else(|| std::env::var("SPROUT_KERNEL_RELEASE").ok());
    let rel = match rel {
        Some(ref s) if !s.is_empty() => s.clone(),
        Some(_) => String::new(), // SPROUT_KERNEL_RELEASE="" = no spoof
        None => {
            let mut u: libc::utsname = unsafe { std::mem::zeroed() };
            let host_rel = if unsafe { libc::uname(&mut u) } == 0 {
                unsafe { std::ffi::CStr::from_ptr(u.release.as_ptr()) }
                    .to_string_lossy()
                    .into_owned()
            } else {
                "unknown".into()
            };
            format!("Sprout-Android-{host_rel}")
        }
    };
    plan.env.push(("SPROUT_KERNEL_RELEASE".into(), rel));
    if let Some(base) = cli.port_mapping {
        if !(1024..=64512).contains(&base) {
            return Err(Error::Cli(format!(
                "-p BASE must be 1024..=64512 (got {base}); 65535-BASE must leave room for privileged ports"
            )));
        }
        plan.env.push(("SPROUT_PORTMAP".into(), "1".into()));
        plan.env
            .push(("SPROUT_PORTMAP_BASE".into(), base.to_string()));
    }
    if cli.ashmem_memfd {
        plan.env.push(("SPROUT_ASHMEM_MEMFD".into(), "1".into()));
    }
    let kill_tag = cli.kill_on_exit.then(|| format!("{}", std::process::id()));
    if let Some(tag) = &kill_tag {
        plan.env.push(("SPROUT_KILL_TAG".into(), tag.clone()));
    }

    if cli.dry_run {
        eprintln!("{}", plan.explain());
        return Ok(0);
    }

    let status = plan.run()?;

    /* proot --kill-on-exit: after the command exits, sweep /proc for any
     * process whose environ carries our SPROUT_KILL_TAG=<pid> marker and
     * SIGKILL it (env survives every exec, so all descendants inherit the
     * tag without the launcher tracing them). Best-effort: processes the
     * signal hits mid-exit may already be gone (ESRCH is swallowed). */
    if let Some(tag) = &kill_tag {
        sweep_kill_tagged(tag);
    }

    Ok(status.code().map(|c| c as u8).unwrap_or(1))
}

/// Kill every LIVE process whose /proc/PID/environ contains
/// "SPROUT_KILL_TAG=<tag>". Skips self (the launcher IS one of the
/// tagged, killing it would short-circuit the sweep before the last PIDs
/// are processed). Proc-parses can fail on per-PID procfs permission
/// denials — those PIDs stay alive, which is the documented best-effort
/// envelope of the v1 implementation (supervisor lanes have pid lists
/// handled by ptrace; this sweep covers the preload fast path).
fn sweep_kill_tagged(tag: &str) {
    let needle = format!("SPROUT_KILL_TAG={}", tag);
    let uid = unsafe { getuid() };
    let self_pid = std::process::id() as i32;
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return;
    };
    for ent in entries.flatten() {
        let name = ent.file_name();
        let Some(name) = name.to_str() else { continue };
        let Ok(pid) = name.parse::<i32>() else {
            continue;
        };
        if pid == self_pid {
            continue;
        }
        let env_path = ent.path().join("environ");
        let Ok(environ) = std::fs::read(&env_path) else {
            continue;
        };
        if std::str::from_utf8(&environ)
            .map(|s| s.contains(needle.as_str()))
            .unwrap_or(false)
        {
            unsafe { kill(pid, 9) };
        }
    }
    let _ = uid;
}

extern "C" {
    fn getuid() -> u32;
    fn kill(pid: i32, sig: i32) -> i32;
}

/// Where the sanitized-libc cache lives (ADR-0007). Override with
/// SPROUT_CACHE_DIR (authoritative: returned verbatim, any error
/// surfaces at use). Otherwise walk a writability-probed cascade:
///
///   1. $HOME/.cache/sprout
///   2. $TMPDIR/sprout-$UID   (or the crate temp_dir when TMPDIR unset)
///   3. /tmp/sprout-$UID
///
/// First candidate that both `create_dir_all`s and takes a probe-file
/// write wins. Each step logs a one-line notice so an EROFS primer
/// (seen on a friend's device with a read-only $HOME) is diagnosable
/// instead of fatal.
fn cache_dir() -> PathBuf {
    if let Ok(p) = std::env::var("SPROUT_CACHE_DIR") {
        return PathBuf::from(p);
    }
    let uid = unsafe { getuid() };
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(home) = std::env::var("HOME") {
        candidates.push(PathBuf::from(home).join(".cache").join("sprout"));
    }
    candidates.push(std::env::temp_dir().join(format!("sprout-{uid}")));
    candidates.push(PathBuf::from(format!("/tmp/sprout-{uid}")));
    let mut tried: Vec<String> = Vec::new();
    for cand in &candidates {
        tried.push(cand.display().to_string());
        if cache_dir_writable(cand) {
            return cand.clone();
        }
    }
    /* Nothing writable: return the HOME candidate anyway so downstream
     * `create_dir_all` produces the *real* errno at the *real* path,
     * with the cascade note prepended for the user's layer-8. */
    eprintln!(
        "sprout: warning: no writable cache dir found (tried: {}); continuing with {}",
        tried.join(", "),
        candidates
            .first()
            .map_or_else(|| "<none>".to_string(), |p| p.display().to_string())
    );
    candidates
        .into_iter()
        .next()
        .unwrap_or_else(std::env::temp_dir)
}

/// Probe: the dir must accept a transient file (some mounts are
/// mkdir-capable yet write-denied; the EROFS primer was exactly that).
fn cache_dir_writable(dir: &std::path::Path) -> bool {
    if std::fs::create_dir_all(dir).is_err() {
        return false;
    }
    let probe = dir.join(format!(".probe-{}", std::process::id()));
    match std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&probe)
    {
        Ok(_) => {
            let _ = std::fs::remove_file(&probe);
            true
        }
        Err(_) => false,
    }
}

/// Parse a guest script's shebang line: `#!/path/to/interp [one-opt-arg]`.
///
/// Kernel rules (fs/binfmt_script.c): everything on the first line after
/// `#!` up to whitespace separates interpreter and ONE optional argument.
/// `/usr/bin/env FOO` needs no special-casing: env resolves FOO on the
/// guest PATH via the interposer's `execvp` chain.
fn parse_shebang(host_path: &std::path::Path) -> Option<(String, Option<String>)> {
    use std::io::{BufRead, BufReader};
    let f = std::fs::File::open(host_path).ok()?;
    let mut first = String::new();
    BufReader::new(f).read_line(&mut first).ok()?;
    let line = first.strip_prefix("#!")?.trim();
    let mut it = line.split_whitespace();
    let interp = it.next()?.to_string();
    if interp.is_empty() {
        return None;
    }
    let opt = it.next().map(std::string::ToString::to_string);
    Some((interp, opt))
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => ExitCode::from(code),
        Err(e) => {
            eprintln!("sprout: {e}");
            ExitCode::from(1)
        }
    }
}
