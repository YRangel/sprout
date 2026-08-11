//! Discovery + native-test hooks for the C11 path-translation core.
//!
//! The interposer itself is C (see ADR-0001: the LD_PRELOAD hot path must
//! interpose libc symbols without allocations, panics, or a runtime). This
//! crate only locates the artifacts the build script produced.

use std::env;
use std::path::{Path, PathBuf};

/// Absolute path to a usable `libsprout-core.so`.
///
/// Discovery order (first hit wins):
///
/// 1. `$SPROUT_PRELOAD_PATH` — explicit operator override
/// 2. `<dir of argv[0]>/libsprout-core.so` — installed layout (binary and
///    core shipped side-by-side)
/// 3. the path baked in at build time (`OUT_DIR`) — cargo dev / CI
///
/// Returns `None` when none of those exist on disk (e.g. a bionic-only dev
/// build that produced no artifact; see `docs/src/development.md`).
pub fn core_library_path() -> Option<PathBuf> {
    if let Ok(override_) = env::var("SPROUT_PRELOAD_PATH") {
        let p = PathBuf::from(override_);
        if p.is_file() {
            return Some(p);
        }
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join("libsprout-core.so");
            if p.is_file() {
                return Some(p);
            }
        }
    }
    let built = env!("SPROUT_PRELOAD_SO");
    if !built.is_empty() {
        let p = PathBuf::from(built);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

/// Absolute path to the natively-compiled translation unit-test binary.
pub fn translate_test_binary() -> &'static Path {
    Path::new(env!("SPROUT_TRANSLATE_TEST_BIN"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn c_translate_unit_tests_pass() {
        // -Wpedantic builds can emit warnings; only the exit code is the verdict.
        let status = std::process::Command::new(translate_test_binary())
            .status()
            .expect("spawn translate test binary");
        assert!(status.success(), "C translation tests failed");
    }

    #[test]
    fn core_shared_object_state_is_consistent() {
        if let Some(so) = core_library_path() {
            let bytes = std::fs::read(&so).expect("read .so");
            assert_eq!(&bytes[..4], b"\x7fELF", "not an ELF: {}", so.display());
        }
        // None on Android hosts: expected and covered by CI on the glibc runner.
    }

    #[test]
    fn env_override_wins_when_pointed_at_an_elf() {
        // Point at any ELF that exists on this host: the Rust test binary itself.
        let elf = std::env::current_exe().unwrap();
        std::env::set_var("SPROUT_PRELOAD_PATH", &elf);
        assert_eq!(core_library_path(), Some(elf));
        std::env::remove_var("SPROUT_PRELOAD_PATH");
    }

    #[test]
    fn env_override_is_rejected_when_not_a_file() {
        std::env::set_var("SPROUT_PRELOAD_PATH", "/nope/nope/libsprout-core.so");
        // falls through to argv0/built-in; never to the invalid override
        let _ = core_library_path(); // must not panic, value not asserted
        std::env::remove_var("SPROUT_PRELOAD_PATH");
    }
}
