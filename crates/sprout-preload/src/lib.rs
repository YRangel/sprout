//! Discovery + native-test hooks for the C11 path-translation core.
//!
//! The interposer itself is C (see ADR-0001: the LD_PRELOAD hot path must
//! interpose libc symbols without allocations, panics, or a runtime). This
//! crate only locates the artifacts the build script produced.

use std::path::Path;

/// Absolute path to the built `libsprout-core.so`, or `None` when built on
/// an Android host (the `.so` must come from a glibc toolchain — see
/// `docs/src/development.md` and ADR-0004).
pub fn core_library_path() -> Option<&'static Path> {
    let p = env!("SPROUT_PRELOAD_SO");
    if p.is_empty() {
        None
    } else {
        Some(Path::new(p))
    }
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
            let bytes = std::fs::read(so).expect("read .so");
            assert_eq!(&bytes[..4], b"\x7fELF", "not an ELF: {}", so.display());
        }
        // None on Android hosts: expected and covered by CI on the glibc runner.
    }
}
