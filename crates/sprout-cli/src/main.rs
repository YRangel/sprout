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
use sprout_core::{classify, Binding, Error, GuestClass, LaunchPlan, Rootfs, Strategy};

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

    /// Fake uid/gid 0 (guest sees itself as root).
    #[arg(short = '0', long = "root-id", default_value_t = false)]
    root_id: bool,

    /// Convert hardlinks to symlinks on guest writes.
    #[arg(long = "link2symlink", default_value_t = false)]
    link2symlink: bool,

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

fn run() -> Result<u8, Error> {
    let cli = Cli::parse();

    let mut rootfs = Rootfs::new(cli.rootfs)?;
    rootfs.cwd = cli.cwd;
    rootfs.fakeroot = cli.root_id;
    rootfs.link2symlink = cli.link2symlink;
    for spec in &cli.binds {
        rootfs.bindings.push(Binding::parse(spec)?);
    }

    let program_name = cli.cmd[0].to_string_lossy();
    let program_host = rootfs.find_program(&program_name)?;

    let class = classify(&program_host)?;
    let strategy = match cli.fallback.as_str() {
        "auto" => Strategy::for_elf(&class).ok_or(Error::UnsupportedElf {
            program: program_name.clone().into_owned(),
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
                    program: program_name.clone().into_owned(),
                });
            }
        }
    }

    if strategy == Strategy::Ptrace {
        return Err(Error::PtraceUnimplemented);
    }

    let preload_so = sprout_preload::core_library_path().ok_or(Error::PreloadNotFound)?;

    let plan = LaunchPlan::preload(&rootfs, program_host, &cli.cmd, preload_so, cli.verbose)?;

    if cli.dry_run {
        eprintln!("{}", plan.explain());
        return Ok(0);
    }

    let status = plan.run()?;
    Ok(status.code().map(|c| (c % 256) as u8).unwrap_or(1))
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
