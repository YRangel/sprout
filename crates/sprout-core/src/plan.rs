//! LaunchPlan: the fully resolved, dry-runnable description of a guest exec.
//!
//! The preload strategy executes the guest program *through* the guest's own
//! glibc loader (ADR-0003):
//!
//! ```text
//! <rootfs>/lib/ld-linux-aarch64.so.1 \
//!     --argv0 <original argv0> \
//!     --inhibit-cache \
//!     --library-path <host lib dirs> \
//!     <host program> [args...]
//! ```
//!
//! LD_PRELOAD carries the host-absolute path to `libsprout-core.so`; the
//! guest loader maps it into every dynamically linked descendant, where the
//! C interposer re-translates paths for libc calls made by the program and
//! by any `execve` chain (v0.2). No `.text` patching anywhere (ADR-0003).

use std::ffi::{OsStr, OsString};
use std::path::PathBuf;

use crate::rootfs::LibcFlavor;
use std::process::ExitStatus;

use crate::error::Error;
use crate::rootfs::Rootfs;
use crate::strategy::Strategy;

#[derive(Debug, Clone)]
pub struct LaunchPlan {
    pub strategy: Strategy,
    /// The program the host kernel executes (guest loader for Preload).
    pub loader: PathBuf,
    /// Full argument vector *after* argv[0].
    pub argv: Vec<OsString>,
    /// Environment to set for the child (additive to the caller's env).
    pub env: Vec<(String, String)>,
    /// Host working directory, if `-w` was given.
    pub cwd: Option<PathBuf>,
    /// Program as the guest should see it in argv[0]/messages.
    pub display: String,
}

/// Resolve the guest PATH: `SPROUT_GUEST_PATH` override wins, else the
/// clean guest-only default; `--host-path` appends the host $PREFIX/bin
/// (proot-distro's opt-in shape).
fn guest_path_for(rootfs: &Rootfs) -> String {
    let mut p = std::env::var("SPROUT_GUEST_PATH").unwrap_or_else(|_| {
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin".to_string()
    });
    if rootfs.host_path {
        let prefix =
            std::env::var("PREFIX").unwrap_or_else(|_| "/data/data/com.termux/files/usr".into());
        p.push_str(&format!(":{prefix}/bin"));
    }
    p
}

/// Compute the guest HOME: /root by default (proot parity); `--host-home`
/// passes the host $HOME through (also used for the default guest cwd).
fn guest_home(rootfs: &Rootfs) -> String {
    if rootfs.host_home {
        std::env::var("HOME").unwrap_or_else(|_| "/root".into())
    } else if let Some(h) = &rootfs.user_home {
        h.clone()
    } else {
        "/root".into()
    }
}

/// Guest-absolute path → host path, honoring `-b` bindings FIRST (first
/// match wins, mirroring the interposer's bind loop in sp_translate()) and
/// falling back to the plain rootfs prefix join. `to_host()` alone silently
/// drops `-b` targets — feeding it a `-w` cwd inside a bound dir then fails
/// at the exec-time chdir with a blameless "os error 2" (caller's
/// `current_dir`).
fn to_host_bound(rootfs: &Rootfs, guest: &str) -> std::path::PathBuf {
    rootfs.to_host_bound(std::path::Path::new(guest))
}

