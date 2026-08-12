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

/// Guest libc flavor: how early-init + sanitization + loader argv are shaped.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LibcFlavor {
    /// GNU libc — needs sanitized copies on Android ≥15 (ADR-0007).
    Glibc,
    /// musl (Alpine & friends) — clean startup on Android, ldso is the libc.
    Musl,
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
    /// pass the host $HOME through to the guest (default: HOME=/root,
    /// proot parity)
    pub host_home: bool,
    /// append the host $PREFIX/bin to the guest PATH (default: clean
    /// guest-only view)
    pub host_path: bool,
    /// `--user` stance: resolved guest identity the fake-id family anchors
    /// to (None = root anchor 0:0, proot -0 parity). Fake-id stays active
    /// (`fakeroot=true`) — only the anchor moves, like `proot -i`.
    pub fake_uid: Option<u32>,
    pub fake_gid: Option<u32>,
    pub user_name: Option<String>,
    pub user_home: Option<String>,
    pub user_shell: Option<String>,
}

impl Rootfs {
    /// Guest libc flavor detection: musl rootfses ship /lib/ld-musl-*.so.1
    /// (which IS the libc); glibc rootfses ship ld-linux sources.
    pub fn libc_flavor(&self) -> LibcFlavor {
        if self.to_host(Path::new("/lib/ld-musl-aarch64.so.1")).exists() {
            return LibcFlavor::Musl;
        }
        LibcFlavor::Glibc
    }

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
            host_home: false,
            host_path: false,
            fake_uid: None,
            fake_gid: None,
            user_name: None,
            user_home: None,
            user_shell: None,
        })
    }

    /// Resolve a `--user` spec `NAME[:GROUP]` / `UID[:GID]` against this
    /// rootfs' /etc/passwd (+ /etc/group). proot `-i` parity for the
    /// name-or-number grammar. Returns (uid, gid, name, home, shell).
    pub fn resolve_user(&self, spec: &str) -> Result<(u32, u32, String, String, String), Error> {
        let (u_part, g_part) = match spec.split_once(':') {
            Some((u, g)) => (u, Some(g)),
            None => (spec, None),
        };
        if u_part.is_empty() {
            return Err(Error::UnknownUser(spec.to_string()));
        }
        let passwd_path = self.root.join("etc/passwd");
        let passwd = std::fs::read_to_string(&passwd_path)
            .map_err(|_| Error::UnknownUser(spec.to_string()))?;
        let mut hit: Option<(&str, &str, u32, u32)> = None; // (name, home, uid, gid)
        let mut shell_of = String::new();
        for line in passwd.lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let f: Vec<&str> = line.split(':').collect();
            if f.len() < 7 {
                continue;
            }
            let (name, uid_s, gid_s) = (f[0], f[2], f[3]);
            let (Ok(uid), Ok(gid)) = (uid_s.parse::<u32>(), gid_s.parse::<u32>()) else {
                continue;
            };
            let matches = if u_part.chars().all(|c| c.is_ascii_digit()) {
                uid.to_string() == u_part
            } else {
                name == u_part
            };
            if matches {
                shell_of = f[6].to_string();
                hit = Some((name, f[5], uid, gid));
                break;
            }
        }
        let Some((name, home, uid, mut gid)) = hit else {
            return Err(Error::UnknownUser(spec.to_string()));
        };
        if let Some(g) = g_part {
            if g.chars().all(|c| c.is_ascii_digit()) {
                gid = g.parse::<u32>()
                    .map_err(|_| Error::UnknownUser(spec.to_string()))?;
            } else {
                let group_path = self.root.join("etc/group");
                let groups = std::fs::read_to_string(&group_path)
                    .map_err(|_| Error::UnknownUser(spec.to_string()))?;
                let mut ghit = None;
                for line in groups.lines() {
                    let f: Vec<&str> = line.split(':').collect();
                    if f.len() >= 3 && f[0] == g {
                        ghit = f[2].parse::<u32>().ok();
                        break;
                    }
                }
                gid = ghit.ok_or_else(|| Error::UnknownUser(spec.to_string()))?;
            }
        }
        let shell = if shell_of.is_empty() {
            "/bin/sh".to_string()
        } else {
            shell_of
        };
        /* passwd homes like /nonexistent (nobody) would kill the launch
         * on cwd ENOENT — fall back to guest / (proot-distro behaves the
         * same when su(1) lands there). */
        let home = if self.guest_real(std::path::Path::new(home)).map(|p| p.is_dir()).unwrap_or(false) {
            home.to_string()
        } else {
            "/".to_string()
        };
        Ok((uid, gid, name.to_string(), home, shell))
    }

    /// Guest-spelled path → host path (root prefixing only; bindings are
    /// resolved inside the guest by the preload core).
    pub fn to_host(&self, guest: &Path) -> PathBuf {
        debug_assert!(guest.is_absolute());
        self.root.join(guest.strip_prefix("/").unwrap_or(guest))
    }

    /// Resolve `guest` (absolute path inside THIS rootfs) through symlink
    /// chains *relative to the rootfs*, not the host root. Bare
    /// fs::metadata() evaluates an absolute symlink like `bin/ls -&gt;
    /// /bin/busybox` against the HOST's /bin — dead under proot-distro
    /// Alpine, where every applet is exactly that link.
    pub fn guest_real(&self, guest_abs: &Path) -> Option<PathBuf> {
        let mut cur = guest_abs.to_path_buf();
        for _ in 0..8 {
            let host = self.to_host(&cur);
            let md = std::fs::symlink_metadata(&host).ok()?;
            if !md.file_type().is_symlink() {
                return Some(host);
            }
            let t = std::fs::read_link(&host).ok()?;
            cur = if t.is_absolute() { t } else { cur.parent().unwrap_or(Path::new("/")).join(t) };
        }
        None
    }

    /// Locate a bare command name against the standard guest PATH dirs.
    pub fn find_program(&self, name: &str) -> Result<PathBuf, Error> {
        if name.contains('/') {
            let guest = Path::new(name);
            if self.guest_real(guest).is_some() {
                return Ok(self.to_host(guest));
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
            let guest = Path::new(d).join(name);
            if let Some(host) = self.guest_real(&guest) {
                if is_executable(&host) {
                    return Ok(self.to_host(&guest));
                }
            }
        }
        Err(Error::ProgramNotFound(name.to_string()))
    }

    /// Dereference a host path that is a symlink chain ending in an
    /// absolute (guest-spelled) target, re-mapping it back into the rootfs.
    /// Busybox alpine: /bin/echo -> /bin/busybox style. Up to 8 hops.
    pub fn resolve_absolute_symlink(&self, host: &Path) -> Option<PathBuf> {
        let mut cur = host.to_path_buf();
        for _ in 0..8 {
            let md = std::fs::symlink_metadata(&cur).ok()?;
            if !md.file_type().is_symlink() {
                return Some(cur);
            }
            let target = std::fs::read_link(&cur).ok()?;
            if target.is_absolute() {
                cur = self.to_host(&target);
            } else {
                cur = cur.parent()?.join(target);
            }
        }
        None
    }

    /// Locate the guest glibc dynamic loader (Ubuntu arm64 spellings).
    pub fn guest_loader(&self) -> Result<PathBuf, Error> {
        if self.libc_flavor() == LibcFlavor::Musl {
            let p = self.to_host(Path::new("/lib/ld-musl-aarch64.so.1"));
            if p.exists() {
                return Ok(p);
            }
        }
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
