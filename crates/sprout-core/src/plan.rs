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
        let loader = rootfs.guest_loader()?;
        let library_path = rootfs.library_path();

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
        let ld_preload = format!("{}:{}", preload_so.display(), sanitized.display());
        let mut env = vec![
            ("SPROUT_ROOTFS".into(), rootfs.root.display().to_string()),
            ("LD_PRELOAD".into(), ld_preload),
            ("LD_LIBRARY_PATH".into(), library_path.clone()),
            ("SPROUT_LOADER".into(), loader.display().to_string()),
            ("SPROUT_LIBRARY_PATH".into(), library_path),
        ];
        if !rootfs.bindings.is_empty() {
            env.push(("SPROUT_BIND".into(), rootfs.binds_env()));
        }
        if rootfs.fakeroot {
            env.push(("SPROUT_FAKEROOT".into(), "1".into()));
        }
        if rootfs.link2symlink {
            env.push(("SPROUT_LINK2SYMLINK".into(), "1".into()));
        }
        if debug {
            env.push(("SPROUT_DEBUG".into(), "1".into()));
        }

        let cwd = rootfs
            .cwd
            .as_ref()
            .map(|c| rootfs.to_host(std::path::Path::new(c)));

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
        fs::write(
            r.join("lib/aarch64-linux-gnu/libc.so.6"),
            fake_libc_bytes(),
        )
        .unwrap();
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
        let plan = LaunchPlan::preload(&rootfs, prog.clone(), &args, PathBuf::from("/so.so"), true, _t.path())
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
}
