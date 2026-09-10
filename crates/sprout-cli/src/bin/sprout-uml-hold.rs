//! `sprout-uml-hold` — host-side rung-3 ring server.
//!
//! Spawned by `sprout uml up --shm` (never by hand). Inherits the
//! guest-physmem memfd as an open fd (number passed in argv[1]) and
//! serves exec requests over the unix socket at argv[2] (dir/ring.sock).
//!
//! Why a separate process: the `up` CLI exits after boot, but the ring
//! needs (a) the memfd to stay open past CLI exit and (b) a stable
//! listener path for later `sprout uml exec` commands. The holder is
//! that anchor; `sprout uml down` kills it via dir/holder.pid.
//!
//! Ring layout (shared page region in the LAST 2MiB of physmem — the
//! kernel never allocates above mem=, so it is ours by construction):
//!   [0..4096)          header: magic "SPR1", host_seq, guest_seq
//!   [4096..4096+128)  8 slot headers: status,len,seq,pad (16B each)
//!   [4224..]           8 x 60KiB payload areas
//! Guest side: uml/sprout-uml-agent.c ring_loop() via /dev/sprout-shm.
//! Frame bodies are byte-identical to the files/vsock transports:
//! u8 op (0=PING,1=EXEC,2=SHUTDOWN) + payload.

use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::io::{FromRawFd, OwnedFd};
use std::os::unix::net::UnixListener;
use std::sync::atomic::{fence, Ordering};
use std::time::{Duration, Instant};

const RING_BYTES: usize = 2 << 20;
const HDR_PAGE: usize = 4096;
const SLOT_COUNT: usize = 8;
const SLOT_HDR: usize = 16;
const MAX_FRAME: usize = 60 * 1024;
const HDR_MAGIC: u32 = 0x31525053; // "SPR1" LE
const FRAME_FREE: u32 = 0;
const FRAME_BUSY: u32 = 1;
// FRAME_DONE (2) is written by the guest agent, never by the holder.
#[allow(dead_code)]
const FRAME_DONE: u32 = 2;

#[repr(C, align(4096))]
struct RingHeader {
    magic: u32,
    version: u32,
    host_seq: u32,
    guest_seq: u32,
    _pad: [u32; 8],
}

fn main() {
    let code = match run() {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("sprout-uml-hold: {e:#}");
            1
        }
    };
    std::process::exit(code);
}

struct Ring {
    base: *mut u8,
    hdr: *mut RingHeader,
}

// # Safety: single-threaded holder; guest accesses go through the same
// shared mapping with release/acquire ordering. Raw pointers never
// escape this struct.
impl Ring {
    fn slot_hdr(&self, i: usize) -> *mut u32 {
        unsafe { self.base.add(HDR_PAGE + i * SLOT_HDR) as *mut u32 }
    }
    fn slot_len(&self, i: usize) -> *mut u32 {
        unsafe { self.slot_hdr(i).add(1) }
    }
    fn slot_seq(&self, i: usize) -> *mut u32 {
        unsafe { self.slot_hdr(i).add(2) }
    }
    fn slot_pay(&self, i: usize) -> *mut u8 {
        unsafe {
            self.base
                .add(HDR_PAGE + SLOT_COUNT * SLOT_HDR + i * MAX_FRAME)
        }
    }
    fn wait_free(&self, deadline: Instant) -> anyhow::Result<usize> {
        loop {
            for i in 0..SLOT_COUNT {
                if unsafe { std::ptr::read_volatile(self.slot_hdr(i)) } == FRAME_FREE {
                    return Ok(i);
                }
            }
            if Instant::now() > deadline {
                anyhow::bail!("no free ring slot");
            }
            std::thread::sleep(Duration::from_micros(200));
        }
    }
}

