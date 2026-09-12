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
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::sync::atomic::{fence, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

#[path = "../journal.rs"]
mod journal;
#[path = "../session_owner.rs"]
mod session_owner;
use journal::Journal;
use session_owner::{acquire_lock, write_state, Shadow};

static ctl_journal: std::sync::OnceLock<Journal> = std::sync::OnceLock::new();

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
    /* ADR-0025 D2: session token lives in the ring header — a pure
     * capability: only processes that can map the memfd (holder, kernel,
     * guest agent) can read it. No file provisioning, no bootstrap window. */
    token: u64,
    /* ADR-0025 D3: holder heartbeat for the guest's dead-man switch. */
    heartbeat: u32,
    _pad: [u32; 5],
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

fn load_or_create_token(dir: &std::path::Path) -> u64 {
    if let Ok(s) = std::fs::read_to_string(dir.join("token")) {
        if let Ok(t) = u64::from_str_radix(s.trim(), 16) {
            if t != 0 {
                return t;
            }
        }
    }
    let mut b = [0u8; 8];
    use std::io::Read;
    let mut f = std::fs::File::open("/dev/urandom").expect("urandom");
    f.read_exact(&mut b).expect("urandom read");
    let t = u64::from_le_bytes(b) | 1; // never 0 (0 = bootstrap fail-open)
    let _ = std::fs::write(dir.join("token"), format!("{t:016x}\n"));
    t
}

/// SO_PEERCRED gate: only same-uid peers may speak to our sockets
/// (ring.sock, shadow.ctl). Termux is single-uid, but a compromised or
/// confused sibling process must not issue bridge ops or mutate binds.
fn peer_uid_ok(conn: &UnixStream) -> bool {
    use std::os::unix::io::AsRawFd;
    let mut cred: libc::ucred = unsafe { std::mem::zeroed() };
    let mut len = std::mem::size_of::<libc::ucred>() as u32;
    let rc = unsafe {
        libc::getsockopt(
            conn.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            &mut cred as *mut _ as *mut libc::c_void,
            &mut len,
        )
    };
    rc == 0 && cred.uid == unsafe { libc::getuid() }
}

fn run() -> anyhow::Result<()> {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() < 3 || argv.len() > 4 {
        anyhow::bail!("usage: sprout-uml-hold <physmem_fd> <ring.sock> [wake_wfd] (internal)");
    }
    let fdnum: i32 = argv[1].parse()?;
    let sock_path = std::path::PathBuf::from(&argv[2]);
    let uml_dir = sock_path
        .parent()
        .map(|p| p.to_path_buf())
        .unwrap_or_else(|| PathBuf::from("."));
    // Optional ring-doorbell write end (host pipe -> guest wake IRQ).
    let wake_wfd: i32 = argv.get(3).and_then(|s| s.parse().ok()).unwrap_or(-1);

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
    /* ADR-0025 D2: the holder is the single writer of the session token.
     * Load the durable one (or mint + persist) BEFORE the agent can boot:
     * the guest reads it from the ring header, never from a file. */
    let token = load_or_create_token(&uml_dir);
    unsafe {
        std::ptr::write_bytes(ring.base, 0, RING_BYTES);
        fence(Ordering::Release);
        std::ptr::write_volatile(
            ring.hdr,
            RingHeader {
                magic: HDR_MAGIC,
                version: 2,
                host_seq: 0,
                guest_seq: 0,
                token,
                heartbeat: 0,
                _pad: [0; 5],
            },
        );
    }
    /* ADR-0025 D3: dead-man heartbeat the guest agent watches. */
    {
        let hdr_usize = ring.hdr as usize;
        thread::spawn(move || loop {
            unsafe {
                let h = hdr_usize as *mut RingHeader;
                let cur = std::ptr::read_volatile(&(*h).heartbeat);
                std::ptr::write_volatile(&mut (*h).heartbeat, cur.wrapping_add(1));
            }
            thread::sleep(Duration::from_millis(250));
        });
    }

    // Serve socket. Remove stale socket from a crashed previous holder.
    let _ = std::fs::remove_file(&sock_path);
    let listener = UnixListener::bind(&sock_path)?;

    // --- Session owner (ADR-0024 §5) ---
    let _lock = acquire_lock(&uml_dir)?;
    let shadow = Arc::new(Mutex::new(Shadow::create_file(
        &uml_dir,
        session_owner::DEFAULT_CAP,
    )?));
    let _ = ctl_journal.set(Journal::open(&uml_dir));
    /* Seed the shadow table from durable journal mount rows: binds are
     * persistent state, re-applied to the guest on every `up`, so the L0
     * table must survive holder restarts too. Row shape:
     * args = [shadow_src, dst, fstype, flags, hostsrc]. */
    if let Some(j) = ctl_journal.get() {
        if let Ok(rows) = j.pending() {
            let mut sh = shadow.lock().unwrap();
            let mut seeded = 0usize;
            for r in &rows {
                if r.op == "mount" && r.args.len() >= 2 {
                    if sh.add_bind(&r.args[1], &r.args[0]).is_ok() {
                        seeded += 1;
                    }
                }
            }
            if seeded > 0 {
                sh.commit();
                eprintln!("sprout-uml-hold: seeded {seeded} shadow binds from journal");
            }
        }
    }
    write_state(&uml_dir, shadow.lock().unwrap().as_raw_fd())?;

    let hb_shadow = Arc::clone(&shadow);
    let hb_stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let hb_stop2 = hb_stop.clone();
    let hb = thread::spawn(move || loop {
        if hb_stop2.load(Ordering::Relaxed) {
            break;
        }
        hb_shadow.lock().unwrap().pulse();
        thread::sleep(Duration::from_millis(250));
    });

    let ctl_path = uml_dir.join("shadow.ctl");
    let _ = std::fs::remove_file(&ctl_path);
    let ctl_listener = UnixListener::bind(&ctl_path)?;
    let ctl_shadow = Arc::clone(&shadow);
    thread::spawn(move || {
        for conn in ctl_listener.incoming() {
            match conn {
                Ok(mut c) => {
                    if !peer_uid_ok(&c) {
                        eprintln!("sprout-uml-hold: shadow.ctl rejected foreign-uid peer");
                        continue;
                    }
                    let _ = ctl_handle(&mut c, &ctl_shadow);
                }
                Err(_) => continue,
            }
        }
    });

    for conn in listener.incoming() {
        let mut conn = match conn {
            Ok(c) => c,
            Err(_) => continue,
        };
        if !peer_uid_ok(&conn) {
            eprintln!("sprout-uml-hold: ring.sock rejected foreign-uid peer");
            continue;
        }
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
            // wake the guest agent out of poll() — one byte is the bell
            if wake_wfd >= 0 {
                let b = [1u8; 1];
                let _ = libc::write(wake_wfd, b.as_ptr() as *const libc::c_void, 1);
            }
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
    // hb_stop/drop happens when run() returns (down path)
    hb_stop.store(true, Ordering::Relaxed);
    let _ = hb.join();
    Ok(())
}

fn ctl_handle(s: &mut UnixStream, shadow: &Arc<Mutex<Shadow>>) -> std::io::Result<()> {
    use std::io::BufRead;
    let s2 = s.try_clone()?;
    let mut s2 = s2;
    let mut r = std::io::BufReader::new(s);
    let mut buf = String::new();
    while r.read_line(&mut buf)? > 0 {
        let line = buf.trim().to_string();
        buf.clear();
        let mut parts = line.split_whitespace();
        let op = parts.next().unwrap_or("");
        match op {
            "bind" => {
                let (dst, src) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
                {
                    let mut sh = shadow.lock().unwrap();
                    match sh.add_bind(dst, src) {
                        Ok(i) => {
                            sh.commit();
                            writeln!(s2, "ok {i}")?;
                        }
                        Err(e) => writeln!(s2, "err {e}")?,
                    }
                }
                /* Journal the bind as DURABLE state — replayed on every
                 * `sprout uml up` (the guest mount table is per-boot), and
                 * consumed only by a matching unbind. Logged AFTER the shadow
                 * commit so a crash between them replays a noop anyway.
                 * JOURNAL args shape (replay + holder seeding understand):
                 *   arg[0]=shadow src (guest path for the L0 table; "none"
                 *          if empty — replay passes "none" as the decorative
                 *          hostfs mount source)
                 *   arg[1]=dst       (guest mount point)
                 *   arg[2]=fstype
                 *   arg[3]=flags     (int64)
                 *   arg[4]=data      (hostfs path: RELATIVE to the share dir)
                 */
                let hostsrc_abs: String = {
                    let hs = parts.next().unwrap_or("");
                    hs.into() // relative — replay maps
                };
                if let Some(j) = ctl_journal.get() {
                    if hostsrc_abs.is_empty() {
                        /* pure L0 shadow bind — no guest-side mount wanted
                         * (a row with empty data would mount the hostfs
                         * ROOT at dst). */
                    } else {
                        /* re-bind = replace: drop any prior row for this dst */
                        let _ = j.drop_mount_by_dst(dst);
                        let _ = j.append(&journal::Row {
                            intent: true,
                            op: "mount".into(),
                            args: vec![
                                if src.is_empty() {
                                    "none".into()
                                } else {
                                    src.into()
                                },
                                dst.into(),
                                "hostfs".into(),
                                "0".into(),
                                hostsrc_abs,
                            ],
                        });
                    }
                }
            }
            "unbind" => {
                let dst = parts.next().unwrap_or("");
                let mut sh = shadow.lock().unwrap();
                let idx = sh
                    .list_binds()
                    .iter()
                    .position(|(d, _, _)| d == dst)
                    .map(|i| i as u32);
                if let Some(i) = idx {
                    sh.mark_state(i, session_owner::S_REMOVED)?;
                    sh.commit();
                    writeln!(s2, "ok {i}")?;
                } else {
                    writeln!(s2, "err not found")?;
                }
                if let Some(j) = ctl_journal.get() {
                    /* consume the durable mount row, then journal the
                     * one-shot umount intent (applied no-op on next boot's
                     * fresh guest, or live if the guest is running). */
                    let _ = j.drop_mount_by_dst(dst);
                    let _ = j.append(&journal::Row {
                        intent: true,
                        op: "umount".into(),
                        args: vec![dst.into()],
                    });
                }
            }
            "dump" => {
                let sh = shadow.lock().unwrap();
                for (dst, src, state) in sh.list_binds() {
                    writeln!(s2, "{dst} <- {src} state={state}")?;
                }
            }
            "quiesce" => {
                let mut sh = shadow.lock().unwrap();
                for i in 0..sh.count() {
                    let _ = sh.mark_state(i, session_owner::S_REMOVED);
                }
                sh.commit();
                writeln!(s2, "ok")?;
            }
            "ping" => writeln!(s2, "ok")?,
            _ => writeln!(s2, "err unknown op")?,
        }
    }
    Ok(())
}
