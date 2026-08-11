//! Discovery for the ptrace supervisor binary built by this crate's build script.

use std::env;
use std::path::{Path, PathBuf};

/// Absolute path to the `sprout-ptrace` helper.
///
/// Order: `$SPROOT_PTRACE_PATH` → sibling of argv[0] → the path baked in
/// at build time (cargo OUT_DIR). None when nothing was built (never on
/// bionic/Android — the supervisor always builds there).
pub fn supervisor_path() -> Option<PathBuf> {
    if let Ok(p) = env::var("SPROOT_PTRACE_PATH") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join("sprout-ptrace");
            if p.is_file() {
                return Some(p);
            }
        }
    }
    let built = env!("SPROOT_PTRACE_EXE");
    if !built.is_empty() {
        let p = PathBuf::from(built);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builds_on_android_host() {
        let p = supervisor_path().expect("ptrace supervisor must build on this host");
        let magic = std::fs::read(&p).expect("read exe");
        assert_eq!(&magic[..4], b"\x7fELF");
        // AArch64 ELF machine type is 0xB7 (183)
        let machine = u16::from_le_bytes([magic[18], magic[19]]);
        assert_eq!(machine, 183, "not aarch64");
    }
}
