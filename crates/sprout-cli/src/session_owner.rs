//! Session owner + shadow table writer (ADR-0024 L0), pure Rust.
//!
//! ROLE. One owner process per uml instance (the ring holder). It owns the
//! shadow memfd: the ONLY writer. Every fast-lane child maps it read-only
//! (SPROUT_SHADOW_FD). Readers fail open on ANY inconsistency, so the
//! write-side discipline here is: seqlock (odd = mutating), heartbeat every
//! 250 ms, whole-file bounded size, never free-format writes.
//!
//! FORMAT. Byte-mirrors crates/sprout-preload/csrc/sprout_shadow.h. The C
//! reader and this writer are pinned together by the cross-format test in
//! sprout-cli's unit tests (sp_shadow_test lookup mode).

use std::fs::{File, OpenOptions};
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::Path;

pub const MAGIC: u64 = 0x5350_5254_5348_4457; // "SPRSHDW"
pub const MAP_SIZE: usize = 1 << 16; // reader maps exactly 64 KiB
pub const HDR_SIZE: usize = 56; // measured C sizeof(struct sp_shadow_hdr)
pub const ENTRY_SIZE: usize = 28; // measured C sizeof(struct sp_shadow_entry)
pub const DEFAULT_CAP: u32 = 256;
pub const T_BIND: u8 = 1;
pub const S_VALID: u8 = 1;
pub const S_REMOVED: u8 = 4;

const HDR_MAGIC: usize = 0;
const HDR_GEN: usize = 8;
const HDR_HEARTBEAT: usize = 16;
const HDR_COUNT: usize = 28;
const HDR_CAP: usize = 32;
const HDR_STRTAB_OFF: usize = 40;
const HDR_STRTAB_LEN: usize = 48;

