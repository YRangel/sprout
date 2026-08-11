use thiserror::Error;

use crate::elf::ElfError;

#[derive(Debug, Error)]
pub enum Error {
    #[error("rootfs '{0}' does not exist or is not a directory")]
    RootfsMissing(String),

    #[error("invalid binding '{0}'; expected 'host[:guest]' with absolute paths")]
    BadBinding(String),

    #[error("guest program '{0}' not found inside rootfs (searched standard PATH dirs)")]
    ProgramNotFound(String),

    #[error("guest program '{program}' is {class:?}; sprout v0.1 runs 64-bit ELF binaries only. Shebang scripts (#!) land in v0.2 via interpreter-dir parsing; 32-bit support is not planned (ADR-0005).")]
    UnsupportedElf {
        program: String,
        class: crate::GuestClass,
    },

    #[error(
        "guest program '{program}' is statically linked; LD_PRELOAD cannot interpose it. \
             Re-run with --fallback=ptrace (v0.3) or use a dynamic build."
    )]
    StaticNeedsPtrace { program: String },

    #[error("--fallback=ptrace is not implemented yet (tracked for v0.3)")]
    PtraceUnimplemented,

    #[error("guest loader not found: tried {tried:?} inside rootfs")]
    LoaderMissing { tried: Vec<String> },

    #[error(transparent)]
    Elf(#[from] ElfError),

    #[error(transparent)]
    Io(#[from] std::io::Error),
}