/// Push HOME/TERM/SHELL/TMPDIR policy defaults onto a plan env:
/// - HOME: /root by default (proot parity); `--host-home` passes the host
///   $HOME through.
/// - TERM: inherited from the host; falls back to xterm-256color.
/// - SHELL: host $SHELL points into bionic world ($PREFIX/bin/...) — every
///   guest that honors it (tmux's default-shell, env(1), getpass...) then
///   execs a binary from the WRONG libc world (observed: tmux pane dying
///   on libdl.so). Rewrite to a same-basename guest shell when it exists,
///   else /bin/bash, else /bin/sh.
/// - TMPDIR: host TMPDIR points outside the guest view; pin /tmp.
/// - OLDPWD: host value confuses guest cd state ($PWD chains); drop it.
fn push_home_term(env: &mut Vec<(String, String)>, rootfs: &Rootfs) {
    if !env.iter().any(|(k, _)| k == "HOME") {
        env.push(("HOME".into(), guest_home(rootfs)));
    }
    /* ADR-0018 binfmt adapter env tuple: the `-q` path feeds both arches
     * (box64's integrated box32 handles i386 as well as x86_64). */
    if let Some(qemu) = &rootfs.qemu {
        if !env.iter().any(|(k, _)| k == "SPROUT_BINFMT_X86_64") {
            env.push(("SPROUT_BINFMT_X86_64".into(), qemu.clone()));
        }
        if !env.iter().any(|(k, _)| k == "SPROUT_BINFMT_I386") {
            env.push(("SPROUT_BINFMT_I386".into(), qemu.clone()));
        }
    }
    if !env.iter().any(|(k, _)| k == "TERM") {
        let term = std::env::var("TERM").unwrap_or_else(|_| "xterm-256color".into());
        env.push(("TERM".into(), term));
    }
    if let Some(name) = &rootfs.user_name {
        if !env.iter().any(|(k, _)| k == "USER") {
            env.push(("USER".into(), name.clone()));
        }
        if !env.iter().any(|(k, _)| k == "LOGNAME") {
            env.push(("LOGNAME".into(), name.clone()));
        }
    }
    if !env.iter().any(|(k, _)| k == "SHELL") {
        let base = std::env::var("SHELL")
            .ok()
            .and_then(|s| s.rsplit('/').next().map(|b| b.to_string()))
            .filter(|b| !b.is_empty());
        /* `--user`: the passwd record's login shell wins over probing the
         * inherited host $SHELL basename — it IS the user's login shell. */
        let mut guest_shell: Option<String> = rootfs.user_shell.clone();
        if guest_shell.is_none() {
            if let Some(base) = base {
                for c in [format!("/bin/{base}"), format!("/usr/bin/{base}")] {
                    if rootfs
                        .guest_real(std::path::Path::new(&c))
                        .map(|p| p.is_file())
                        .unwrap_or(false)
                    {
                        guest_shell = Some(c);
                        break;
                    }
                }
            }
        }
        if guest_shell.is_none() {
            for c in ["/bin/bash", "/bin/sh"] {
                if rootfs
                    .guest_real(std::path::Path::new(c))
                    .map(|p| p.is_file())
                    .unwrap_or(false)
                {
                    guest_shell = Some(c.to_string());
                    break;
                }
            }
        }
        env.push((
            "SHELL".into(),
            guest_shell.unwrap_or_else(|| "/bin/sh".into()),
        ));
    }
    if !env.iter().any(|(k, _)| k == "TMPDIR") {
        env.push(("TMPDIR".into(), "/tmp".into()));
    }
    if !env.iter().any(|(k, _)| k == "OLDPWD") {
        env.push(("OLDPWD".into(), guest_home(rootfs)));
    }
    // X11/audio env policy (2026-08-13, user decision): INHERIT-ONLY by
    // default. sprout never invents DISPLAY/PULSE_SERVER — a host-set value
    // passes through (the user's explicit export), and the --termux-x11
    // preset flag injects the proot-distro-style bundle on request.
    // Why PULSE over TCP 127.0.0.1: the unix-socket path authenticates on
    // SO_PEERCRED, which BOTH proot and sprout spoof under the fake-id
    // anchor; the host pulse server distrusts a peer claiming its own uid.
    // TCP + auth-anonymous sidesteps cred spoofing (proot-distro's answer).
    if rootfs.termux_x11 && !env.iter().any(|(k, _)| k == "PULSE_SERVER") {
        env.push(("PULSE_SERVER".into(), "127.0.0.1".into()));
    }
    if !env.iter().any(|(k, _)| k == "DISPLAY") {
        if let Ok(d) = std::env::var("DISPLAY") {
            if !d.is_empty() {
                env.push(("DISPLAY".into(), d));
            }
        } else if rootfs.termux_x11 && rootfs.bindings.iter().any(|b| b.guest == *"/tmp") {
            // preset :0 needs the /tmp bind actually carrying the X socket;
            // main.rs warns when the flag is set without --shared-tmp.
            env.push(("DISPLAY".into(), ":0".into()));
        }
    }
}

/// Ensure the guest has a working POSIX-shm directory: python's
/// multiprocessing.SemLock (`shm_open(/dev/shm/sem.…)`) and assorted
/// tools die with ENOENT when /dev/shm is absent (proot-distro keeps a
/// writable one; mirrors it).
fn ensure_dev_shm(rootfs: &Rootfs) {
    let shm = rootfs.root.join("dev/shm");
    let _ = std::fs::create_dir_all(&shm);
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&shm, std::fs::Permissions::from_mode(0o1777));
    }
}