fn mono_ns() -> u64 {
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

pub struct Shadow {
    fd: OwnedFd,
    map: *mut u8,
    strtab_len: usize,
}

// The mmap'd memfd is unaliased except through this struct; the holder
// mediates all access through a Mutex. Heartbeat + ctl threads share it.
unsafe impl Send for Shadow {}

impl Shadow {
    pub fn create(cap: u32) -> io::Result<Self> {
        if (HDR_SIZE + cap as usize * ENTRY_SIZE) >= MAP_SIZE / 2 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "cap too large"));
        }
        let fd = unsafe {
            libc::syscall(libc::SYS_memfd_create, b"sprout-shadow\0".as_ptr(), 0u32)
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let fd = unsafe { OwnedFd::from_raw_fd(fd as RawFd) };
        Self::from_fd(fd, cap)
    }

    /// File-backed variant. ADR-0024 amendment: usable only when the
    /// memfd-via-pidfd mint fails (SELinux, container namespaces). Any
    /// process on the same SELinux context can open the same file and
    /// mmap its magic bytes — simplest possible owner file observable.
    pub fn create_file(dir: &Path, cap: u32) -> io::Result<Self> {
        let p = dir.join("shadow.bin");
        let f = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(true)
            .open(&p)?;
        f.set_len(MAP_SIZE as u64)?;
        Self::from_fd(f.into(), cap)
    }

    fn from_fd(fd: OwnedFd, cap: u32) -> io::Result<Self> {
        if (HDR_SIZE + cap as usize * ENTRY_SIZE) >= MAP_SIZE / 2 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "cap too large"));
        }
        if unsafe { libc::ftruncate(fd.as_raw_fd(), MAP_SIZE as i64) } != 0 {
            return Err(io::Error::last_os_error());
        }
        let map = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                MAP_SIZE,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd.as_raw_fd(),
                0,
            )
        };
        if map == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let map = map as *mut u8;
        let sh = Shadow { fd, map, strtab_len: 0 };
        unsafe {
            sh.w64(HDR_MAGIC, MAGIC);
            sh.w64(HDR_GEN, 1); // odd: not yet published
            sh.w64(HDR_HEARTBEAT, mono_ns());
            sh.w32(HDR_COUNT, 0);
            sh.w64(HDR_CAP, cap as u64);
            sh.w64(HDR_STRTAB_OFF, (HDR_SIZE + cap as usize * ENTRY_SIZE) as u64);
            sh.w64(HDR_STRTAB_LEN, 0);
        }
        Ok(sh)
    }

    /// # Safety: internal — writes u64 LE at `off` with volatile semantics
    /// (~atomic_release for our x86_64/aarch64 purposes via the fence in
    /// commit/pulse).
    unsafe fn w64(&self, off: usize, v: u64) {
        std::ptr::write_volatile(self.map.add(off) as *mut u64, v);
    }
    unsafe fn w32(&self, off: usize, v: u32) {
        std::ptr::write_volatile(self.map.add(off) as *mut u32, v);
    }
    unsafe fn w8(&self, off: usize, v: u8) {
        std::ptr::write_volatile(self.map.add(off), v);
    }
    unsafe fn r64(&self, off: usize) -> u64 {
        std::ptr::read_volatile(self.map.add(off) as *const u64)
    }
    unsafe fn r32(&self, off: usize) -> u32 {
        std::ptr::read_volatile(self.map.add(off) as *const u32)
    }

    fn strtab_base(&self) -> usize {
        unsafe { self.r64(HDR_STRTAB_OFF) as usize }
    }

    pub fn count(&self) -> u32 {
        unsafe { self.r32(HDR_COUNT) }
    }

    /// Add a BIND entry (guest `dst` mounted from `src`). Returns index.
    /// Bytes are copied in while gen is ODD (created state); caller must
    /// `commit()` to publish.
    pub fn add_bind(&mut self, dst: &str, src: &str) -> io::Result<u32> {
        if !dst.starts_with('/') || !src.starts_with('/') {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "paths must be absolute"));
        }
        let count = self.count();
        let cap = unsafe { self.r64(HDR_CAP) } as u32;
        if count >= cap {
            return Err(io::Error::new(io::ErrorKind::OutOfMemory, "shadow full"));
        }
        let base = self.strtab_base();
        let need = dst.len() + 1 + src.len() + 1;
        if base + self.strtab_len + need > MAP_SIZE {
            return Err(io::Error::new(io::ErrorKind::OutOfMemory, "strtab full"));
        }
        let dst_off = self.strtab_len;
        let src_off = dst_off + dst.len() + 1;
        unsafe {
            // entry
            let e = HDR_SIZE + count as usize * ENTRY_SIZE;
            self.w8(e, T_BIND);
            self.w8(e + 1, S_VALID);
            (self.map.add(e + 2) as *mut u16).write_volatile(0);
            self.w32(e + 4, src_off as u32);
            self.w32(e + 8, src.len() as u32);
            self.w32(e + 12, dst_off as u32);
            self.w32(e + 16, dst.len() as u32);
            self.w32(e + 20, 0);
            // strtab
            std::ptr::copy_nonoverlapping(dst.as_ptr(), self.map.add(base + dst_off), dst.len());
            self.w8(base + dst_off + dst.len(), 0);
            std::ptr::copy_nonoverlapping(src.as_ptr(), self.map.add(base + src_off), src.len());
            self.w8(base + src_off + src.len(), 0);
            self.w64(HDR_STRTAB_LEN, (self.strtab_len + need) as u64);
            self.w32(HDR_COUNT, count + 1);
        }
        self.strtab_len += need;
        Ok(count)
    }

    /// Mark entry state (S_REMOVED to tombstone). Caller commits.
    pub fn mark_state(&mut self, idx: u32, state: u8) -> io::Result<()> {
        if idx >= self.count() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "bad idx"));
        }
        unsafe { self.w8(HDR_SIZE + idx as usize * ENTRY_SIZE + 1, state) };
        Ok(())
    }

    /// Publish: advance gen to a new EVEN value + stamp heartbeat.
    /// Readers snap gen; if they see odd, they retry/fail open.
    pub fn commit(&mut self) {
        unsafe {
            let g = self.r64(HDR_GEN);
            self.w64(HDR_HEARTBEAT, mono_ns());
            std::sync::atomic::fence(std::sync::atomic::Ordering::Release);
            self.w64(HDR_GEN, (g & !1) + 2);
        }
    }

    /// Cheap liveness bump (no gen change).
    pub fn pulse(&self) {
        unsafe {
            self.w64(HDR_HEARTBEAT, mono_ns());
            std::sync::atomic::fence(std::sync::atomic::Ordering::Release);
        }
    }

    pub fn as_raw_fd(&self) -> RawFd {
        self.fd.as_raw_fd()
    }

    /// Snapshot all BIND entries: (dst, src, state). Read-only scan;
    /// caller should hold any needed synchronization.
    pub fn list_binds(&self) -> Vec<(String, String, u8)> {
        let count = self.count();
        let base = self.strtab_base();
        let mut out = Vec::new();
        for i in 0..count as usize {
            let e = HDR_SIZE + i * ENTRY_SIZE;
            unsafe {
                if std::ptr::read_volatile(self.map.add(e)) != T_BIND {
                    continue;
                }
                let state = std::ptr::read_volatile(self.map.add(e + 1));
                let src_off = self.r32(e + 4) as usize;
                let src_len = self.r32(e + 8) as usize;
                let dst_off = self.r32(e + 12) as usize;
                let dst_len = self.r32(e + 16) as usize;
                if base + dst_off + dst_len > MAP_SIZE || base + src_off + src_len > MAP_SIZE {
                    continue;
                }
                let dst = std::slice::from_raw_parts(self.map.add(base + dst_off), dst_len);
                let src = std::slice::from_raw_parts(self.map.add(base + src_off), src_len);
                out.push((
                    String::from_utf8_lossy(dst).into_owned(),
                    String::from_utf8_lossy(src).into_owned(),
                    state,
                ));
            }
        }
        out
    }
}

impl Drop for Shadow {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.map as *mut libc::c_void, MAP_SIZE) };
    }
}

