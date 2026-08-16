//! Builds the C11 LD_PRELOAD core into `libsprout-core.so`.
//!
//! Gating: the `.so` must target *glibc aarch64* (it is loaded by the
//! guest's ld.so). Building on a bionic host (Termux) would silently produce
//! an unusable artifact, so on Android hosts we skip the link and let the
//! Rust API report `None`; CI builds the real artifact on ubuntu-24.04-arm.
//!
//! A native unit-test binary for the pure translation functions is always
//! compiled (no interposition), so `cargo test` exercises the C code on any
//! host, including Termux.

use std::env;
use std::path::PathBuf;
use std::process::Command;

fn compiler_flags(cmd: &mut Command) {
    cmd.arg("-std=c11")
        .arg("-O2")
        .arg("-Wall")
        .arg("-Wextra")
        .arg("-Wpedantic")
        .arg("-D_GNU_SOURCE");
}

fn main() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR set by cargo"));
    for f in [
        "csrc/sprout_preload.c",
        "csrc/sprout_preload.h",
        "csrc/tests/test_translate.c",
    ] {
        println!("cargo:rerun-if-changed={f}");
    }

    let build = cc::Build::new();
    let cc_tool = build.get_compiler();

    // --- 1. unit-test binary (always built: pure code, no interposition) ---
    let test_bin = out_dir.join("sp_translate_test");
    let mut test_cmd = cc_tool.to_command();
    compiler_flags(&mut test_cmd);
    test_cmd
        .arg("-o")
        .arg(&test_bin)
        .arg("csrc/tests/test_translate.c")
        .arg("csrc/sprout_preload.c");
    run(test_cmd, "translate unit tests");
    println!(
        "cargo:rustc-env=SPROUT_TRANSLATE_TEST_BIN={}",
        test_bin.display()
    );

    // --- 2. the shipping artifact: libsprout-core.so (glibc hosts only) ---
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let host_is_bionic = target_os == "android";
    let forced = env::var("SPROUT_FORCE_PRELOAD_BUILD").is_ok();

    if host_is_bionic && !forced {
        println!(
            "cargo:warning=skipping libsprout-core.so — this is EXPECTED and \ 
             harmless: the interposer is glibc-linked (loaded by the guest's \ 
             ld.so), and building it on bionic would produce an unusable ABI. \ 
             You do not need to do anything: './install.sh' downloads the \ 
             prebuilt glibc+musl DSOs from the latest GitHub release and \ 
             verifies their hashes. (To build from this tree instead, compile \ 
             crates/sprout-preload/csrc/sprout_preload.c with a glibc gcc \ 
             inside a glibc guest, or set SPROUT_FORCE_PRELOAD_BUILD=1 with \ 
             a glibc cross-gcc.)"
        );
        println!("cargo:rustc-env=SPROUT_PRELOAD_SO=");
        return;
    }

    let so_path = out_dir.join("libsprout-core.so");
    let mut so_cmd = cc_tool.to_command();
    compiler_flags(&mut so_cmd);
    so_cmd
        .arg("-fPIC")
        .arg("-shared")
        .arg("-DSPROUT_INTERPOSE")
        .arg("-o")
        .arg(&so_path)
        .arg("csrc/sprout_preload.c")
        .arg("-ldl");
    run(so_cmd, "libsprout-core.so");

    println!("cargo:rustc-env=SPROUT_PRELOAD_SO={}", so_path.display());
}

fn run(mut cmd: Command, what: &str) {
    let status = cmd
        .status()
        .unwrap_or_else(|e| panic!("failed to spawn compiler for {what}: {e}"));
    if !status.success() {
        panic!("compiler failed building {what}: {status}");
    }
}
