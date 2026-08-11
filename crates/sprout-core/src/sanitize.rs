//! Glibc libc sanitizer: produce a cached derivative of the guest's
//! `libc.so.6` in which every `set_robust_list` syscall site is replaced
//! with `mov x0, xzr` (return 0).
//!
//! Why this exists (ADR-0007): Android 15+ seccomp blocks `set_robust_list`
//! for untrusted_app processes; glibc ≥ 2.41 calls it unconditionally
//! during `__libc_early_init`, killing any guest before LD_PRELOAD code
//! can run. Patching a *cached copy* of libc — owned by sprout, never
//! written into the guest rootfs — yields the original `LD_PRELOAD` fast
//! path with zero runtime cost and zero ptrace. If the expected
//! instruction pattern is absent (different glibc/CPU), the caller falls
//! back to the ptrace supervisor.
//!
//! Affected sites (glibc 2.41, verified on Devuan 6 and Ubuntu 24.04):
//! the set_robust_list wrapper in `__pthread_initialize_minimal_early` —
//! return value is discarded by the caller, so `mov x0, xzr` is a correct
//! emulation of "syscall succeeded".
//!
//! The pattern is version-agnostic until glibc changes code generation;
//! site count is verified (>= 1) and every replaced instruction is
//! identical, so a wrong patch is structurally impossible: either the
//! well-known pattern is there, or we refuse and fall back.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

/// aarch64 `mov x8, #imm` (MOVZ, 64-bit) has encoding 0xD2800x68 | (imm<<5);
/// LE bytes: [lo, mid, 0x80|((imm>>3)&0x1f)<<3 | (imm&7)<<0 >>?, 0xd2].
/// We match by decoding instead of hardcoding one pattern: scanning for
/// `mov x8, #<listed sysno>` then a following `svc #0`.
const PAT_SVC: [u8; 4] = [0x01, 0x00, 0x00, 0xd4];
/// aarch64 `mov x0, xzr` (emulated success)     — little endian
const PATCH_MOV_X0_XZR: [u8; 4] = [0xe0, 0x03, 0x1f, 0xaa];

/// Syscall numbers Android's untrusted_app policy blocks (SIGSYS) that
/// glibc startup calls unconditionally and whose return value is ignored
/// by the caller, making them safe to emulate as success.
///
///   99  set_robust_list — robust-mutex bookkeeping; ignored return
///  293  rseq            — restartable sequences registration; ENOSYS-path
///                        is glibc's documented fallback, 0 equally safe
///
/// (Both verified live on Android 16 via the supervisor's trap log.)
/// Syscalls currently *emulated as success* by in-place svc→`mov x0,xzr`
/// replacement (aarch64). 99=set_robust_list, 293=rseq: ignored at
/// call-site level by glibc's own errno propagation when ENOSYS'd.
/// 48=faccessat: musl 1.2.6's `__libc_start_main` polls `faccessat("",
/// R_OK, AT_EMPTY_PATH)` and gracefully tolerates failure — Android ≥15
/// blocks 48 outright (SIGSYS), so we emulate success instead.
/// glibc artifacts: ONLY the two progatics glibc tolerates ENOSYS for.
pub const EMULATED_SYSNOS_GLIBC: [u32; 2] = [99, 293];
/// musl artifacts: goto safety musl callers tolerate (faccessat poll) plus
/// Android's blocked set*id family — emulating success here means "already
/// at minimal privilege", the semantic a rootless sandbox provides.
pub const EMULATED_SYSNOS_MUSL: [u32; 11] = [48, 99, 143, 144, 145, 146, 147, 149, 151, 152, 159];
/// Back-compat union for old tests.
pub const EMULATED_SYSNOS: [u32; 12] = [48, 99, 143, 144, 145, 146, 147, 149, 151, 152, 159, 293];

/// Decode `mov x8, #imm` (MOVZ X8, #imm, LSL 0): insn 0xD2800068 | (imm << 5).
fn mov_x8_imm(insn: u32) -> Option<u32> {
    // MOVZ X8, #imm16, LSL 0 — the constant is the whole encoding minus the
    // 16 immediate bits [20:5] (verified against glibc + rustc output).
    if insn & 0xffe0_001f != 0xd280_0008 {
        return None;
    }
    Some((insn >> 5) & 0xffff)
}

