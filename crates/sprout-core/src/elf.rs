//! Guest ELF classification: dynamic vs static, 32 vs 64 bit, loader path.
//!
//! This decides the interception strategy (ADR-0002): LD_PRELOAD works only
//! for dynamically linked binaries, so anything without PT_INTERP goes to
//! the ptrace fallback.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use thiserror::Error;

const PT_INTERP: u32 = 3;
const PT_LOAD: u32 = 1;
const PT_NOTE: u32 = 4;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GuestClass {
    /// PT_INTERP present; the interpreter path (e.g. "/lib64/ld-linux-aarch64.so.1").
    Dynamic { interp: String },
    /// Dynamic Go binary (libc-linked, but Go's os package issues RAW
    /// syscalls bypassing libc — the LD_PRELOAD interposer never sees its
    /// path arguments). Detected by the PT_NOTE "Go" buildid note. Needs
    /// supervisor translation wrapped in a loader chain launch.
    GoDynamic { interp: String },
    /// No PT_INTERP: static binary or static-PIE (Go, musl-static, etc.).
    Static,
    /// Not an ELF at all (script, text, garbage).
    NotElf,
    /// 32-bit ELF — out of scope for v0.1 (ADR-0005).
    Elf32,
}

#[derive(Debug, Error)]
pub enum ElfError {
    #[error("failed to read '{0}': {1}")]
    Read(String, #[source] std::io::Error),
}

fn read_range(path: &Path, offset: u64, len: usize) -> std::io::Result<Vec<u8>> {
    let mut f = File::open(path)?;
    f.seek(SeekFrom::Start(offset))?;
    let mut buf = vec![0u8; len];
    let mut got = 0;
    while got < len {
        match f.read(&mut buf[got..]) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    buf.truncate(got);
    Ok(buf)
}

/// Classify an ELF file by header + program headers. Reads only the header
/// pages plus the PT_INTERP string — never the whole file.
pub fn classify(path: &Path) -> Result<GuestClass, ElfError> {
    let name = || path.display().to_string();
    let head = read_range(path, 0, 64).map_err(|e| ElfError::Read(name(), e))?;
    if head.len() < 20 || &head[0..4] != b"\x7fELF" {
        return Ok(GuestClass::NotElf);
    }
    match head[4] {
        1 => return Ok(GuestClass::Elf32),
        2 => {}
        _ => return Ok(GuestClass::NotElf),
    }
    if head.len() < 64 {
        return Ok(GuestClass::NotElf);
    }

    let e_phoff = u64::from_le_bytes(head[32..40].try_into().unwrap());
    let e_phentsize = u16::from_le_bytes(head[54..56].try_into().unwrap()) as usize;
    let e_phnum = u16::from_le_bytes(head[56..58].try_into().unwrap()) as usize;
    if e_phentsize < 56 {
        return Ok(GuestClass::NotElf);
    }
    if e_phnum == 0 {
        // Zero program headers: not an executable image we can launch as-is.
        // From sprout's perspective, treat as static (needs ptrace fallback
        // rather than a guest loader invocation).
        return Ok(GuestClass::Static);
    }

    let mut interp: Option<String> = None;
    let mut is_go = false;

    for i in 0..e_phnum.min(4096) {
        let off = e_phoff + (i * e_phentsize) as u64;
        let ph = match read_range(path, off, 56) {
            Ok(b) if b.len() == 56 => b,
            _ => return Ok(GuestClass::NotElf),
        };
        let p_type = u32::from_le_bytes(ph[0..4].try_into().unwrap());
        let p_offset = u64::from_le_bytes(ph[8..16].try_into().unwrap());
        let p_filesz = u64::from_le_bytes(ph[32..40].try_into().unwrap()) as usize;
        if p_type == PT_NOTE && p_filesz > 0 && p_filesz <= 1 << 20 {
            // Scan notes for namesz==4, name=="Go": the Go runtime embeds
            // ".note.go.buildid" in every binary, stripped or not. This
            // marks direct-syscall images (Go bypasses libc wrappers, so
            // LD_PRELOAD alone can never translate for them).
            if let Ok(raw) = read_range(path, p_offset, p_filesz.min(1 << 20)) {
                if let Some(n) = parse_note_names(&raw)
                    .iter()
                    .find(|n| n.starts_with(b"Go\x00\x00"))
                {
                    if n.len() == 4 {
                        is_go = true;
                    }
                }
            }
            continue;
        }
        if p_type != PT_INTERP {
            let _ = PT_LOAD; // segment kinds we don't need yet
            continue;
        }
        if p_filesz == 0 || p_filesz > 4096 {
            return Ok(GuestClass::NotElf);
        }
        let raw = read_range(path, p_offset, p_filesz).map_err(|e| ElfError::Read(name(), e))?;
        let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
        if raw.len() != p_filesz || end == 0 {
            return Ok(GuestClass::NotElf);
        }
        interp = Some(String::from_utf8_lossy(&raw[..end]).into_owned());
    }
    match interp {
        Some(i) if is_go => Ok(GuestClass::GoDynamic { interp: i }),
        Some(i) => Ok(GuestClass::Dynamic { interp: i }),
        None => Ok(GuestClass::Static),
    }
}

/// Extract each note's name field from a PT_NOTE segment (namesz/descsz
/// header, both 4-byte aligned payloads). Best-effort: malformed segments
/// just yield what parsed before the corruption.
fn parse_note_names(seg: &[u8]) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let mut pos = 0usize;
    while pos + 12 <= seg.len() {
        let namesz = u32::from_le_bytes(seg[pos..pos + 4].try_into().unwrap()) as usize;
        let descsz = u32::from_le_bytes(seg[pos + 4..pos + 8].try_into().unwrap()) as usize;
        if namesz == 0 || namesz > 256 || pos + 12 + namesz > seg.len() {
            break;
        }
        let name = seg[pos + 12..pos + 12 + namesz].to_vec();
        out.push(name);
        let name_aligned = (namesz + 3) & !3;
        let desc_aligned = (descsz + 3) & !3;
        let next = pos + 12 + name_aligned + desc_aligned;
        if next <= pos || next > seg.len() {
            break;
        }
        pos = next;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    /// Minimal 64-bit PIE with one PT_INTERP segment.
    fn fixture_dynamic() -> Vec<u8> {
        let mut b = vec![0u8; 512];
        b[0..4].copy_from_slice(b"\x7fELF");
        b[4] = 2; // 64-bit
        b[5] = 1; // LE
        b[16..18].copy_from_slice(&3u16.to_le_bytes()); // ET_DYN
        b[32..40].copy_from_slice(&64u64.to_le_bytes()); // e_phoff
        b[54..56].copy_from_slice(&56u16.to_le_bytes()); // e_phentsize
        b[56..58].copy_from_slice(&1u16.to_le_bytes()); // e_phnum
                                                        // phdr @ 64
        b[64..68].copy_from_slice(&PT_INTERP.to_le_bytes());
        b[72..80].copy_from_slice(&256u64.to_le_bytes()); // p_offset
        let interp = b"/lib64/ld-linux-aarch64.so.1\0";
        b[96..104].copy_from_slice(&(interp.len() as u64).to_le_bytes()); // p_filesz
        b[256..256 + interp.len()].copy_from_slice(interp);
        b
    }

    /// Dynamic ELF + PT_NOTE containing a Go buildid note → GoDynamic.
    /// Note: length must differ from fixture_dynamic()'s (with_temp keys on
    /// byte length; equal lengths race in parallel test runs).
    fn fixture_go_dynamic() -> Vec<u8> {
        let mut b = vec![0u8; 768];
        b[..512].copy_from_slice(&fixture_dynamic());
        b[56..58].copy_from_slice(&2u16.to_le_bytes()); // e_phnum = 2
        // phdr[1] @ 120: PT_NOTE at offset 320
        b[120..124].copy_from_slice(&PT_NOTE.to_le_bytes());
        b[128..136].copy_from_slice(&320u64.to_le_bytes()); // p_offset
        // note: namesz=4 "Go\0\0", descsz=8, type=4
        let mut note = Vec::new();
        note.extend_from_slice(&4u32.to_le_bytes());
        note.extend_from_slice(&8u32.to_le_bytes());
        note.extend_from_slice(&4u32.to_le_bytes());
        note.extend_from_slice(b"Go\x00\x00");
        note.extend_from_slice(&8u64.to_le_bytes());
        b[152..160].copy_from_slice(&(note.len() as u64).to_le_bytes()); // p_filesz
        b[320..320 + note.len()].copy_from_slice(&note);
        b
    }

    #[test]
    fn detects_go_dynamic() {
        with_temp(&fixture_go_dynamic(), |p| match classify(p).unwrap() {
            GuestClass::GoDynamic { .. } => {}
            other => panic!("expected GoDynamic, got {:?}", other),
        });
    }

    #[test]
    fn parses_go_note_name() {
        let mut seg = Vec::new();
        seg.extend_from_slice(&4u32.to_le_bytes());
        seg.extend_from_slice(&8u32.to_le_bytes());
        seg.extend_from_slice(&4u32.to_le_bytes());
        seg.extend_from_slice(b"Go\x00\x00");
        seg.extend_from_slice(&1234u64.to_le_bytes());
        let names = parse_note_names(&seg);
        assert_eq!(names.len(), 1);
        assert_eq!(&names[0], b"Go\x00\x00");
    }

    fn fixture_static() -> Vec<u8> {
        // ET_EXEC with a single PT_LOAD segment and no PT_INTERP.
        let mut b = vec![0u8; 200];
        b[0..4].copy_from_slice(b"\x7fELF");
        b[4] = 2;
        b[5] = 1;
        b[16..18].copy_from_slice(&2u16.to_le_bytes()); // ET_EXEC
        b[32..40].copy_from_slice(&64u64.to_le_bytes()); // e_phoff
        b[54..56].copy_from_slice(&56u16.to_le_bytes()); // e_phentsize
        b[56..58].copy_from_slice(&1u16.to_le_bytes()); // e_phnum = 1
        b[64..68].copy_from_slice(&PT_LOAD.to_le_bytes()); // p_type
        b
    }

    fn with_temp(bytes: &[u8], f: impl Fn(&Path)) {
        let dir =
            std::env::temp_dir().join(format!("sprout-elf-{}-{}", std::process::id(), bytes.len()));
        fs::create_dir_all(&dir).unwrap();
        let p = dir.join("bin");
        fs::write(&p, bytes).unwrap();
        f(&p);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn detects_dynamic_with_interp() {
        with_temp(&fixture_dynamic(), |p| match classify(p).unwrap() {
            GuestClass::Dynamic { interp } => {
                assert_eq!(interp, "/lib64/ld-linux-aarch64.so.1")
            }
            other => panic!("expected Dynamic, got {other:?}"),
        });
    }

    #[test]
    fn detects_static() {
        with_temp(&fixture_static(), |p| {
            assert_eq!(classify(p).unwrap(), GuestClass::Static);
        });
    }

    #[test]
    fn rejects_garbage() {
        with_temp(b"#!/bin/sh\n", |p| {
            assert_eq!(classify(p).unwrap(), GuestClass::NotElf);
        });
    }

    #[test]
    fn classifies_real_binary() {
        // The test binary itself is a dynamically linked ELF on every host
        // we care about (glibc on CI, bionic on Termux).
        let me = std::env::current_exe().unwrap();
        match classify(&me).unwrap() {
            GuestClass::Dynamic { interp } => assert!(interp.starts_with('/')),
            other => panic!("expected Dynamic for test binary, got {other:?}"),
        }
    }
}