/// Guest-spelled canonical image path for the SPROUT_EXE stamp.
///
/// `guest_prog` is the host-absolute *resolved* path of the guest binary;
/// the stamp must be the GUEST spelling (rootfs prefix stripped) because
/// the readlinkat() interposer answers /proc/self/exe verbatim — a
/// host-spelled answer leaks '/data/data/…' into guests and a raw
/// launch-alias answer (the chain case) makes symlink-launched binaries
/// (firefox-esr -> ../lib/firefox-esr/firefox-esr) cut the wrong GRE dir
/// out of it ("Couldn't load XPCOM").
fn exe_guest_spelling(rootfs: &Rootfs, guest_prog: &std::path::Path) -> String {
    match guest_prog.strip_prefix(&rootfs.root) {
        Ok(rel) if rel.as_os_str().len() > 0 => format!("/{}", rel.display()),
        _ => guest_prog.display().to_string(),
    }
}

impl LaunchPlan {
    /// Build the preload launch plan for an already-classified dynamic ELF.
    ///
    /// * `guest_prog` — host-absolute path of the resolved guest binary
    /// * `preload_so` — host-absolute path of `libsprout-core.so`
    #[allow(clippy::too_many_arguments)]
    pub fn preload(
        rootfs: &Rootfs,
        guest_prog: PathBuf,
        args: &[OsString],
        preload_so: PathBuf,
        debug: bool,
        cache_dir: &std::path::Path,
    ) -> Result<Self, Error> {
        let flavor = rootfs.libc_flavor();
        let loader = rootfs.guest_loader()?;
        let library_path = rootfs.library_path();

        if flavor == LibcFlavor::Musl {
            /* musl: the ldso IS the libc. Its early init (and every musl
             * caller of set*id/faccessat!) crosses Android's blocked table
             * → svc→mov-x0-xzr sanitization (ADR-0007). musl dedups by
             * filename, so the shadow dir must host the artifact under
             * its real SONAME (libc.musl-aarch64.so.1) and lead the
             * --library-path order. */
            let loader = crate::sanitize::ensure_musl_shadow_ldso(&loader, cache_dir)
                .map_err(|e| crate::error::Error::Sanitize(e.to_string()))?;
            let library_path = format!("{}:{}", loader.parent().unwrap().display(), library_path);
            let mut argv: Vec<OsString> = vec![
                "--argv0".into(),
                args.first()
                    .cloned()
                    .unwrap_or_else(|| guest_prog.clone().into_os_string()),
                "--library-path".into(),
                library_path.clone().into(),
                guest_prog.clone().into_os_string(),
            ];
            argv.extend(args.iter().skip(1).cloned());

            let mut env = vec![
                ("PATH".into(), guest_path_for(rootfs)),
                ("SPROUT_ROOTFS".into(), rootfs.root.display().to_string()),
                ("LD_PRELOAD".into(), preload_so.display().to_string()),
                ("LD_LIBRARY_PATH".into(), library_path.clone()),
                ("SPROUT_LOADER".into(), loader.display().to_string()),
                ("SPROUT_LIBRARY_PATH".into(), library_path),
                ("SPROUT_EXE".into(), exe_guest_spelling(rootfs, &guest_prog)),
                ("SPROUT_LIBC".into(), "musl".into()),
                // Autoconf-style guests fork/exec hundreds of conftest binaries;
                // pre-binding every PLT entry on load prevents a class of
                // stationary hangs observed in dash's variable-hashing
                // paths under the lazy loader (≣`cqc` in bug reports).
                ("LD_BIND_NOW".into(), "1".into()),
            ];
            if !rootfs.bindings.is_empty() {
                env.push(("SPROUT_BIND".into(), rootfs.binds_env()));
            }
            if rootfs.fakeroot {
                env.push(("SPROUT_FAKEROOT".into(), "1".into()));
                /* --user anchor: the interposer's fake-id family answers
                 * from these instead of root (proot -i parity). */
                if let (Some(u), Some(g)) = (rootfs.fake_uid, rootfs.fake_gid) {
                    env.push(("SPROUT_FAKE_UID".into(), u.to_string()));
                    env.push(("SPROUT_FAKE_GID".into(), g.to_string()));
                }
            }
            if rootfs.link2symlink {
                env.push(("SPROUT_LINK2SYMLINK".into(), "1".into()));
            }
            if debug {
                env.push(("SPROUT_DEBUG".into(), "1".into()));
            }
            push_home_term(&mut env, rootfs);
            ensure_dev_shm(rootfs);
            let def_cwd = if rootfs.cwd.is_some() {
                rootfs.cwd.clone().unwrap()
            } else {
                guest_home(rootfs)
            };
            /* cwd must stay HOST-absolute: --host-home gives an already-host
             * path; feeding it through to_host() double-prefixes the root
             * ("os error 2" on launch). */
            let host_cwd = if rootfs.host_home {
                std::path::PathBuf::from(&def_cwd)
            } else {
                to_host_bound(rootfs, &def_cwd)
            };
            return Ok(Self {
                strategy: Strategy::Preload,
                loader,
                argv,
                env,
                /* proot parity: guest cwd defaults to the guest HOME
                 * (/root; proot-distro behavior). Inheriting the host cwd
                 * drops the child OUTSIDE the rootfs, and root-relative
                 * guest programs (apk!) then create junk like
                 * '/home/projeto/triggers.tmp.PID'. */
                cwd: Some(host_cwd),
                display: guest_prog.display().to_string(),
            });
        }

        // ADR-0007: sanitized libc copy with set_robust_list emulated.
        let guest_libc = rootfs.find_libc()?;
        let sanitized = crate::sanitize::ensure_sanitized_libc(&guest_libc, cache_dir)
            .map_err(|e| crate::error::Error::Sanitize(e.to_string()))?;

        // The loader is statically linked and carries its OWN early-init copy
        // of the blocked syscalls (verified: Devuan6 ld.so sites at 0x10618
        // and 0x106d0). Sanitize it the same way; this is the object the
        // kernel actually execs, so the fast path's entry point must be clean.
        let loader = crate::sanitize::ensure_sanitized_glibc(&loader, cache_dir, "ldso")
            .map_err(|e| crate::error::Error::Sanitize(e.to_string()))?;

        let mut argv: Vec<OsString> = vec![
            "--argv0".into(),
            args.first()
                .cloned()
                .unwrap_or_else(|| guest_prog.clone().into_os_string()),
            "--inhibit-cache".into(),
            "--library-path".into(),
            library_path.clone().into(),
            guest_prog.clone().into_os_string(),
        ];
        argv.extend(args.iter().skip(1).cloned());

        // Order matters: libsprout-core.so FIRST so its wrappers win symbol
        // resolution over libc (verified via dladdr probing: LD_PRELOAD is
        // resolved left-to-right).
        // SPROUT_EMU_SYSVIPC_PATH: host-emulator-lane arm64 SysV-IPC shim
        // gets prepended at the same phase (only consulted here; injects
        // into any aarch64-host gclib emulator process env).
        let loader_shim = std::env::var("SPROUT_EMU_SYSVIPC_PATH")
            .ok()
            .filter(|s| !s.is_empty());
        let ld_preload = match loader_shim {
            Some(s) => format!("{}:{}:{}", s, preload_so.display(), sanitized.display()),
            None => format!("{}:{}", preload_so.display(), sanitized.display()),
        };

        // Inheriting the host PATH leaks host dirs into the guest PATH
        // search (python's shutil.which, env, shells). proot-distro sets a
        // guest-sane PATH for the same reason. SPROUT_GUEST_PATH overrides.
        let mut env = vec![
            ("PATH".into(), guest_path_for(rootfs)),
            ("SPROUT_ROOTFS".into(), rootfs.root.display().to_string()),
            ("LD_PRELOAD".into(), ld_preload),
            ("LD_LIBRARY_PATH".into(), library_path.clone()),
            ("SPROUT_LOADER".into(), loader.display().to_string()),
            ("SPROUT_LIBRARY_PATH".into(), library_path),
            /* /proc/self/exe truth: the loader chain exec()s the sanitized
             * ld.so, so the kernel points self/exe at OUR launcher and any
             * guest computing its own dir from readlink(/proc/self/exe)
             * (mozilla "Couldn't load XPCOM") searches the launcher dir.
             * The preload interposer answers /proc/self/exe with this var;
             * chains re-stamp it per child exec. */
            ("SPROUT_EXE".into(), exe_guest_spelling(rootfs, &guest_prog)),
            // See musl branch note above: avoids conftest-hang class.
            ("LD_BIND_NOW".into(), "1".into()),
        ];
        if !rootfs.bindings.is_empty() {
            env.push(("SPROUT_BIND".into(), rootfs.binds_env()));
        }
        if rootfs.fakeroot {
            env.push(("SPROUT_FAKEROOT".into(), "1".into()));
            if let (Some(u), Some(g)) = (rootfs.fake_uid, rootfs.fake_gid) {
                env.push(("SPROUT_FAKE_UID".into(), u.to_string()));
                env.push(("SPROUT_FAKE_GID".into(), g.to_string()));
            }
        }
        if rootfs.link2symlink {
            env.push(("SPROUT_LINK2SYMLINK".into(), "1".into()));
        }
        if debug {
            env.push(("SPROUT_DEBUG".into(), "1".into()));
        }
        push_home_term(&mut env, rootfs);
        ensure_dev_shm(rootfs);

        /* same default: no host-cwd inheritance outside the rootfs. */
        let def_cwd = if rootfs.cwd.is_some() {
            rootfs.cwd.clone().unwrap()
        } else {
            guest_home(rootfs)
        };
        let cwd = if rootfs.host_home {
            Some(std::path::PathBuf::from(&def_cwd))
        } else {
            Some(to_host_bound(rootfs, &def_cwd))
        };

        Ok(Self {
            strategy: Strategy::Preload,
            loader,
            argv,
            env,
            cwd,
            display: guest_prog.display().to_string(),
        })
    }