fn run() -> anyhow::Result<()> {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() != 3 {
        anyhow::bail!("usage: sprout-uml-hold <physmem_fd> <ring.sock> (internal)");
    }
    let fdnum: i32 = argv[1].parse()?;
    let sock_path = std::path::PathBuf::from(&argv[2]);

    // SAFETY: fd handed over by the parent (sprout uml up) pre-exec.
    let physfd = unsafe { OwnedFd::from_raw_fd(fdnum) };

    let mem_bytes = {
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(physfd.as_raw_fd(), &mut st) } < 0 {
            anyhow::bail!("fstat physmem fd: {}", std::io::Error::last_os_error());
        }
        st.st_size as u64
    };
    if (mem_bytes as usize) < RING_BYTES {
        anyhow::bail!("memfd too small for ring ({mem_bytes} < {RING_BYTES})");
    }
    let ring_off = mem_bytes as usize - RING_BYTES;

    // MAP_SHARED the ring tail of the same memfd the guest maps: writes
    // here are visible to /dev/sprout-shm readers inside the guest.
    let map = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            RING_BYTES,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            physfd.as_raw_fd(),
            ring_off as libc::off_t,
        )
    };
    if map == libc::MAP_FAILED {
        anyhow::bail!("mmap ring: {}", std::io::Error::last_os_error());
    }
    let ring = Ring {
        base: map as *mut u8,
        hdr: map as *mut RingHeader,
    };
    // Zero the whole ring ONCE (memfd starts zeroed anyway, but a reused
    // region could be stale after a crash-reboot cycle), then publish the
    // header. The agent spin-waits for the magic, so this is the
    // handoff: after magic appears the agent never rewrites the header.
    unsafe {
        std::ptr::write_bytes(ring.base, 0, RING_BYTES);
        fence(Ordering::Release);
        std::ptr::write_volatile(
            ring.hdr,
            RingHeader {
                magic: HDR_MAGIC,
                version: 1,
                host_seq: 0,
                guest_seq: 0,
                _pad: [0; 8],
            },
        );
    }

    // Serve socket. Remove stale socket from a crashed previous holder.
    let _ = std::fs::remove_file(&sock_path);
    let listener = UnixListener::bind(&sock_path)?;

    for conn in listener.incoming() {
        let mut conn = match conn {
            Ok(c) => c,
            Err(_) => continue,
        };
        // one request per connection (same shape as the files transport:
        // req file = one body, resp = one stream)
        let mut lenb = [0u8; 4];
        if conn.read_exact(&mut lenb).is_err() {
            continue;
        }
        let len = u32::from_le_bytes(lenb) as usize;
        if len == 0 || len > MAX_FRAME {
            let _ = conn.write_all(&0u32.to_le_bytes());
            continue;
        }
        let mut body = vec![0u8; len];
        if conn.read_exact(&mut body).is_err() {
            continue;
        }
        let t0 = Instant::now();

        // find a free slot, publish BUSY
        let i = ring.wait_free(t0 + Duration::from_secs(5))?;
        let seq = unsafe { std::ptr::read_volatile(&(*ring.hdr).host_seq) }.wrapping_add(1);
        unsafe {
            let pay = ring.slot_pay(i);
            std::ptr::copy_nonoverlapping(body.as_ptr(), pay, len);
            fence(Ordering::Release);
            std::ptr::write_volatile(ring.slot_len(i), len as u32);
            std::ptr::write_volatile(ring.slot_seq(i), seq);
            std::ptr::write_volatile(ring.slot_hdr(i), FRAME_BUSY);
            // doorbell: host_seq publish is the release point
            std::ptr::write_volatile(&mut (*ring.hdr).host_seq, seq);
        }

        // wait for guest DONE
        let mut resp_len = 0usize;
        let mut done = false;
        while Instant::now() < t0 + Duration::from_secs(60) {
            let gseq = unsafe { std::ptr::read_volatile(&(*ring.hdr).guest_seq) };
            if gseq >= seq {
                // confirm slot state too (gseq is authoritative doorbell)
                resp_len =
                    unsafe { std::ptr::read_volatile(ring.slot_len(i)) as usize }.min(MAX_FRAME);
                done = true;
                break;
            }
            std::thread::sleep(Duration::from_micros(200));
        }
        if !done {
            let _ = conn.write_all(&0u32.to_le_bytes());
            // free the slot so a later request can proceed
            unsafe { std::ptr::write_volatile(ring.slot_hdr(i), FRAME_FREE) };
            continue;
        }

        // stream reply
        let rbuf = unsafe { std::slice::from_raw_parts(ring.slot_pay(i), resp_len) };
        let ok = conn.write_all(&(resp_len as u32).to_le_bytes()).is_ok()
            && conn.write_all(rbuf).is_ok();
        let _ = ok;
        // release the slot only AFTER the reply is fully read out
        fence(Ordering::Acquire);
        unsafe { std::ptr::write_volatile(ring.slot_hdr(i), FRAME_FREE) };
    }
    Ok(())
}