/* ------------------------------------------------------------------ */
/* Session lock: flock-based, kernel releases on process death — so   */
/* owner-death steal is just "next starter wins the flock".           */
/* ------------------------------------------------------------------ */

pub fn acquire_lock(dir: &Path) -> io::Result<File> {
    std::fs::create_dir_all(dir)?;
    let f = OpenOptions::new()
        .create(true)
        .write(true)
        .open(dir.join("session.lock"))?;
    let rc = unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
    if rc != 0 {
        return Err(io::Error::new(
            io::ErrorKind::WouldBlock,
            "session owner lock held by live process",
        ));
    }
    Ok(f)
}

/// Write the discovery file readers use: pid + fd num. Readers open
/// /proc/<pid>/fd/<fd> to mint their own handle to the same memfd.
pub fn write_state(dir: &Path, fd: RawFd) -> io::Result<()> {
    std::fs::write(
        dir.join("shadow.env"),
        format!("pid={}\nfd={}\n", std::process::id(), fd),
    )
}

/// Fast-lane discovery: given the uml dir, return a fresh fd (open,
/// CLOEXEC **unset** so it survives exec into the guest) to the owner's
/// shadow memfd. None if owner is absent/dead.
pub fn connect_shadow(dir: &Path) -> Option<RawFd> {
    let txt = std::fs::read_to_string(dir.join("shadow.env")).ok()?;
    let mut pid = 0i32;
    let mut fd = -1i32;
    for line in txt.lines() {
        if let Some(v) = line.strip_prefix("pid=") {
            pid = v.trim().parse().ok()?;
        } else if let Some(v) = line.strip_prefix("fd=") {
            fd = v.trim().parse().ok()?;
        }
    }
    if pid <= 0 || fd < 0 {
        return None;
    }
    // owner alive? flock probe would do too; kill(0) is enough.
    if unsafe { libc::kill(pid, 0) } != 0 {
        return None;
    }
    // Android SELinux denies /proc/<pid>/fd/<n> across processes even for the
    // same uid (EACCES). pidfd_open + pidfd_getfd was verified to work on
    // this platform (SYSCALL 434/438 on aarch64).
    const SYS_PIDFD_OPEN: libc::c_long = 434;
    const SYS_PIDFD_GETFD: libc::c_long = 438;
    let pidfd = unsafe { libc::syscall(SYS_PIDFD_OPEN, pid, 0u32) };
    if pidfd < 0 {
        return None;
    }
    /* Try a fresh fd from pidfd; if that fails (SELinux / different
     * namespace), fall back to the shadow.bin file which IS visible across
     * every namespace boundary. */
    let nfd = unsafe { libc::syscall(SYS_PIDFD_GETFD, pidfd, fd, 0u32) } as RawFd;
    unsafe { libc::close(pidfd as RawFd) };
    if nfd >= 0 {
        return Some(nfd);
    }
    // File fallback: mmap readable + shared.
    let f = dir.join("shadow.bin");
    let c = std::ffi::CString::new(f.as_os_str().as_encoded_bytes()).ok()?;
    let nfd = unsafe { libc::open(c.as_ptr(), libc::O_RDONLY) };
    if nfd < 0 {
        None
    } else {
        Some(nfd)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn write_and_self_read_layout() {
        let mut sh = Shadow::create(16).unwrap();
        let i0 = sh.add_bind("/data", "/hostfs/data").unwrap();
        let i1 = sh.add_bind("/x", "/hostfs/x").unwrap();
        assert_eq!((i0, i1), (0, 1));
        sh.commit();
        unsafe {
            assert_eq!(sh.r64(0), MAGIC);
            assert_eq!(sh.r32(28), 2); // count
            assert_eq!(sh.r64(8) % 2, 0); // gen even after commit
            let e = HDR_SIZE;
            assert_eq!(std::ptr::read_volatile(sh.map.add(e)), T_BIND);
            assert_eq!(std::ptr::read_volatile(sh.map.add(e + 1)), S_VALID);
            let base = sh.strtab_base() as usize;
            let dst_off = sh.r32(e + 12) as usize;
            let s = std::slice::from_raw_parts(sh.map.add(base + dst_off), 5);
            assert_eq!(s, b"/data");
        }
        sh.pulse();
    }

    #[test]
    fn rejects_relative_and_overflow() {
        let mut sh = Shadow::create(4).unwrap();
        assert!(sh.add_bind("rel", "/abs").is_err());
        for i in 0..4u32 {
            sh.add_bind(&format!("/d{i}"), &format!("/s{i}")).unwrap();
        }
        assert!(sh.add_bind("/x", "/y").is_err());
    }
}

#[cfg(test)]
mod live_tests {
    use super::*;
    #[test]
    fn live_shadow_discovery() {
        let d = format!("{}/.sprout/uml/main", std::env::var("HOME").unwrap());
        let r = connect_shadow(std::path::Path::new(&d));
        eprintln!("connect_shadow({d}) -> {r:?}");
    }
}
