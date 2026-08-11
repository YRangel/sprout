//! Builds the ptrace supervisor as a native executable.
//!
//! Unlike the preload `.so`, the supervisor:
//!   1. has its own `main` (it is a process, not a library)
//!   2. runs on the *host* (so bionic aarch64 from Termux clang is fine)

use std::env;
use std::path::PathBuf;

fn main() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR set by cargo"));
    let exe = out_dir.join("sprout-ptrace");

    println!("cargo:rerun-if-changed=csrc/sprout_ptrace.c");
    println!("cargo:rerun-if-changed=csrc/sprout_preload.h");
    println!("cargo:rerun-if-changed=../sprout-preload/csrc/sprout_preload.h");

    let build = cc::Build::new();
    let cc_tool = build.get_compiler();

    let mut cmd = cc_tool.to_command();
    cmd.arg("-std=c11")
        .arg("-O2")
        .arg("-Wall")
        .arg("-Wextra")
        .arg("-D_GNU_SOURCE")
        .arg("-DSPROOT_HOST_BUILD")
        .arg("-o")
        .arg(&exe)
        .arg("csrc/sprout_ptrace.c")
        .arg("../sprout-preload/csrc/sprout_preload.c"); // pure translation only

    let status = cmd
        .status()
        .unwrap_or_else(|e| panic!("failed to spawn compiler: {e}"));
    if !status.success() {
        panic!("compiler failed building sprout-ptrace: {status}");
    }

    println!("cargo:rustc-env=SPROOT_PTRACE_EXE={}", exe.display());
}