    /// Render a shell-quoted preview for `--dry-run` / verbose output.
    pub fn explain(&self) -> String {
        let mut out = String::new();
        for (k, v) in &self.env {
            out.push_str(&format!("export {k}={v:?}\n"));
        }
        if let Some(c) = &self.cwd {
            out.push_str(&format!("cd {:?}\n", c));
        }
        out.push_str(&self.loader.display().to_string());
        for a in &self.argv {
            out.push(' ');
            out.push_str(&a.to_string_lossy());
        }
        out
    }

    /// Execute the plan in a foreground child and mirror its exit status.
    #[cfg(unix)]
    pub fn run(&self) -> Result<ExitStatus, Error> {
        use std::os::unix::process::CommandExt;

        let mut cmd = std::process::Command::new(&self.loader);
        cmd.arg0(&self.loader) // loader sees itself as argv[0]
            .args(&self.argv)
            .envs(self.env.iter().map(|(k, v)| (OsStr::new(k), OsStr::new(v))));
        if let Some(c) = &self.cwd {
            cmd.current_dir(c);
        }
        let status = cmd.status()?;
        Ok(status)
    }

    /// Build the supervisor launch plan for a static / preload-incapable
    /// guest binary (ADR-0002: ptrace is the last-resort fallback).
    ///
    /// Executes `sprout-ptrace -- <host-program> [args...]` with
    /// `SPROUT_ROOTFS` set so the supervisor's syscall-entry hook can
    /// translate absolute pathname arguments.
    #[allow(clippy::too_many_arguments)]
    pub fn supervisor(
        rootfs: &Rootfs,
        supervisor: PathBuf,
        guest_prog: PathBuf,
        display_program: &str,
        args: &[OsString],
        debug: bool,
        preload_so: PathBuf,
        cache_dir: &std::path::Path,
    ) -> Result<Self, Error> {
        let flavor = rootfs.libc_flavor();
        let mut argv: Vec<OsString> = vec!["--".into(), guest_prog.into_os_string()];
        argv.extend(args.iter().cloned());

        // The supervisor rewrites static→dynamic execve into the loader
        // chain; give it the same sanitized assets the preload plan uses.
        // Failure is non-fatal: without them, only static→static exec is
        // possible under the supervisor.
        let mut env: Vec<(String, String)> = vec![
            ("SPROUT_ROOTFS".into(), rootfs.root.display().to_string()),
            ("PATH".into(), guest_path_for(rootfs)),
        ];
        push_home_term(&mut env, rootfs);
        if flavor == LibcFlavor::Musl {
            /* musl under the supervisor: same chain mechanics but pointing
             * at the musl ld.so and the musl-built artifact; no sanitized
             * copy (musl early-init never crosses Android's blocked list). */
            if let Ok(loader) = rootfs.guest_loader() {
                let loader = crate::sanitize::ensure_musl_shadow_ldso(&loader, cache_dir)
                    .map_err(|e| crate::error::Error::Sanitize(e.to_string()))?;
                env.push(("SPROUT_LOADER".into(), loader.display().to_string()));
                env.push((
                    "SPROUT_LIBRARY_PATH".into(),
                    format!(
                        "{}:{}",
                        loader.parent().unwrap().display(),
                        rootfs.library_path()
                    ),
                ));
                env.push(("SPROUT_LIBC".into(), "musl".into()));
                env.push((
                    "SPROUT_GUEST_PRELOAD".into(),
                    preload_so.display().to_string(),
                ));
            }
        } else if let (Ok(loader), Ok(libc)) = (rootfs.guest_loader(), rootfs.find_libc()) {
            if let (Ok(loader_s), Ok(libc_s)) = (
                crate::sanitize::ensure_sanitized_glibc(&loader, cache_dir, "ldso"),
                crate::sanitize::ensure_sanitized_libc(&libc, cache_dir),
            ) {
                env.push(("SPROUT_LOADER".into(), loader_s.display().to_string()));
                env.push(("SPROUT_LIBRARY_PATH".into(), rootfs.library_path()));
                env.push((
                    "SPROUT_GUEST_PRELOAD".into(),
                    format!("{}:{}", preload_so.display(), libc_s.display()),
                ));
            }
        }
        if debug {
            env.push(("SPROUT_DEBUG".into(), "1".into()));
        }
        Ok(Self {
            strategy: Strategy::Ptrace,
            loader: supervisor,
            argv,
            env,
            cwd: None,
            display: display_program.to_string(),
        })
    }

