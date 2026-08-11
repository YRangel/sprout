//! Rootfs + bindings: the value the whole launch is derived from.

use std::path::{Path, PathBuf};

use crate::error::Error;

/// One `-b host[:guest]` mapping, canonicalized (absolute, no trailing slash).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Binding {
    pub host: PathBuf,
    pub guest: PathBuf,
}

impl Binding {
    /// Parse proot-style `-b` syntax: `host`, `host:guest`. A bare `host`
    /// means "make this host dir visible at the same path in the guest".
    pub fn parse(spec: &str) -> Result<Self, Error> {
        let (host, guest) = match spec.split_once(':') {
            Some((h, g)) => (h, g),
            None => (spec, spec),
        };
        if !host.starts_with('/') || !guest.starts_with('/') || host.is_empty() || guest.is_empty()
        {
            return Err(Error::BadBinding(spec.to_string()));
        }
        Ok(Self {
            host: PathBuf::from(trim(host)),
            guest: PathBuf::from(trim(guest)),
        })
    }
}

fn trim(p: &str) -> String {
    if p.len() > 1 {
        p.trim_end_matches('/').to_string()
    } else {
        p.to_string()
    }
}

/// A validated guest root plus launch-time options.
#[derive(Debug, Clone)]
pub struct Rootfs {
    /// Absolute host path of the guest root.
    pub root: PathBuf,
    pub bindings: Vec<Binding>,
    /// Guest working directory (`-w`), guest-spelled.
    pub cwd: Option<String>,
    pub fakeroot: bool,
    pub link2symlink: bool,
}

impl Rootfs {
    pub fn new(root: PathBuf) -> Result<Self, Error> {
        if !root.is_dir() {
            return Err(Error::RootfsMissing(root.display().to_string()));
        }
        Ok(Self {
            root,
            bindings: Vec::new(),
            cwd: None,
            fakeroot: false,
            link2symlink: false,
        })
    }

    /// Guest-spelled path → host path (root prefixing only; bindings are
    /// resolved inside the guest by the preload core).
    pub fn to_host(&self, guest: &Path) -> PathBuf {
        debug_assert!(guest.is_absolute());
        self.root.join(guest.strip_prefix("/").unwrap_or(guest))
    }

    /// Locate a bare command name against the standard guest PATH dirs.
    pub fn find_program(&self, name: &str) -> Result<PathBuf, Error> {
        if name.contains('/') {
            let p = self.to_host(Path::new(name));
            if p.exists() {
                return Ok(p);
            }
            return Err(Error::ProgramNotFound(name.to_string()));
        }
        const DIRS: [&str; 6] = [
            "/usr/local/sbin",
            "/usr/local/bin",
            "/usr/sbin",
            "/usr/bin",
            "/sbin",
            "/bin",
        ];
        for d in DIRS {
            let host = self.to_host(Path::new(d)).join(name);
            if is_executable(&host) {
                return Ok(host);
            }
        }
        Err(Error::ProgramNotFound(name.to_string()))
    }

    /// Locate the guest glibc dynamic loader (Ubuntu arm64 spellings).
    pub fn guest_loader(&self) -> Result<PathBuf, Error> {
        const CANDIDATES: [&str; 3] = [
            "/lib/ld-linux-aarch64.so.1",
            "/lib64/ld-linux-aarch64.so.1",
            "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
        ];
        for c in CANDIDATES {
            let p = self.to_host(Path::new(c));
            if p.exists() {
                return Ok(p);
            }
        }
        Err(Error::LoaderMissing {
            tried: CANDIDATES.iter().map(|s| s.to_string()).collect(),
        })
    }

    /// Host-absolute library search path for the guest loader, built from
    /// dirs that actually exist in the rootfs.
    pub fn library_path(&self) -> String {
        const DIRS: [&str; 6] = [
            "/lib/aarch64-linux-gnu",
            "/usr/lib/aarch64-linux-gnu",
            "/usr/local/lib/aarch64-linux-gnu",
            "/lib64",
            "/lib",
            "/usr/lib",
        ];
        DIRS.iter()
            .map(Path::new)
            .map(|d| self.to_host(d))
            .filter(|p| p.is_dir())
            .map(|p| p.display().to_string())
            .collect::<Vec<_>>()
            .join(":")
    }

    /// Locate the guest's libc.so.6 (a real shared object, not the
    /// `/usr/lib/.../libc.so` linker script).
    pub fn find_libc(&self) -> Result<PathBuf, Error> {
        const CANDIDATES: [&str; 4] = [
            "/lib/aarch64-linux-gnu/libc.so.6",
            "/usr/lib/aarch64-linux-gnu/libc.so.6",
            "/lib64/libc.so.6",
            "/lib/libc.so.6",
        ];
        for c in CANDIDATES {
            let p = self.to_host(Path::new(c));
            if p.is_file() {
                return Ok(p);
            }
        }
        Err(Error::LoaderMissing {
            tried: CANDIDATES.iter().map(|s| s.to_string()).collect(),
        })
    }

    /// Serialized `SPROUT_BIND` value consumed by the preload core.
    pub fn binds_env(&self) -> String {
        self.bindings
            .iter()
            .map(|b| format!("{}={}", b.host.display(), b.guest.display()))
            .collect::<Vec<_>>()
            .join(";")
    }
}

#[cfg(unix)]
fn is_executable(p: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    p.metadata()
        .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

#[cfg(not(unix))]
fn is_executable(p: &Path) -> bool {
    p.is_file()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_binding_forms() {
        let b = Binding::parse("/sdcard").unwrap();
        assert_eq!(b.host, PathBuf::from("/sdcard"));
        assert_eq!(b.guest, PathBuf::from("/sdcard"));

        let b = Binding::parse("/sdcard:/mnt/sd").unwrap();
        assert_eq!(b.guest, PathBuf::from("/mnt/sd"));

        assert!(Binding::parse("rel/path").is_err());
        assert!(Binding::parse("/h:rel").is_err());
        assert!(Binding::parse("").is_err());
    }

    #[test]
    fn binding_trailing_slash_trimmed() {
        let b = Binding::parse("/sd/:/g/").unwrap();
        assert_eq!(b.guest, PathBuf::from("/g"));
    }
}
