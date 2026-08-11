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

    let mut program_name = cli.cmd[0].to_string_lossy().into_owned();
    let mut program_host = rootfs.find_program(&program_name)?;

    // Full argv passed to the plan (argv[0] included, matching kernel exec):
    // for shebang scripts this becomes [interp, opt?, script, orig args...].
    let mut full_cmd: Vec<OsString> = cli.cmd.clone();

    let mut class = classify(&program_host)?;

    // Shebang (#!): exec the script's interpreter instead (kernel semantics:
    // argv = [interp, opt-arg?, script, orig args...]). The interpreter must
    // itself be a guest binary on PATH (supports '#!/usr/bin/env X' too).
    if matches!(class, GuestClass::NotElf) {
        if let Some((interp, opt)) = parse_shebang(&program_host) {
            let interp_host = rootfs.find_program(&interp).map_err(|_| Error::UnsupportedElf {
                program: program_name.clone(),
                class: class.clone(),
            })?;
            let interp_class = classify(&interp_host)?;
            let script_guest = program_name.clone();
            let orig_args: Vec<OsString> = cli.cmd[1..].to_vec();
            let mut rebuilt: Vec<OsString> = vec![interp.clone().into()];
            if let Some(o) = opt {
                rebuilt.push(o.into());
            }
            rebuilt.push(script_guest.into());
            rebuilt.extend(orig_args);
            full_cmd = rebuilt;
            program_host = interp_host;
            program_name = interp;
            class = interp_class;
        }
    }
    let strategy = match cli.fallback.as_str() {
        "auto" => Strategy::for_elf(&class).ok_or(Error::UnsupportedElf {
            program: program_name.clone(),
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
                    program: program_name.clone(),
                });
            }
        }
    }

    let plan = if strategy == Strategy::Ptrace {
        /* Last-resort path (ADR-0002): supervisor translates syscall args
         * for static / preload-incapable images, and rewrites static→dynamic
         * exec into the sanitized loader chain. */
        let supervisor = sprout_ptrace::supervisor_path().ok_or(Error::PtraceUnimplemented)?;
        let preload_so = sprout_preload::core_library_path().ok_or(Error::PreloadNotFound)?;
        let cache_dir = cache_dir();
        LaunchPlan::supervisor(
            &rootfs,
            supervisor,
            program_host,
            &program_name,
            &cli.cmd,
            cli.verbose,
            preload_so,
            &cache_dir,
        )?
    } else {
        let preload_so = sprout_preload::core_library_path().ok_or(Error::PreloadNotFound)?;
        let cache_dir = cache_dir();
        LaunchPlan::preload(
            &rootfs,
            program_host,
            &full_cmd,
            preload_so,
            cli.verbose,
            &cache_dir,
        )?
    };

    if cli.dry_run {
        eprintln!("{}", plan.explain());
        return Ok(0);
    }

    let status = plan.run()?;
    Ok(status.code().map(|c| c as u8).unwrap_or(1))
}

/// Where the sanitized-libc cache lives (ADR-0007). Override with
/// SPROUT_CACHE_DIR; defaults to $HOME/.cache/sprout, or the system temp
/// dir when HOME is unset.
fn cache_dir() -> PathBuf {
    if let Ok(p) = std::env::var("SPROUT_CACHE_DIR") {
        return PathBuf::from(p);
    }
    if let Ok(home) = std::env::var("HOME") {
        return PathBuf::from(home).join(".cache").join("sprout");
    }
    std::env::temp_dir().join("sprout")
}

/// Parse a guest script's shebang line: `#!/path/to/interp [one-opt-arg]`.
///
/// Kernel rules (fs/binfmt_script.c): everything on the first line after
/// `#!` up to whitespace separates interpreter and ONE optional argument.
/// `/usr/bin/env FOO` needs no special-casing: env resolves FOO on the
/// guest PATH via the interposer's `execvp` chain.
fn parse_shebang(host_path: &std::path::Path) -> Option<(String, Option<String>)> {
    use std::io::{BufRead, BufReader};
    let f = std::fs::File::open(host_path).ok()?;
    let mut first = String::new();
    BufReader::new(f).read_line(&mut first).ok()?;
    let line = first.strip_prefix("#!")?.trim();
    let mut it = line.split_whitespace();
    let interp = it.next()?.to_string();
    if interp.is_empty() {
        return None;
    }
    let opt = it.next().map(std::string::ToString::to_string);
    Some((interp, opt))
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