    /// Ensure the supervisor's translation layers treat host Termux
    /// paths (loader dir, libc shadow cache, tmp) as pass-through. This
    /// matters once the ptrace supervisor installs its usernotify filter:
    /// without it, the guest loader's own absolute host paths would be
    /// double-prefixed with the guest rootfs.
    fn push_passthrough(env: &mut Vec<(String, String)>, prefixes: &[&str]) {
        let mut merged: Vec<String> = vec!["/proc".into(), "/sys".into(), "/dev".into()];
        if let Some((_, v)) = env.iter().find(|(k, _)| k == "SPROUT_PASSTHROUGH") {
            for part in v.split(';') {
                let part = part.trim();
                if !part.is_empty() && !merged.iter().any(|e| e == part) {
                    merged.push(part.to_string());
                }
            }
            env.retain(|(k, _)| k != "SPROUT_PASSTHROUGH");
        }
        for p in prefixes {
            if !p.is_empty() && p.starts_with('/') && !merged.iter().any(|e| e == p) {
                merged.push((*p).to_string());
            }
        }
        env.push(("SPROUT_PASSTHROUGH".into(), merged.join(";")));
    }

    /// Wrap an existing plan (normally a preload plan) under the ptrace
    /// supervisor. Used for Go-dynamic images: they are ET_DYN with
    /// PT_INTERP — so they still need the sanitized loader chain as the
    /// kernel launch vehicle — but their runtime issues syscalls DIRECTLY
    /// (bypassing libc), so only the supervisor can translate their path
    /// arguments at syscall-entry stops.
    pub fn supervise(mut plan: LaunchPlan, supervisor: PathBuf) -> LaunchPlan {
        let mut argv: Vec<OsString> = vec!["--".into()];
        argv.push(plan.loader.clone().into_os_string());
        argv.extend(plan.argv);
        /* musl-dynamic (kind 3) shadows exactly like glibc: its ldso-chain
         * interposer covers the PLT surface. (static musl / Go stay on
         * per-syscall stops — they never carry the interposer.) */
        if plan
            .env
            .iter()
            .any(|(k, v)| k == "SPROUT_LIBC" && v == "musl")
        {
            plan.env.push(("SPROUT_SHADOW".into(), "1".into()));
        }
        // The supervisor performs its exec rewrites using
        // SPROUT_GUEST_PRELOAD (interposer:sanitized-libc pair); the
        // preload plan holds exactly that pair in LD_PRELOAD.
        let mut pts: Vec<String> = Vec::new();
        for (k, v) in &plan.env {
            if k == "SPROUT_LOADER" || k == "SPROOT_LIBRARY_PATH" || k == "SPROUT_LIBRARY_PATH" {
                for part in v.split(':') {
                    if part.starts_with('/') {
                        pts.push(part.to_string());
                    }
                }
            }
            if k == "SPROUT_GUEST_PRELOAD" {
                for part in v.split(':') {
                    if let Some(dir) = std::path::Path::new(part).parent() {
                        pts.push(dir.display().to_string());
                    }
                }
            }
        }
        {
            let mut prefix = std::env::var("PREFIX")
                .unwrap_or_else(|_| "/data/data/com.termux/files/usr".into());
            pts.push(prefix);
            prefix =
                std::env::var("HOME").unwrap_or_else(|_| "/data/data/com.termux/files/home".into());
            pts.push(prefix);
        }
        {
            let refs: Vec<&str> = pts.iter().map(|s| s.as_str()).collect();
            Self::push_passthrough(&mut plan.env, &refs);
        }
        let has_guest_preload = plan.env.iter().any(|(k, _)| k == "SPROUT_GUEST_PRELOAD");
        if !has_guest_preload {
            if let Some((_, ld)) = plan.env.iter().find(|(k, _)| k == "LD_PRELOAD") {
                let v = ld.clone();
                plan.env.push(("SPROUT_GUEST_PRELOAD".into(), v));
            }
        }
        // The supervisor is a *host* binary (bionic on Android): it must
        // never see LD_PRELOAD / LD_LIBRARY_PATH at exec time, because the
        // host linker resolves them before main() and would try to link
        // glibc objects. The supervisor re-injects LD_PRELOAD into the
        // tracee child itself from SPROUT_GUEST_PRELOAD, and the guest lib
        // path travels in the chain argv via --library-path.
        plan.env
            .retain(|(k, _)| k != "LD_PRELOAD" && k != "LD_LIBRARY_PATH");
        plan.loader = supervisor;
        plan.argv = argv;
        plan.strategy = Strategy::Ptrace;
        plan
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    /// Minimal fake Ubuntu-ish rootfs: loader + one lib dir + sh.
    fn fake_rootfs() -> (::tempfile::TempDir, Rootfs) {
        let t = ::tempfile::tempdir().unwrap();
        let r = t.path();
        fs::create_dir_all(r.join("lib/aarch64-linux-gnu")).unwrap();
        fs::create_dir_all(r.join("usr/bin")).unwrap();
        fs::write(r.join("lib/ld-linux-aarch64.so.1"), fake_libc_bytes()).unwrap();
        fs::write(r.join("usr/bin/mytool"), b"fake").unwrap();
        set_exec(r.join("usr/bin/mytool"));
        fs::write(r.join("lib/aarch64-linux-gnu/libc.so.6"), fake_libc_bytes()).unwrap();
        let mut fsroot = Rootfs::new(r.to_path_buf()).unwrap();
        fsroot.cwd = Some("/root".into());
        (t, fsroot)
    }

    /// Same minimal ELF64 with a patchable set_robust_list site as the
    /// sanitize tests' fixture (kept self-contained here).
    fn fake_libc_bytes() -> Vec<u8> {
        let mut b = vec![0u8; 1024];
        b[0..4].copy_from_slice(b"\x7fELF");
        b[4] = 2;
        b[5] = 1;
        b[16..18].copy_from_slice(&3u16.to_le_bytes());
        b[32..40].copy_from_slice(&64u64.to_le_bytes());
        b[54..56].copy_from_slice(&56u16.to_le_bytes());
        b[56..58].copy_from_slice(&1u16.to_le_bytes());
        b[64..68].copy_from_slice(&1u32.to_le_bytes()); // PT_LOAD
        b[68..72].copy_from_slice(&5u32.to_le_bytes()); // PF_R|PF_X
        b[72..80].copy_from_slice(&256u64.to_le_bytes());
        b[96..104].copy_from_slice(&64u64.to_le_bytes());
        let text: [u32; 4] = [0xd2800c68, 0xf9400400, 0x91400000, 0xd4000001];
        for (i, w) in text.iter().enumerate() {
            b[256 + i * 4..260 + i * 4].copy_from_slice(&w.to_le_bytes());
        }
        b
    }

    #[cfg(unix)]
    fn set_exec(p: impl AsRef<std::path::Path>) {
        use std::os::unix::fs::PermissionsExt;
        let mut perm = std::fs::metadata(p.as_ref()).unwrap().permissions();
        perm.set_mode(0o755);
        std::fs::set_permissions(p, perm).unwrap();
    }

    #[test]
    fn plan_executes_through_guest_loader() {
        let (_t, rootfs) = fake_rootfs();
        let prog = rootfs.find_program("mytool").unwrap();
        let args = vec![OsString::from("mytool"), OsString::from("--flag")];
        let plan = LaunchPlan::preload(
            &rootfs,
            prog.clone(),
            &args,
            PathBuf::from("/so.so"),
            true,
            _t.path(),
        )
        .unwrap();

        assert_eq!(plan.strategy, Strategy::Preload);
        assert!(
            plan.loader
                .file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with("ldso-sanitized-"),
            "loader should be the sanitized copy: {:?}",
            plan.loader
        );
        // --argv0 preserves the original argv[0]
        let argv: Vec<_> = plan
            .argv
            .iter()
            .map(|a| a.to_string_lossy().into_owned())
            .collect();
        assert_eq!(&argv[0..2], &["--argv0", "mytool"]);
        assert!(argv.contains(&"--inhibit-cache".to_string()));
        assert_eq!(argv.last().unwrap(), "--flag");
        // env contract for the preload core
        let env: std::collections::HashMap<_, _> = plan.env.iter().cloned().collect();
        // LD_PRELOAD is sprout-core:sanitized-libc (ADR-0007), colon-separated
        let ld = &env["LD_PRELOAD"];
        assert!(ld.starts_with("/so.so:"), "LD_PRELOAD was {ld}");
        assert!(ld.contains("libc-sanitized-"));
        assert_eq!(env["SPROUT_LIBRARY_PATH"], env["LD_LIBRARY_PATH"]);
        assert!(env.contains_key("SPROUT_LOADER"));
        assert_eq!(env["SPROUT_ROOTFS"], rootfs.root.display().to_string());
        assert!(env.contains_key("LD_LIBRARY_PATH"));
    }

    #[test]
    fn explain_renders_all_env() {
        let (_t, rootfs) = fake_rootfs();
        let prog = rootfs.find_program("mytool").unwrap();
        let plan = LaunchPlan::preload(
            &rootfs,
            prog,
            &[OsString::from("x")],
            PathBuf::from("/s"),
            false,
            _t.path(),
        )
        .unwrap();
        let text = plan.explain();
        assert!(text.contains("LD_PRELOAD"));
        assert!(text.contains("ldso-sanitized-"));
    }

    /// Regression guard: the supervisor plan env MUST spell the prefix
    /// SPROUT_ (a single-letter typo like SPROOT_ROOTFS compiles fine but
    /// breaks the supervisor at runtime — burned one iteration live).
    #[test]
    fn supervisor_plan_env_spells_sprout_keys() {
        let (_t, rootfs) = fake_rootfs();
        let cache = ::tempfile::tempdir().unwrap();
        let plan = LaunchPlan::supervisor(
            &rootfs,
            PathBuf::from("/host/sprout-ptrace"),
            rootfs.find_program("mytool").unwrap(),
            "mytool",
            &[OsString::from("--flag")],
            false,
            PathBuf::from("/host/libsprout-core.so"),
            cache.path(),
        )
        .unwrap();
        let has = |k: &str| plan.env.iter().any(|(ek, _)| ek == k);
        assert!(has("SPROUT_ROOTFS"), "env must carry SPROUT_ROOTFS");
        assert!(has("SPROUT_LOADER"), "env must carry SPROUT_LOADER");
        assert!(has("SPROUT_LIBRARY_PATH"));
        assert!(has("SPROUT_GUEST_PRELOAD"));
        assert!(has("PATH"), "env must carry guest PATH default");
        for (k, _) in &plan.env {
            assert!(
                !k.starts_with("SPROOT"),
                "typo prefix SPROOT_ leaked into env: {k}"
            );
        }
        assert_eq!(plan.argv[0], OsString::from("--"));
    }
}