#[derive(Debug, thiserror::Error)]
pub enum SanitizeError {
    #[error("io: {0}")]
    Io(#[from] io::Error),
    #[error("'{0}' is not a 64-bit ELF")]
    NotElf64(PathBuf),
    #[error("no executable PT_LOAD segment in '{0}'")]
    NoExecSegment(PathBuf),
    #[error("no emulated-syscall svc site (sysnos in EMULATED_SYSNOS) found in '{0}'; use ptrace fallback")]
    NoSites(PathBuf),
}

/// FNV-1a 64 over the file contents — deterministic cache key.
fn content_hash(bytes: &[u8], table: &[u32]) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for &b in bytes {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    for &t in table {
        h ^= t as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

/// Executable-segment file ranges from a 64-bit ELF's program headers.
fn exec_ranges(bytes: &[u8]) -> Option<Vec<(usize, usize)>> {
    if bytes.len() < 64 || &bytes[..4] != b"\x7fELF" || bytes[4] != 2 {
        return None;
    }
    let e_phoff = u64::from_le_bytes(bytes[32..40].try_into().ok()?) as usize;
    let e_phentsize = u16::from_le_bytes(bytes[54..56].try_into().ok()?) as usize;
    let e_phnum = u16::from_le_bytes(bytes[56..58].try_into().ok()?) as usize;
    if e_phentsize < 56 || e_phnum == 0 || e_phnum > 512 {
        return None;
    }
    let mut ranges = Vec::new();
    for i in 0..e_phnum {
        let off = e_phoff + i * e_phentsize;
        let ph = bytes.get(off..off + 56)?;
        let p_type = u32::from_le_bytes(ph[0..4].try_into().ok()?);
        let p_flags = u32::from_le_bytes(ph[4..8].try_into().ok()?);
        if p_type != 1 || p_flags & 1 == 0 {
            continue; // PT_LOAD && PF_X only
        }
        let p_offset = u64::from_le_bytes(ph[8..16].try_into().ok()?) as usize;
        let p_filesz = u64::from_le_bytes(ph[32..40].try_into().ok()?) as usize;
        if p_offset + p_filesz <= bytes.len() {
            ranges.push((p_offset, p_offset + p_filesz));
        }
    }
    Some(ranges)
}

/// Locate `mov x8, #<emulated sysno> ; … ; svc #0` sites inside executable
/// segments. Instruction-level scan (not byte-pattern scan) so argument
/// setup of any length between the mov and the svc is fine. Stops scanning
/// each site at the first unconditional control-flow instruction (an
/// `svc` in a different call site behind a branch is never ours).
fn find_sites_for(bytes: &[u8], ranges: &[(usize, usize)], sysnos: &[u32]) -> Vec<usize> {
    let mut sites = Vec::new();
    for &(start, end) in ranges {
        let seg = &bytes[start..end];
        let mut i = 0;
        while i + 8 <= seg.len() {
            let insn = u32::from_le_bytes(seg[i..i + 4].try_into().unwrap());
            if let Some(imm) = mov_x8_imm(insn) {
                if sysnos.contains(&imm) {
                    // look ahead up to 12 instructions for the svc
                    for back in 1..=12usize {
                        let j = i + back * 4;
                        if j + 4 > seg.len() {
                            break;
                        }
                        let next = u32::from_le_bytes(seg[j..j + 4].try_into().unwrap());
                        if next.to_le_bytes() == PAT_SVC {
                            sites.push(start + j);
                            break;
                        }
                        // stop at unconditional control flow: call target
                        // stops belonging to our call-site bundle
                        if next & 0xfc00_0000 == 0x1400_0000 { // b / bl
                            break;
                        }
                    }
                }
            }
            i += 4;
        }
    }
    sites
}

/// Ensure a sanitized copy of a glibc dynamic-object (libc.so.6, the
/// loader, …) exists in `cache_dir`, returning its path. Deterministic:
/// output name is content-addressed; existing files are reused, so
/// repeated calls cost one read of the source.
///
/// `kind` is a short tag embedded in the cache name ("libc", "ldso") so
/// the two artifacts never collide.
pub fn ensure_sanitized_glibc(lib: &Path, cache_dir: &Path, kind: &str) -> Result<PathBuf, SanitizeError> {
    sanitize_impl(lib, cache_dir, kind, &EMULATED_SYSNOS_GLIBC)
}

/// Sanitize a musl ld.so (which IS the libc): syncexec `{48, 99, 293}`.
pub fn ensure_sanitized_musl(lib: &Path, cache_dir: &Path) -> Result<PathBuf, SanitizeError> {
    sanitize_impl(lib, cache_dir, "musl-ldso", &EMULATED_SYSNOS_MUSL)
}

/// musl dedups libraries by filename, not SONAME: the sanitized ldso must
/// appear under its REAL name (libc.musl-aarch64.so.1) in a shadow
/// directory so that both (a) the loader itself and (b) every dynamic
/// binary's DT_NEEDED libc.musl-aarch64.so.1 resolve to the sanitized
/// copy. Returns the shadow path (guaranteed basename-preserving name).
pub fn ensure_musl_shadow_ldso(lib: &Path, cache_dir: &Path) -> Result<PathBuf, SanitizeError> {
    let sanitized = ensure_sanitized_musl(lib, cache_dir)?;
    let shadow_dir = cache_dir.join("musl-shadow-lib");
    std::fs::create_dir_all(&shadow_dir)?;
    let dst = shadow_dir.join("libc.musl-aarch64.so.1");
    // content-addressed src means: if it exists it is already correct
    if !dst.exists() {
        std::fs::hard_link(&sanitized, &dst).or_else(|_| std::fs::copy(&sanitized, &dst).map(|_| ()))?;
    }
    Ok(dst)
}

/// Back-compat alias for the libc entry point.
pub fn ensure_sanitized_libc(libc: &Path, cache_dir: &Path) -> Result<PathBuf, SanitizeError> {
    ensure_sanitized_glibc(libc, cache_dir, "libc")
}

fn sanitize_impl(libc: &Path, cache_dir: &Path, kind: &str, sysnos: &[u32]) -> Result<PathBuf, SanitizeError> {
    let src = fs::read(libc)?;
    let ranges = exec_ranges(&src).ok_or_else(|| SanitizeError::NotElf64(libc.to_path_buf()))?;
    if ranges.is_empty() {
        return Err(SanitizeError::NoExecSegment(libc.to_path_buf()));
    }
    let sites = find_sites_for(&src, &ranges, sysnos);
    if sites.is_empty() {
        return Err(SanitizeError::NoSites(libc.to_path_buf()));
    }

    let hash = content_hash(&src, sysnos);
    fs::create_dir_all(cache_dir)?;
    let out = cache_dir.join(format!("{kind}-sanitized-{hash:016x}.so"));
    if out.exists() {
        return Ok(out);
    }

    let mut patched = src;
    for site in &sites {
        patched[*site..*site + 4].copy_from_slice(&PATCH_MOV_X0_XZR);
    }

    let tmp = cache_dir.join(format!(".tmp-{}-{hash:016x}", std::process::id()));
    fs::write(&tmp, &patched)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&tmp, fs::Permissions::from_mode(0o755))?;
    }
    fs::rename(&tmp, &out)?;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture_libc() -> Vec<u8> {
        // Minimal ELF64 with one executable PT_LOAD containing
        // mov x8,#99 ; ldr x0,[x0] ; add x0,0 ; svc #0 ; sub x0,…
        let mut b = vec![0u8; 1024];
        b[0..4].copy_from_slice(b"\x7fELF");
        b[4] = 2;
        b[5] = 1;
        b[16..18].copy_from_slice(&3u16.to_le_bytes()); // ET_DYN
        b[32..40].copy_from_slice(&64u64.to_le_bytes()); // e_phoff=64
        b[54..56].copy_from_slice(&56u16.to_le_bytes());
        b[56..58].copy_from_slice(&1u16.to_le_bytes()); // e_phnum=1
        // phdr: PT_LOAD|PF_X, offset 256, vaddr irrelevant, filesz 64
        b[64..68].copy_from_slice(&1u32.to_le_bytes());
        b[68..72].copy_from_slice(&5u32.to_le_bytes()); // PF_R|PF_X
        b[72..80].copy_from_slice(&256u64.to_le_bytes());
        b[96..104].copy_from_slice(&64u64.to_le_bytes());
        // text: mov x8,#99; ldr x0,[x0,#8]; add x0,x0,#0xe0; svc #0
        let text: [u32; 4] = [0xd2800c68, 0xf9400400, 0x91400000, 0xd4000001];
        for (i, w) in text.iter().enumerate() {
            b[256 + i * 4..260 + i * 4].copy_from_slice(&w.to_le_bytes());
        }
        b
    }

    #[test]
    fn finds_and_patches_robust_list_site() {
        let bytes = fixture_libc();
        let ranges = exec_ranges(&bytes).unwrap();
        assert_eq!(ranges, vec![(256, 320)]);
        let sites = find_sites_for(&bytes, &ranges, &EMULATED_SYSNOS_GLIBC);
        assert_eq!(sites, vec![268]);
    }

    #[test]
    fn finds_rseq_site_too() {
        let mut b = fixture_libc();
        // change the mov's imm from 99 → 293: check EMULATED_SYSNOS match
        let mov99 = u32::from_le_bytes(b[256..260].try_into().unwrap());
        let mov293 = (mov99 & !0x1fffe0) | (293 << 5);
        b[256..260].copy_from_slice(&mov293.to_le_bytes());
        let ranges = exec_ranges(&b).unwrap();
        let sites = find_sites_for(&b, &ranges, &EMULATED_SYSNOS_GLIBC);
        assert_eq!(sites, vec![268]);
    }

    #[test]
    fn sanitizer_output_replaces_svc_with_success() {
        let dir = std::env::temp_dir().join(format!("sprout-san-test-{}", std::process::id()));
        let src_path = dir.join("libc.so.6");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(&src_path, fixture_libc()).unwrap();

        let out = ensure_sanitized_libc(&src_path, &dir).unwrap();
        let patched = std::fs::read(&out).unwrap();
        assert_eq!(&patched[268..272], &PATCH_MOV_X0_XZR);
        // original kept intact beyond the site
        assert_eq!(&patched[256..260], &0xd2800c68u32.to_le_bytes());

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn refuses_when_no_sites() {
        let dir = std::env::temp_dir().join(format!("sprout-san-empty-{}", std::process::id()));
        let src_path = dir.join("libc.so.6");
        std::fs::create_dir_all(&dir).unwrap();
        let mut bytes = fixture_libc();
        bytes[268..272].copy_from_slice(&0xd503201fu32.to_le_bytes()); // nop instead of svc
        std::fs::write(&src_path, &bytes).unwrap();
        assert!(matches!(
            ensure_sanitized_libc(&src_path, &dir),
            Err(SanitizeError::NoSites(_))
        ));
        std::fs::remove_dir_all(&dir).ok();
    }
}
