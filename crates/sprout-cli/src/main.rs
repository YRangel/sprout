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
use sprout_core::{classify, Binding, Error, GuestClass, LaunchPlan, LibcFlavor, Rootfs, Strategy};

/// Rootless glibc Linux userspace for Android (proot-compatible CLI).
///
/// Runs guest binaries through `LD_PRELOAD` path translation with an
/// automatic ptrace fallback for static/Go binaries (v0.3).
#[derive(Debug, Parser)]
#[command(name = "sprout", version, about, long_about = None)]
struct Cli {
    /// Guest root directory (the "fake chroot").
    #[arg(short = 'r', long = "rootfs", value_name = "PATH")]
    rootfs: PathBuf,

    /// Working directory inside the guest.
    #[arg(short = 'w', long = "cwd", value_name = "GUEST_DIR")]
    cwd: Option<String>,

    /// Bind host path into the guest: `-b host` or `-b host:guest`. Repeatable.
    #[arg(short = 'b', long = "bind", value_name = "HOST[:GUEST]")]
    binds: Vec<String>,

    /// Fake uid/gid 0 — DEFAULT, proot parity. Guests believe they run as
    /// root unless --no-fakeroot is given.
    #[arg(short = '0', long = "root-id", default_value_t = true, overrides_with = "no_fakeroot")]
    root_id: bool,

    /// Run as the REAL host uid/gid: identity syscalls and get*id answers
    /// are kernel-truthful (mostly EPERM for anything privileged).
    #[arg(long = "no-fakeroot", default_value_t = false)]
    no_fakeroot: bool,

    /// proot-distro `--shared-tmp` parity: bind the host $PREFIX/tmp into the
    /// guest at /tmp, preserving X11/Wayland/VirGL/virpipe/ssh-agent sockets
    /// (the guest sees the live host socket dir, so X11 apps transact with the
    /// Termux X server like proot-distro login --shared-tmp).
    #[arg(long = "shared-tmp", default_value_t = false)]
    shared_tmp: bool,

    /// Convert hardlinks to symlinks on guest writes — DEFAULT (proot-distro
    /// parity: SELinux denies hardlinks under /data/data/.../files).
    #[arg(long = "link2symlink", default_value_t = true, overrides_with = "no_link2symlink")]
    link2symlink: bool,

    /// Disable the default hardlink→symlink fallback.
    #[arg(long = "no-link2symlink", default_value_t = false)]
    no_link2symlink: bool,

    /// Pass the host $HOME through to the guest instead of the proot-parity
    /// HOME=/root default.
    #[arg(long = "host-home")]
    host_home: bool,

    /// Fake-login as a guest user instead of root: `-u NAME`, `-u UID`,
    /// `-u NAME:GROUP` or `-u UID:GID` (proot `-i` / proot-distro `--user`
    /// parity). Resolved against the guest's /etc/passwd (+ /etc/group).
    /// The fake-id family, ownership spoof, SO_PEERCRED, HOME/SHELL/USER/
    /// LOGNAME and the default cwd all anchor to that user. Kernel truth
    /// is unchanged (the app uid) — this is fake-id, not privilege change.
    #[arg(short = 'u', long = "user", value_name = "USER[:GROUP]", conflicts_with = "no_fakeroot")]
    user: Option<String>,

    /// Append the host $PREFIX/bin to the guest PATH (default: the clean
    /// guest-only PATH).
    #[arg(long = "host-path")]
    host_path: bool,

    /// Force interception strategy (auto detects from guest ELF).
    #[arg(
        long = "fallback",
        value_name = "preload|ptrace",
        default_value = "auto"
    )]
    fallback: String,

    /// Show the resolved launch plan without executing.
    #[arg(long = "dry-run", default_value_t = false)]
    dry_run: bool,

    /// Log every path translation to stderr.
    #[arg(short = 'v', long = "verbose", default_value_t = false)]
    verbose: bool,

    /// Command and arguments, guest-spelled.
    #[arg(
        value_name = "COMMAND [ARGS...]",
        trailing_var_arg = true,
        allow_hyphen_values = true,
        required = true
    )]
    cmd: Vec<OsString>,
}

extern "C" {
    fn umask(mask: u32) -> u32;
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
    if let Some(spec) = &cli.user {
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
        rootfs.bindings.push(Binding::parse(&format!("{}:/tmp", tmp))?);
    }
    // Implicit bind: guest /dev/shm → $ROOTFS/tmp (proot-distro parity;
    // proot-distro binds its rootfs's own /tmp as /dev/shm, giving python's
    // multiprocessing SemLock, postgres, etc. a writable POSIX-shm dir even
    // though Android hosts have no /dev/shm). Pushed LAST so an explicit
    // -b …:/dev/shm of equal length still wins (ties = insertion order).
    rootfs.bindings.push(Binding::parse(&format!("{}:/dev/shm", rootfs.root.join("tmp").display()))?);

    let mut program_name = cli.cmd[0].to_string_lossy().into_owned();
    let mut program_host = rootfs.find_program(&program_name)?;
    /* Symlink-forest guests (Alpine: every applet -> /bin/busybox) must be
     * classified/read through the ROOTFS-relative resolution; fs::File::open
     * on the raw host path would chase the link against the HOST root,
     * where /bin/busybox doesn't exist. Derive the guest-absolute path from
     * the (rootfs-prefixed) resolution find_program returned. */
    let guest_abs = program_name.starts_with('/').then(|| std::path::PathBuf::from(&program_name))
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

    // Shebang (#!): exec the script's interpreter instead (kernel semantics:
    // argv = [interp, opt-arg?, script, orig args...]). The interpreter must
    // itself be a guest binary on PATH (supports '#!/usr/bin/env X' too).
    if matches!(class, GuestClass::NotElf) {
        if let Some((interp, opt)) = parse_shebang(&classify_path) {
            let interp_host = rootfs.find_program(&interp).map_err(|_| Error::UnsupportedElf {
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
            let script_guest = program_name.clone();
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
                        cli.verbose,
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
                        cli.verbose,
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
                    cli.verbose,
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
                    cli.verbose,
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
                    cli.verbose,
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
            cli.verbose,
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

    if cli.dry_run {
        eprintln!("{}", plan.explain());
        return Ok(0);
    }

    let mut plan = plan;
    /* Static binaries launched from INSIDE the preload interposer must
     * route through the supervisor (the interposer can't seccomp-emulate
     * them). Advertising the supervisor's own path via env lets the C
     * exec-chain exec it directly (SP_ELF_STATIC arm of sp_execve_chain). */
    if !plan.env.iter().any(|(k, _)| k == "SPROUT_PTRACE") {
        if let Some(px) = sprout_ptrace::supervisor_path() {
            plan.env.push(("SPROUT_PTRACE".into(), px.display().to_string()));
        }
    }

    let status = plan.run()?;
    Ok(status.code().map(|c| c as u8).unwrap_or(1))
}

/// Where the sanitized-libc cache lives (ADR-0007). Override with
/// SPROUT_CACHE_DIR; defaults to $HOME/.cache/sprout, or the system temp
/// dir when HOME is unset.
fn cache_dir() -> PathBuf {
    if let Ok(p) = std::env::var("SPROUT_CACHE_DIR") {
        return PathBuf::from(p);
    }
    if let Ok(home) = std::env::var("HOME") {
        return PathBuf::from(home).join(".cache").join("sprout");
    }
    std::env::temp_dir().join("sprout")
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
