//! Builds the ptrace supervisor as a native executable.
//!
//! Unlike the preload `.so`, the supervisor:
//!   1. has its own `main` (it is a process, not a library)
//!   2. runs on the *host* (so bionic aarch64 from Termux clang is fine)

use std::env;
use std::path::PathBuf;

fn main() {
    /* The supervisor handles aarch64 ptrace registers and the stub is raw
     * aarch64 svc asm — both exist only for aarch64 guests. On non-aarch64
     * hosts (ubuntu-latest lint/CI) skip cleanly, like sprout-preload's
     * glibc gate; lib.rs resolves None when nothing was built. */
    if !env::var("TARGET").unwrap_or_default().contains("aarch64") {
        println!(
            "cargo:warning=sprout-ptrace: aarch64-only C artifacts skipped on non-aarch64 target"
        );
        return;
    }
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR set by cargo"));
    let exe = out_dir.join("sprout-super");

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
        panic!("compiler failed building sprout-super: {status}");
    }

    /* ADR-0016: the freestanding notify-statics parasite. No libc; the
     * high image base keeps it from colliding with ET_EXEC guests. */
    println!("cargo:rerun-if-changed=csrc/sprout_stub.c");
    let stub = out_dir.join("sprout-stub");
    let mut scmd = cc_tool.to_command();
    scmd.arg("-static")
        .arg("-nostdlib")
        .arg("-nostartfiles")
        .arg("-Os")
        .arg("-fno-stack-protector")
        .arg("-Wl,--image-base=0x70000000")
        .arg("-Wl,--no-dynamic-linker")
        .arg("-o")
        .arg(&stub)
        .arg("csrc/sprout_stub.c");
    let sstatus = scmd
        .status()
        .unwrap_or_else(|e| panic!("failed to spawn stub compiler: {e}"));
    if !sstatus.success() {
        panic!("compiler failed building sprout-stub: {sstatus}");
    }

    println!("cargo:rustc-env=SPROOT_PTRACE_EXE={}", exe.display());
    println!("cargo:rustc-env=SPROUT_STUB_EXE={}", stub.display());
}
