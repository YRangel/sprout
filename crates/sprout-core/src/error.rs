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

    #[error("guest program '{program}' is {class:?}; shebang scripts are supported when the interpreter resolves inside the guest (this one did not); 32-bit support is not planned (ADR-0005).")]
    UnsupportedElf {
        program: String,
        class: crate::GuestClass,
    },

    #[error(
        "guest program '{program}' is statically linked; LD_PRELOAD cannot interpose it. \
             Re-run with --fallback=ptrace (v0.3) or use a dynamic build."
    )]
    StaticNeedsPtrace { program: String },

    #[error("cli error: {0}")]
    Cli(String),

    #[error("--fallback=ptrace is not implemented yet (tracked for v0.3)")]
    PtraceUnimplemented,

    #[error("guest loader not found: tried {tried:?} inside rootfs")]
    LoaderMissing { tried: Vec<String> },

    #[error(
        "binfmt: no emulator for foreign-arch target '{program}' (set SPROUT_BINFMT_X86_64/SPROUT_BINFMT_I386 or install box64 at {emu})"
    )]
    BinfmtNoEmulator { program: String, emu: String },

    #[error("libsprout-core.so not found. Either set SPROUT_PRELOAD_PATH=/path/to/libsprout-core.so, install it beside the `sprout` binary, or build it on a glibc host (docs/src/development.md).")]
    PreloadNotFound,

    #[error("libc sanitization failed: {0}")]
    Sanitize(String),

    #[error("guest user '{0}' not found in rootfs' /etc/passwd (or unknown group)")]
    UnknownUser(String),

    #[error(transparent)]
    Elf(#[from] ElfError),

    #[error(transparent)]
    Io(#[from] std::io::Error),
}
