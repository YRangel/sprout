//! `sprout uml` — UML sidecar management (ADR-0023 Phase 1).
//!
//! ADR-0023 plan (`sprout-uml-plan.md`): never merge kernels, partition
//! per-process. This module owns the slow lane's lifecycle ONLY — the fast
//! lane exec path in main.rs is untouched by construction (separate argv[1]
//! arm, no shared state).
//!
//! v1 transport: hostfs-backed AF_UNIX socket (~30µs, no guest net
//! needed). The agent listens on /run/sprout/exec.sock inside the guest;
//! that path is a hostfs mount of the per-guest share dir, so the host
//! dials the same socket path directly. No slirp/TUN/TAP, no TCP, no
//! port-forwards — works rootless on Android. (Ancient plan revisions
//! mentioned slirp port-forward; there is no CONFIG_SLIRP — the UML
//! network driver is UML_NET_VECTOR with userspace transports.)

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

/// Protocol v1 framing (matches sprout-uml-plan.md §3.2, agent side).
/// All integers little-endian.
pub const PROTO_PING: u8 = 0x00;
pub const PROTO_EXEC: u8 = 0x01;
pub const PROTO_SHUTDOWN: u8 = 0x02;
const RESP_STDOUT: u32 = 1;
const RESP_STDERR: u32 = 2;
const RESP_EXIT: u32 = 0;
/// Agent's AF_VSOCK listener port (guest side; host reaches it through
/// the vhost-device-vsock bridge as CONNECT <port> on the control UDS).
pub const VSOCK_PORT: u32 = 2225;

/// Per-guest state dir: ~/.sprout/uml/<id>/
pub fn uml_dir(id: &str) -> PathBuf {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".sprout").join("uml").join(id)
}

/// Default UML binary lookup: $SPROUT_UML_BIN, then PATH, then ./linux.uml.
pub fn find_uml_bin() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("SPROUT_UML_BIN") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    if let Ok(path) = std::env::var("PATH") {
        for d in path.split(':') {
            let p = PathBuf::from(d).join("linux.uml");
            if p.is_file() {
                return Some(p);
            }
        }
    }
    let local = PathBuf::from("./linux.uml");
    if local.is_file() {
        return Some(local);
    }
    None
}

/// Build the headless UML command line. Pure function — unit-testable.
/// No guest networking: the agent socket lives on the hostfs share dir,
/// so host↔guest exec needs no slirp/TAP/TCP at all.
#[allow(clippy::too_many_arguments)]
pub fn build_cmdline(
    uml_bin: &std::path::Path,
    cow: &std::path::Path,
    backing: &std::path::Path,
    share_dir: &std::path::Path,
    mem: &str,
    cpus: u32,
    umid: &str,
    vsock_dev: Option<&std::path::Path>,
    extra: &[String],
) -> (PathBuf, Vec<String>) {
    // COW is opt-in (SPROUT_UML_COW=1): the empty-cow-file trick needs a
    // kernel whose COW driver accepts zeroed files (upstream does; some
    // ports reject them with errno 22). Default = direct backing, which
    // every kernel accepts and matches the harness boot path.
    let cow_enabled = std::env::var("SPROUT_UML_COW")
        .map(|v| v == "1")
        .unwrap_or(false);
    let ubd = if cow_enabled {
        format!("ubd0={},{}", cow.display(), backing.display())
    } else {
        format!("ubd0={}", backing.display())
    };
    let mut args = vec![
        ubd,
        // rw: the guest fstab is unconfigured ("UNCONFIGURED FSTAB"), so
        // systemd-remount-fs has nothing to remount and the kernel default
        // (ro) would stick — every rootfs write (agent install, dpkg, …)
        // would fail with EROFS.
        "root=/dev/ubda".to_string(),
        "rw".to_string(),
        format!("mem={mem}"),
        format!("ncpus={cpus}"),
        // Hostfs exchange dir (NOT root — UBD is root; hostfs is the
        // suitcase, measured ~10x slower for bulk IO). Kernel-side
        // hostfs= takes <host dir>,<flags> and CONFINES all guest
        // hostfs mounts to that host tree; the guest mounts it with
        // `mount -t hostfs none /run/sprout` (init script).
        format!("hostfs={}", share_dir.display()),
        // Console off: console emulation is a trap per character.
        "con=null".to_string(),
        "con0=null,fd:2".to_string(),
        format!("umid={umid}"),
        // No eth0/slirp: the agent socket is on the hostfs share dir,
        // reachable from the host as a plain AF_UNIX path. Guest net
        // stays for docker-inside only (UML_NET_VECTOR, configured
        // inside the guest when needed).
        "sprout_uml=1".to_string(),
    ];
    // Android: exec'ing the stub from a memfd is denied (SELinux/app exec
    // rules), so UML needs stub_exe=<file> pointing at the
    // stub built next to the kernel (arch/um/kernel/skas/stub_exe).
    // SPROUT_UML_STUB overrides; else stub_exe beside the kernel binary.
    if let Some(stub) = find_stub(uml_bin) {
        args.push(format!("stub_exe={}", stub.display()));
    }
    // vsock fast transport: attach the virtio-uml device only when a
    // backend is guaranteed to be listening (cmd_up socket-waits before
    // this). Device id 19 = virtio-uml.0 in the guest.
    if let Some(vm_sock) = vsock_dev {
        args.push(format!("virtio_uml.device={}:19", vm_sock.display()));
    }
    args.extend(extra.iter().cloned());
    (uml_bin.to_path_buf(), args)
}

/// Locate stub_exe: $SPROUT_UML_STUB, then <kernel dir>/stub_exe, then
/// the real build path <kernel dir>/arch/um/kernel/skas/stub_exe.
/// The deep path is the one kbuild actually produces; missing it meant
/// no stub_exe= arg → memfd exec (SELinux-denied on Android) → every
/// guest execve failed (init exec error -12 panic loop).
fn find_stub(uml_bin: &std::path::Path) -> Option<PathBuf> {
    if let Ok(p) = std::env::var("SPROUT_UML_STUB") {
        let pb = PathBuf::from(p);
        if pb.is_file() {
            return Some(pb);
        }
    }
    let parent = uml_bin.parent()?;
    ["stub_exe", "arch/um/kernel/skas/stub_exe"]
        .iter()
        .map(|rel| parent.join(rel))
        .find(|c| c.is_file())
}

/// Send one protocol frame over the hostfs-shared AF_UNIX socket, read
/// response frames until RESP_EXIT. Returns (exit_code, stdout, stderr).
/// `sock_path` is the HOST path of exec.sock inside the per-guest share dir.
pub fn agent_exec(
    sock_path: &str,
    argv: &[String],
    env: &[String],
    cwd: &str,
    stdin_data: &[u8],
    timeout: Duration,
) -> anyhow::Result<(i32, Vec<u8>, Vec<u8>)> {
    let s = UnixStream::connect(sock_path)?;
    s.set_read_timeout(Some(timeout))?;
    s.set_write_timeout(Some(timeout))?;
    let mut s = s;

    let mut req = Vec::new();
    req.push(PROTO_EXEC);
    push_strs(&mut req, argv);
    push_strs(&mut req, env);
    push_str(&mut req, cwd.as_bytes());
    push_u32(&mut req, stdin_data.len() as u32);
    req.extend_from_slice(stdin_data);
    // uid/gid/timeout/flags: 0 = guest agent defaults
    push_u32(&mut req, 0);
    push_u32(&mut req, 0);
    push_u32(&mut req, timeout.as_millis().min(u32::MAX as u128) as u32);
    push_u32(&mut req, 0);
    s.write_all(&req)?;

    let mut out = Vec::new();
    let mut err = Vec::new();
    loop {
        let mut hdr = [0u8; 8];
        s.read_exact(&mut hdr)?;
        let stream = u32::from_le_bytes(hdr[0..4].try_into().unwrap());
        let len = u32::from_le_bytes(hdr[4..8].try_into().unwrap()) as usize;
        if stream == RESP_EXIT {
            let mut code_b = [0u8; 4];
            s.read_exact(&mut code_b)?;
            return Ok((i32::from_le_bytes(code_b), out, err));
        }
        let mut buf = vec![0u8; len];
        s.read_exact(&mut buf)?;
        if stream == RESP_STDOUT {
            out.extend_from_slice(&buf);
        } else if stream == RESP_STDERR {
            err.extend_from_slice(&buf);
        }
    }
}

/// Parse a protocol-v1 response frame stream: u32 type + u32 len frames,
/// type 1=stdout 2=stderr 0=exit(+i32 code). Returns (code, stdout, stderr).
fn parse_frames(mut cur: &[u8]) -> anyhow::Result<(i32, Vec<u8>, Vec<u8>)> {
    let mut out = Vec::new();
    let mut err = Vec::new();
    loop {
        if cur.len() < 8 {
            anyhow::bail!("truncated frame header");
        }
        let stream = u32::from_le_bytes(cur[0..4].try_into().unwrap());
        let len = u32::from_le_bytes(cur[4..8].try_into().unwrap()) as usize;
        cur = &cur[8..];
        if stream == RESP_EXIT {
            if cur.len() < 4 {
                anyhow::bail!("truncated exit code");
            }
            let code = i32::from_le_bytes(cur[0..4].try_into().unwrap());
            return Ok((code, out, err));
        }
        if cur.len() < len {
            anyhow::bail!("truncated frame body");
        }
        let (b, rest) = cur.split_at(len);
        if stream == RESP_STDOUT {
            out.extend_from_slice(b);
        } else if stream == RESP_STDERR {
            err.extend_from_slice(b);
        }
        cur = rest;
    }
}

/// Exec via the file transport (hostfs share): hostfs socket nodes are
/// placeholders on the host, so host->guest exec rides req.<n>/resp.<n>
/// files. Wire format identical to the socket protocol.
/// Ring transport (rung 3): body identical to the files transport, but
/// the round trip goes CLI -> holder (unix socket) -> shared-physmem ring
/// -> agent. The holder does the memfd-slot dance; we just speak
/// length-prefixed frames to it.
pub fn agent_exec_ring(
    ring_sock: &std::path::Path,
    argv: &[String],
    env: &[String],
    cwd: &str,
    stdin_data: &[u8],
    timeout: Duration,
) -> anyhow::Result<(i32, Vec<u8>, Vec<u8>)> {
    use anyhow::Context;
    use std::io::Write;
    use std::os::unix::net::UnixStream;
    let mut conn = UnixStream::connect(ring_sock)
        .with_context(|| format!("ring connect {}", ring_sock.display()))?;
    let mut body = Vec::new();
    body.push(PROTO_EXEC);
    push_strs(&mut body, argv);
    push_strs(&mut body, env);
    push_str(&mut body, cwd.as_bytes());
    push_u32(&mut body, stdin_data.len() as u32);
    body.extend_from_slice(stdin_data);
    push_u32(&mut body, 0);
    push_u32(&mut body, 0);
    push_u32(&mut body, timeout.as_millis().min(u32::MAX as u128) as u32);
    push_u32(&mut body, 0);
    conn.write_all(&(body.len() as u32).to_le_bytes())?;
    conn.write_all(&body)?;
    conn.flush()?;

    let deadline = Instant::now() + timeout + Duration::from_secs(10);
    conn.set_read_timeout(Some(deadline.saturating_duration_since(Instant::now())))?;
    let mut lenb = [0u8; 4];
    conn.read_exact(&mut lenb)
        .context("ring: holder closed before reply len")?;
    let rlen = u32::from_le_bytes(lenb) as usize;
    if rlen > 8 << 20 {
        anyhow::bail!("ring reply too big ({rlen})");
    }
    let mut raw = vec![0u8; rlen];
    conn.read_exact(&mut raw)
        .context("ring: short reply from holder")?;
    parse_frames(&raw)
}

/* ADR-0024 §9: PROTO_MOUNT over the ring (op 0x03). Wire:
 *   [u8 0x03][16B auth hdr: u64 token LE | u32 flags=0 | u32 rsvd=0]
 *   [u8 subop][lp src][lp dst][lp fstype][u64 mflags][lp data]
 * Reply: [u8 status][u32 len][payload]. Status 0 = ok, else errno. */
const PROTO_MOUNT: u8 = 0x03;

fn agent_bridge_op_ring(
    ring_sock: &std::path::Path,
    token: u64,
    subop: u8,
    src: &str,
    dst: &str,
    fstype: &str,
    mflags: u64,
    data: &str,
    timeout: Duration,
) -> anyhow::Result<u8> {
    use anyhow::Context;
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;
    let mut conn = UnixStream::connect(ring_sock)
        .with_context(|| format!("ring connect {}", ring_sock.display()))?;
    let mut body = Vec::with_capacity(64);
    body.push(PROTO_MOUNT);
    body.extend_from_slice(&token.to_le_bytes());
    body.extend_from_slice(&0u32.to_le_bytes()); // flags
    body.extend_from_slice(&0u32.to_le_bytes()); // reserved
    body.push(subop);
    push_str(&mut body, src.as_bytes());
    push_str(&mut body, dst.as_bytes());
    push_str(&mut body, fstype.as_bytes());
    body.extend_from_slice(&mflags.to_le_bytes());
    push_str(&mut body, data.as_bytes());
    conn.write_all(&(body.len() as u32).to_le_bytes())?;
    conn.write_all(&body)?;
    conn.flush()?;
    conn.set_read_timeout(Some(timeout))?;
    /* Wire shape (holder transport): [u32 total_len][agent frame], where the
     * agent frame itself is [u8 status][u32 payload_len][payload]. Read the
     * outer frame first, then parse the agent frame from inside it. */
    let mut lb = [0u8; 4];
    conn.read_exact(&mut lb)
        .context("ring: no bridge-op reply")?;
    let total = u32::from_le_bytes(lb) as usize;
    if total < 5 || total > (1 << 20) {
        anyhow::bail!("ring: malformed bridge-op reply (len {total})");
    }
    let mut frame = vec![0u8; total];
    conn.read_exact(&mut frame)
        .context("ring: truncated bridge-op reply")?;
    let status = frame[0];
    let plen = u32::from_le_bytes([frame[1], frame[2], frame[3], frame[4]]) as usize;
    let _ = plen; // payload already consumed inside `frame`
    Ok(status)
}

/// Replay the intent journal into a freshly booted guest (T9). Best-effort:
/// failures are logged, rows stay pending for the next up. Token: read from
/// dir/token (provisioned into the guest at up by the caller); falls back to
/// exec-free skip when absent.
fn journal_replay(dir: &std::path::Path) {
    use crate::journal::Journal;
    let j = Journal::open(dir);
    let pending = match j.pending() {
        Ok(p) => p,
        Err(e) => {
            eprintln!("sprout: journal read failed: {e}");
            return;
        }
    };
    if pending.is_empty() {
        return;
    }
    let ring = dir.join("ring.sock");
    let token = read_token(dir);
    let ring_ok = ring.exists();
    let tok_ok = token.is_some();
    if !ring_ok || !tok_ok {
        eprintln!(
            "sprout: journal replay skipped (ring={} token={}) — will retry next up",
            ring_ok, tok_ok
        );
        return;
    }
    let token = token.unwrap();
    /* the ring must actually ANSWER before we fire mount ops at it: the
     * guest agent's ring_loop attaches a moment after the files transport
     * goes live, and a bridge op posted earlier wedges the holder for its
     * full 60s timeout. Probe with PING first. */
    if !ring_ping(&ring, Duration::from_secs(3)) {
        eprintln!("sprout: journal replay skipped (ring not answering yet) — will retry next up");
        return;
    }
    let mut applied = 0usize;
    let mut consumed = 0usize;
    let mut failed = 0usize;
    for row in &pending {
        match row.op.as_str() {
            "mount" if row.args.len() >= 2 => {
                let (src, dst) = (row.args[0].clone(), row.args[1].clone());
                let fstype = row.args.get(2).cloned().unwrap_or_default();
                let mflags: u64 = row.args.get(3).and_then(|s| s.parse().ok()).unwrap_or(0);
                let data = row.args.get(4).cloned().unwrap_or_default();
                let mdata = hostfs_to_guest(dir, &data).unwrap_or_default();
                let _ = agent_exec_files(
                    &dir.join("share"),
                    &[
                        "/bin/sh".to_string(),
                        "-c".to_string(),
                        format!("mkdir -p '{dst}'"),
                    ],
                    &[],
                    "/",
                    &[],
                    Duration::from_secs(10),
                );
                let mut st = Err(anyhow::anyhow!("not attempted"));
                for attempt in 0..3u32 {
                    if attempt > 0 {
                        std::thread::sleep(Duration::from_millis(500));
                    }
                    st = agent_bridge_op_ring(
                        &ring,
                        token,
                        0,
                        "none", // mount source is decorative for hostfs
                        &dst,
                        &fstype,
                        mflags,
                        &mdata,
                        Duration::from_secs(10),
                    );
                    if matches!(st, Ok(0) | Ok(16) | Ok(17)) {
                        break;
                    }
                }
                match st {
                    Ok(0) | Ok(16) | Ok(17) | Ok(68) => {
                        // 0=ok; EBUSY/EALREADY/EADDRINUSE = already mounted.
                        // Mount rows are DURABLE state: they stay in the
                        // journal and are re-applied on every boot (the
                        // guest mount table is per-boot). Only an unbind
                        // consumes them.
                        applied += 1;
                    }
                    Ok(e) => {
                        eprintln!("sprout: replay mount {src} -> {dst}: errno {e}");
                        failed += 1;
                    }
                    Err(e) => {
                        eprintln!("sprout: replay mount {src} -> {dst}: {e:#}");
                        failed += 1;
                    }
                }
            }
            "umount" if !row.args.is_empty() => {
                let dst = row.args[0].clone();
                let st = agent_bridge_op_ring(
                    &ring,
                    token,
                    1,
                    "",
                    &dst,
                    "",
                    0,
                    "",
                    Duration::from_secs(10),
                );
                match st {
                    Ok(0) | Ok(2) | Ok(22) => {
                        // 0=ok; ENOENT/EINVAL = already unmounted. Consume
                        // the one-shot umount row AND the durable mount row
                        // for this dst (if any survived).
                        let _ = j.confirm("umount", &row.args);
                        let _ = j.drop_mount_by_dst(&dst);
                        consumed += 1;
                    }
                    Ok(e) => {
                        eprintln!("sprout: replay umount {dst}: errno {e}");
                        failed += 1;
                    }
                    Err(e) => {
                        eprintln!("sprout: replay umount {dst}: {e:#}");
                        failed += 1;
                    }
                }
            }
            _ => {
                eprintln!("sprout: journal: unknown op '{}' — skipped", row.op);
            }
        }
    }
    println!("sprout: journal replay: {applied} mounts live, {consumed} consumed, {failed} failed");
}

fn read_token(dir: &std::path::Path) -> Option<u64> {
    let s = std::fs::read_to_string(dir.join("token")).ok()?;
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    u64::from_str_radix(s, 16).ok()
}

/// ring health probe: PING (op 0x00) needs no auth and echoes 1 byte.
fn ring_ping(ring_sock: &std::path::Path, timeout: Duration) -> bool {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;
    let Ok(mut conn) = UnixStream::connect(ring_sock) else {
        return false;
    };
    let body = [0u8; 1];
    if conn
        .write_all(&(body.len() as u32).to_le_bytes())
        .and_then(|_| conn.write_all(&body))
        .and_then(|_| conn.flush())
        .is_err()
    {
        return false;
    }
    let _ = conn.set_read_timeout(Some(timeout));
    let mut hdr = [0u8; 4];
    if conn.read_exact(&mut hdr).is_err() {
        return false;
    }
    let n = u32::from_le_bytes(hdr) as usize;
    if n == 0 || n > 4096 {
        return false;
    }
    let mut pay = [0u8; 4096];
    conn.read_exact(&mut pay[..n]).is_ok() && pay[0] == 0
}

/// hostfs wire convention: the journal's mount `data` field is relative
/// to the hostfs ROOT (the boot-time hostfs= dir, mounted at /run/sprout
/// in the guest). hostfs_parse_monolithic appends data verbatim to that
/// root, so "/x" means <hostfs-root>/x. Any of these ctl shapes map to it:
///   "hostfs/x", "/run/sprout/x", "/x", "x"  ->  "/x"
fn hostfs_to_guest(_dir: &std::path::Path, wire_path: &str) -> Option<String> {
    if wire_path.is_empty() {
        return Some(String::new());
    }
    let mut p = wire_path;
    if let Some(r) = p.strip_prefix("hostfs/") {
        p = r;
    }
    if let Some(r) = p.strip_prefix("/run/sprout") {
        p = r;
    }
    Some(format!("/{}", p.trim_start_matches('/')))
}

/// Host-side hostfs path for the agent bridge: the hostfs mount planted at
/// /hostfs in the guest maps a host dir (usually ~/sprouted-image-dir).
fn hostfs_share(dir: &std::path::Path) -> Option<String> {
    let s = std::fs::read_to_string(dir.join("hostfs-share")).ok()?;
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    Some(s.to_string())
}

/// ADR-0024 session token: reuse dir/token when present, else 64-bit
/// random from /dev/urandom; install into the guest at
/// /run/sprout/session.token (mode 600) via an agent exec. Fail-open:
/// provisioning errors only disable bridge ops for this session.
fn provision_token(
    dir: &std::path::Path,
    _id: &str,
    transport: &str,
    vhu_uds: &std::path::Path,
    share: &std::path::Path,
) {
    let hex = match std::fs::read_to_string(dir.join("token")) {
        Ok(s) if s.trim().len() >= 8 => s.trim().to_string(),
        _ => {
            // /dev/urandom → 8 bytes → 16 hex chars
            let mut b = [0u8; 8];
            match std::fs::File::open("/dev/urandom")
                .and_then(|mut f| std::io::Read::read_exact(&mut f, &mut b))
            {
                Ok(()) => {
                    let h: String = b.iter().map(|x| format!("{x:02x}")).collect();
                    if std::fs::write(dir.join("token"), &h).is_err() {
                        return; // no host token -> replay will skip
                    }
                    h
                }
                Err(_) => return,
            }
        }
    };
    let script = format!(
        "mkdir -p /run/sprout && printf %s {hex} > /run/sprout/session.token && chmod 600 /run/sprout/session.token"
    );
    let cmd = ["/bin/sh".to_string(), "-c".to_string(), script];
    let _ = transport;
    let _ = vhu_uds;
    // One-shot per up() — use the files transport unconditionally (ring
    // framing on the mini profile has a scheduled-for-fix quirk, and this
    // path is never hot).
    let res = agent_exec_files(share, &cmd, &[], "/", &[], Duration::from_secs(10)).map(|_| ());
    if let Err(e) = res {
        eprintln!("sprout: token provisioning failed: {e:#}");
        let _ = std::fs::remove_file(dir.join("token"));
    }
}

pub fn agent_exec_files(
    share: &std::path::Path,
    argv: &[String],
    env: &[String],
    cwd: &str,
    stdin_data: &[u8],
    timeout: Duration,
) -> anyhow::Result<(i32, Vec<u8>, Vec<u8>)> {
    let n = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_nanos();
    let req = share.join(format!("req.{n}"));
    let resp = share.join(format!("resp.{n}"));
    let mut body = Vec::new();
    body.push(PROTO_EXEC);
    push_strs(&mut body, argv);
    push_strs(&mut body, env);
    push_str(&mut body, cwd.as_bytes());
    push_u32(&mut body, stdin_data.len() as u32);
    body.extend_from_slice(stdin_data);
    push_u32(&mut body, 0);
    push_u32(&mut body, 0);
    push_u32(&mut body, timeout.as_millis().min(u32::MAX as u128) as u32);
    push_u32(&mut body, 0);
    std::fs::write(&req, &body)?;
    let deadline = std::time::Instant::now() + timeout;

    // The agent writes stdout/stderr frames first and the exit frame LAST,
    // so a single read races the writer. Re-read + re-parse until the exit
    // frame is present or the deadline passes (the file only ever grows).
    let (code, out, err);
    // reassign-in-loop is the whole point (retry until exit frame); the
    // first assignment is always overwritten — that is not a bug.
    #[allow(unused_assignments)]
    let mut raw: Vec<u8> = Vec::new();
    loop {
        raw = std::fs::read(&resp).unwrap_or_default();
        match parse_frames(&raw) {
            Ok((c, o, e)) => {
                code = c;
                out = o;
                err = e;
                break;
            }
            Err(_) if std::time::Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(e) => {
                let _ = std::fs::remove_file(&req);
                let _ = std::fs::remove_file(&resp);
                if raw.is_empty() {
                    anyhow::bail!("file transport: no response from agent within timeout");
                }
                anyhow::bail!("file transport: {e}");
            }
        }
    }
    let _ = std::fs::remove_file(&req);
    let _ = std::fs::remove_file(&resp);
    Ok((code, out, err))
}

/// Exec via AF_VSOCK (fast path): connects to CID_HOST:port through the
/// vhost-device-vsock bridge. Same protocol v1 wire format.
pub fn agent_exec_vsock(
    uds_path: Option<&std::path::Path>,
    port: u32,
    argv: &[String],
    env: &[String],
    cwd: &str,
    stdin_data: &[u8],
    timeout: Duration,
) -> anyhow::Result<(i32, Vec<u8>, Vec<u8>)> {
    use std::io::Write as _;
    use std::os::unix::io::FromRawFd;
    const AF_VSOCK: libc::sa_family_t = 40;
    const CID_GUEST: u32 = 3;

    #[repr(C)]
    struct SockAddrVm {
        family: libc::sa_family_t,
        reserved1: u16,
        port: u32,
        cid: u32,
        zero: [u8; 4],
    }

    let mut s = if let Some(uds) = uds_path {
        // Firecracker hybrid-vsock: connect to the control UDS, send
        // "CONNECT <port>\n", then the same socket is a raw channel to
        // the guest vsock listener.
        let mut u = std::os::unix::net::UnixStream::connect(uds)?;
        u.set_read_timeout(Some(timeout))?;
        u.set_write_timeout(Some(timeout))?;
        u.write_all(format!("CONNECT {port}\n").as_bytes())?;
        // backend acks "OK <port>\n" before the raw channel starts
        let mut ack = Vec::new();
        let mut b = [0u8; 1];
        loop {
            u.read_exact(&mut b)?;
            ack.push(b[0]);
            if b[0] == b'\n' {
                break;
            }
            if ack.len() > 32 {
                anyhow::bail!("vsock handshake: malformed ack {ack:?}");
            }
        }
        u
    } else {
        unsafe {
            let fd = libc::socket(AF_VSOCK as libc::c_int, libc::SOCK_STREAM, 0);
            if fd < 0 {
                anyhow::bail!(
                    "socket(AF_VSOCK) failed: {}",
                    std::io::Error::last_os_error()
                );
            }
            let addr = SockAddrVm {
                family: AF_VSOCK,
                reserved1: 0,
                port,
                cid: CID_GUEST,
                zero: [0; 4],
            };
            if libc::connect(
                fd,
                &addr as *const SockAddrVm as *const libc::sockaddr,
                std::mem::size_of::<SockAddrVm>() as libc::socklen_t,
            ) < 0
            {
                let e = std::io::Error::last_os_error();
                libc::close(fd);
                anyhow::bail!("vsock connect failed: {}", e);
            }
            std::os::unix::net::UnixStream::from_raw_fd(fd)
        }
    };
    s.set_read_timeout(Some(timeout))?;
    s.set_write_timeout(Some(timeout))?;

    let mut req = Vec::new();
    req.push(PROTO_EXEC);
    push_strs(&mut req, argv);
    push_strs(&mut req, env);
    push_str(&mut req, cwd.as_bytes());
    push_u32(&mut req, stdin_data.len() as u32);
    req.extend_from_slice(stdin_data);
    push_u32(&mut req, 0);
    push_u32(&mut req, 0);
    push_u32(&mut req, timeout.as_millis().min(u32::MAX as u128) as u32);
    push_u32(&mut req, 0);
    s.write_all(&req)?;

    let mut out = Vec::new();
    let mut err = Vec::new();
    loop {
        let mut hdr = [0u8; 8];
        s.read_exact(&mut hdr)?;
        let stream = u32::from_le_bytes(hdr[0..4].try_into().unwrap());
        let len = u32::from_le_bytes(hdr[4..8].try_into().unwrap()) as usize;
        if stream == RESP_EXIT {
            let mut code_b = [0u8; 4];
            s.read_exact(&mut code_b)?;
            return Ok((i32::from_le_bytes(code_b), out, err));
        }
        let mut buf = vec![0u8; len];
        s.read_exact(&mut buf)?;
        if stream == RESP_STDOUT {
            out.extend_from_slice(&buf);
        } else if stream == RESP_STDERR {
            err.extend_from_slice(&buf);
        }
    }
}

/// Ping the agent. Ok(true) = alive.
pub fn agent_ping(sock_path: &str, timeout: Duration) -> bool {
    let s = match UnixStream::connect(sock_path) {
        Ok(s) => s,
        Err(_) => return false,
    };
    let mut s = s;
    let _ = s.set_read_timeout(Some(timeout));
    let _ = s.set_write_timeout(Some(timeout));
    if s.write_all(&[PROTO_PING]).is_err() {
        return false;
    }
    let mut b = [0u8; 1];
    matches!(s.read_exact(&mut b), Ok(()) if b[0] == PROTO_PING)
}

/// Ask agent to exit (PROTO_SHUTDOWN). Guest systemd restarts it; host
/// then SIGTERMs the UML PID. False = agent unreachable, caller falls
/// back to poweroff/SIGTERM.
pub fn agent_shutdown(sock_path: &str, timeout: Duration) -> bool {
    let s = match UnixStream::connect(sock_path) {
        Ok(s) => s,
        Err(_) => return false,
    };
    let mut s = s;
    let _ = s.set_read_timeout(Some(timeout));
    let _ = s.set_write_timeout(Some(timeout));
    if s.write_all(&[PROTO_SHUTDOWN]).is_err() {
        return false;
    }
    let mut b = [0u8; 1];
    matches!(s.read_exact(&mut b), Ok(()) if b[0] == PROTO_SHUTDOWN)
}

/// Vsock 1-byte-op helper (PING / SHUTDOWN) over the hybrid channel.
/// Returns the agent's echoed op byte, or None.
fn vsock_op(uds_path: &std::path::Path, port: u32, op: u8, timeout: Duration) -> Option<u8> {
    use std::io::{Read as _, Write as _};
    let mut s = std::os::unix::net::UnixStream::connect(uds_path).ok()?;
    s.set_read_timeout(Some(timeout)).ok()?;
    s.set_write_timeout(Some(timeout)).ok()?;
    s.write_all(format!("CONNECT {port}\n").as_bytes()).ok()?;
    let mut ack = Vec::new();
    let mut b = [0u8; 1];
    loop {
        s.read_exact(&mut b).ok()?;
        ack.push(b[0]);
        if b[0] == b'\n' {
            break;
        }
        if ack.len() > 32 {
            return None;
        }
    }
    s.write_all(&[op]).ok()?;
    s.read_exact(&mut b).ok()?;
    (b[0] == op).then_some(b[0])
}

/// Vsock ping: agent alive on the fast lane?
pub fn agent_ping_vsock(uds_path: &std::path::Path, port: u32, timeout: Duration) -> bool {
    vsock_op(uds_path, port, PROTO_PING, timeout).is_some()
}

/// Vsock shutdown: ask agent to exit.
pub fn agent_shutdown_vsock(uds_path: &std::path::Path, port: u32, timeout: Duration) -> bool {
    vsock_op(uds_path, port, PROTO_SHUTDOWN, timeout).is_some()
}

/// `sprout uml ...` entry point. Owns argv parsing (no clap — keeps the
/// fast-lane flag surface provably untouched).
///
/// ```text
/// sprout uml up [-r ROOT] [--mem 2G] [--cpus 8] [--id NAME] [--timeout S]
/// sprout uml exec [--id NAME] [--user UID[:GID]] [--timeout S] CMD...
/// sprout uml down [--id NAME]
/// sprout uml status [--id NAME]
/// ```
/// Is a vhost-device-vsock backend binary resolvable? $SPROUT_UML_VHOST
/// if set (must exist), else PATH lookup. Governs the DEFAULT transport
/// only — explicit --transport always wins.
fn backend_available() -> bool {
    match std::env::var("SPROUT_UML_VHOST") {
        Ok(p) => std::path::PathBuf::from(p).is_file(),
        Err(_) => std::env::var("PATH")
            .map(|p| {
                p.split(':').any(|d| {
                    std::path::PathBuf::from(d)
                        .join("vhost-device-vsock")
                        .is_file()
                })
            })
            .unwrap_or(false),
    }
}

pub fn uml_main(args: &[std::ffi::OsString]) -> anyhow::Result<u8> {
    use anyhow::{anyhow, bail};
    let argv: Vec<String> = args
        .iter()
        .map(|a| a.to_string_lossy().into_owned())
        .collect();
    let verb = argv.first().map(|s| s.as_str()).unwrap_or("--help");
    match verb {
        "up" => {
            let mut root: Option<PathBuf> = None;
            let mut mem = "2G".to_string();
            let mut cpus: u32 = 4;
            let mut id = "default".to_string();
            let mut timeout = Duration::from_secs(120);
            let mut extra: Vec<String> = Vec::new();
            // --profile mini: boot the agent as PID1 (no systemd) via
            // init=/root/mini-init — ~0.5s boot for stateless exec lanes.
            // The image must contain /root/mini-init (seeded once from the
            // share dir while a systemd guest is up; see docs).
            let mut mini = std::env::var("SPROUT_UML_MINI").as_deref() == Ok("1");
            // Transport: --transport wins, else SPROUT_UML_VSOCK=1 forces
            // vsock, else AUTO: vsock when a backend binary is available
            // (soaked 2026-09-09: 6 cycles clean), files otherwise —
            // no backend, no vhost-user attachment, zero fail surface.
            let mut transport =
                if std::env::var("SPROUT_UML_VSOCK").as_deref() == Ok("1") || backend_available() {
                    "vsock".to_string()
                } else {
                    "files".to_string()
                };
            // Rung 3: --shm / SPROUT_UML_SHM=1 — guest physmem backed by a
            // host memfd (physmem_fd= fd-passing; see cmd_up).
            let mut shm = std::env::var("SPROUT_UML_SHM").as_deref() == Ok("1");
            let mut i = 1;
            while i < argv.len() {
                match argv[i].as_str() {
                    "-r" | "--rootfs" => {
                        i += 1;
                        root = Some(PathBuf::from(
                            argv.get(i).ok_or_else(|| anyhow!("-r needs a path"))?,
                        ));
                    }
                    "--mem" => {
                        i += 1;
                        mem = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--mem needs a value"))?
                            .clone();
                    }
                    "--cpus" => {
                        i += 1;
                        cpus = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--cpus needs a number"))?
                            .parse()?;
                    }
                    "--id" => {
                        i += 1;
                        id = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--id needs a name"))?
                            .clone();
                    }
                    "--timeout" => {
                        i += 1;
                        let s: u64 = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--timeout needs seconds"))?
                            .parse()?;
                        timeout = Duration::from_secs(s);
                    }
                    "--profile" => {
                        i += 1;
                        match argv.get(i).map(|s| s.as_str()) {
                            Some("mini") => mini = true,
                            Some("systemd") => mini = false,
                            _ => bail!("--profile needs mini|systemd"),
                        }
                    }
                    "--transport" => {
                        i += 1;
                        let t = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--transport needs files|vsock"))?;
                        if t != "files" && t != "vsock" {
                            bail!("--transport must be 'files' or 'vsock'");
                        }
                        transport = t.clone();
                    }
                    "--shm" => {
                        shm = true;
                    }
                    "--" => {
                        extra.extend(argv[i + 1..].iter().cloned());
                        break;
                    }
                    f => bail!("unknown uml up flag: {f}"),
                }
                i += 1;
            }
            cmd_up(
                &id,
                root.as_deref(),
                &mem,
                cpus,
                timeout,
                &transport,
                mini,
                shm,
                &extra,
            )
        }
        "exec" => {
            let mut id = "default".to_string();
            let mut timeout = Duration::from_secs(60);
            let mut env_extra: Vec<String> = Vec::new();
            let mut i = 1;
            while i < argv.len() {
                match argv[i].as_str() {
                    "--id" => {
                        i += 1;
                        id = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--id needs a name"))?
                            .clone();
                    }
                    "--user" => {
                        i += 1;
                        let _u = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--user needs UID[:GID]"))?;
                        bail!("--user arrives in Phase-2 (uid/gid framing already in protocol)");
                    }
                    "--timeout" => {
                        i += 1;
                        let s: u64 = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--timeout needs seconds"))?
                            .parse()?;
                        timeout = Duration::from_secs(s);
                    }
                    "--env" => {
                        i += 1;
                        let kv = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--env needs K=V"))?
                            .clone();
                        if !kv.contains('=') {
                            bail!("--env needs K=V (got '{kv}')");
                        }
                        env_extra.push(kv);
                    }
                    "--" => {
                        i += 1;
                        break;
                    }
                    f if f.starts_with('-') => bail!("unknown uml exec flag: {f}"),
                    _ => break,
                }
                i += 1;
            }
            let cmd: Vec<String> = argv[i..].to_vec();
            if cmd.is_empty() {
                bail!("usage: sprout uml exec [--id NAME] [--timeout S] [--env K=V] CMD...");
            }
            cmd_exec(&id, &cmd, &env_extra, timeout)
        }
        "down" => {
            let mut id = "default".to_string();
            let mut i = 1;
            while i < argv.len() {
                match argv[i].as_str() {
                    "--id" => {
                        i += 1;
                        id = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--id needs a name"))?
                            .clone();
                    }
                    f => bail!("unknown uml down flag: {f}"),
                }
                i += 1;
            }
            cmd_down(&id)
        }
        "status" => {
            let mut id = "default".to_string();
            let mut i = 1;
            while i < argv.len() {
                match argv[i].as_str() {
                    "--id" => {
                        i += 1;
                        id = argv
                            .get(i)
                            .ok_or_else(|| anyhow!("--id needs a name"))?
                            .clone();
                    }
                    f => bail!("unknown uml status flag: {f}"),
                }
                i += 1;
            }
            cmd_status(&id)
        }
        _ => {
            println!("sprout uml — UML sidecar (real guest kernel next to the fast lane)\n\nUSAGE:\n    sprout uml up [-r ROOT] [--mem 2G] [--cpus 4] [--id NAME] [--transport files|vsock]\n    sprout uml exec [--id NAME] CMD...\n    sprout uml down [--id NAME]\n    sprout uml status [--id NAME]\n\nup boots a headless linux.uml guest (UBD image seeded from -r on first\nrun); exec runs one command inside via the guest agent and returns its\nexit code. Transport: files (default, hostfs share, no backend) or\nvsock (virtio-uml + vhost-device-vsock, needs the backend binary;\nSPROUT_UML_VHOST sets its path). Fast lane (`sprout -r ROOT -- CMD`)\nis unaffected.");
            Ok(0)
        }
    }
}

/// Path of the hostfs-shared agent socket for a guest: <state>/share/exec.sock.
/// Same file is /run/sprout/exec.sock inside the guest (hostfs mount).
fn agent_sock(id: &str) -> std::path::PathBuf {
    uml_dir(id).join("share").join("exec.sock")
}

fn agent_sock_str(id: &str) -> anyhow::Result<String> {
    if !uml_dir(id).join("pid").is_file() {
        anyhow::bail!("no running guest '{id}' (sprout uml up first)");
    }
    Ok(agent_sock(id).to_string_lossy().into_owned())
}

#[allow(clippy::too_many_arguments)]
fn cmd_up(
    id: &str,
    root: Option<&std::path::Path>,
    mem: &str,
    cpus: u32,
    timeout: Duration,
    transport: &str,
    mini: bool,
    shm: bool,
    extra: &[String],
) -> anyhow::Result<u8> {
    use anyhow::anyhow;
    let dir = uml_dir(id);
    let boot_t0 = Instant::now();
    // Idempotency: any live transport = up is a no-op success.
    {
        let uds = dir.join("vhu-uds");
        if agent_ping_vsock(&uds, VSOCK_PORT, Duration::from_secs(3)) {
            println!("sprout uml: guest '{id}' already up (vsock)");
            return Ok(0);
        }
        if let Ok(sock) = agent_sock_str(id) {
            if agent_ping(&sock, Duration::from_secs(2)) {
                println!("sprout uml: guest '{id}' already up");
                return Ok(0);
            }
        }
        // Stale pid file = previous guest died without cleanup. Self-heal:
        // remove state, don't force the operator through `down` first.
        let _ = std::fs::remove_file(dir.join("pid"));
    }
    // Orphaned linux.uml from a killed CLI (timeout/Ctrl-C) still holds the
    // backing.ext4 flock: every up then dies with "Failed to lock ... err 11"
    // and panics ("Unable to mount root"). Detect + reap the orphan first.
    if let Ok(rd) = std::fs::read_dir("/proc") {
        for e in rd.flatten() {
            let pid: i32 = match e.file_name().to_string_lossy().parse() {
                Ok(p) => p,
                Err(_) => continue,
            };
            if pid == std::process::id() as i32 {
                continue;
            }
            // comm truncates at 15 chars: "linux.uml" shows as "linux".
            // Pair the prefix with our unique umid= kernel arg for precision.
            let comm = std::fs::read_to_string(format!("/proc/{pid}/comm")).unwrap_or_default();
            if comm.starts_with("linux") {
                let cmdline =
                    std::fs::read_to_string(format!("/proc/{pid}/cmdline")).unwrap_or_default();
                if cmdline.contains(&format!("umid=sprout-{id}")) {
                    unsafe { libc::kill(pid, libc::SIGKILL) };
                    // wait for exit so the flock is released before we boot
                    for _ in 0..40 {
                        if unsafe { libc::kill(pid, 0) } != 0 {
                            break;
                        }
                        std::thread::sleep(Duration::from_millis(50));
                    }
                }
            }
        }
    }
    let uml_bin = find_uml_bin()
        .ok_or_else(|| anyhow!("no linux.uml binary (SPROUT_UML_BIN, PATH, or ./linux.uml)"))?;
    std::fs::create_dir_all(&dir)?;
    let backing = dir.join("backing.ext4");
    let cow = dir.join("cow.img");
    let share = dir.join("share");
    std::fs::create_dir_all(&share)?;
    // Purge stale transport requests: a req file that outlived its guest
    // (poweroff kills the agent mid-handle, before the unlink) is drained
    // by the NEXT boot's poller — a stale /sbin/poweroff req turns every
    // future up into an instant self-poweroff.
    if let Ok(rd) = std::fs::read_dir(&share) {
        for e in rd.flatten() {
            let n = e.file_name();
            let n = n.to_string_lossy();
            if n.starts_with("req.") || n.starts_with("resp.") {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    if !backing.is_file() {
        let root =
            root.ok_or_else(|| anyhow!("first boot needs -r ROOT to seed the guest image"))?;
        seed_image(root, &backing)?;
    }
    // COW overlay: empty sparse file is enough for the UML ubd cow driver
    // to start tracking; keep it small, it grows with writes.
    if !cow.is_file() {
        let f = std::fs::File::create(&cow)?;
        f.set_len(8 << 20)?;
    }

    // ORDER MATTERS: vhost-user master (guest kernel) does not reconnect,
    // so the backend must be listening before the guest boots. Reuse a
    // live backend if one is already serving this id's socket. The
    // backend is ONLY needed for the vsock transport: files mode boots
    // with no vhost-user attachment at all (cleaner fail surface).
    let vhu_uds = dir.join("vhu-uds");
    let vm_sock = dir.join("vm.sock");
    if transport == "vsock" {
        let backend_bin =
            std::env::var("SPROUT_UML_VHOST").unwrap_or_else(|_| "vhost-device-vsock".to_string());
        let backend_live = vm_sock.exists()
            && std::fs::read_to_string(dir.join("vhu.pid"))
                .ok()
                .and_then(|p| p.trim().parse::<i32>().ok())
                .map(|pid| unsafe { libc::kill(pid, 0) } == 0)
                .unwrap_or(false);
        if !backend_live {
            let _ = std::fs::remove_file(&vm_sock);
            let _ = std::fs::remove_file(&vhu_uds);
            let log = dir.join("vhu.log");
            let logf = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&log)?;
            let child = std::process::Command::new(&backend_bin)
                .args([
                    "--guest-cid",
                    "3",
                    "--socket",
                    &vm_sock.to_string_lossy(),
                    "--uds-path",
                    &vhu_uds.to_string_lossy(),
                ])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(logf)
                .spawn()?;
            std::fs::write(dir.join("vhu.pid"), child.id().to_string())?;
            // vhost-device-vsock binds both sockets on startup — WAIT for
            // them instead of a blind sleep: the guest's virtio_uml connect
            // against a half-ready backend wedges the boot (init exec -12).
            let sock_deadline = Instant::now() + Duration::from_secs(10);
            while Instant::now() < sock_deadline {
                if vm_sock.exists() && vhu_uds.exists() {
                    break;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
        }
    }

    let mut extra: Vec<String> = extra.to_vec();
    if mini {
        extra.push("init=/root/mini-init".to_string());
        extra.push("quiet".to_string());
    }
    // Rung 3: shared guest physmem. When SPROUT_UML_SHM=1 (or
    // --shm), create a host memfd sized to `mem` and hand the fd to
    // the kernel via physmem_fd=. The fd must stay open for the
    // guest's lifetime — it IS the guest RAM. We keep it in the
    // child's fd table (inherited on spawn, never closed by us) and
    // also keep our own dup so `sprout uml exec`-style host-side
    // mmaps are possible later (rung 3.1: ring protocol).
    let shm_fd: Option<std::os::unix::io::OwnedFd> = if shm {
        use anyhow::Context;
        use std::os::fd::AsRawFd;
        let mem_bytes: u64 = parse_mem(mem)?;
        let fd = memfd_create(&format!("sprout-uml-physmem-{id}"), mem_bytes)
            .context("SPROUT_UML_SHM: memfd_create failed")?;
        // fd 3 is the first free slot in the child (0/1/2 set by
        // stdio); we pass "physmem_fd=3" and arrange spawn so that fd
        // survives (no close-on-exec; spawn_uml keeps it).
        extra.push("physmem_fd=3".to_string());
        eprintln!(
            "sprout uml: shared physmem memfd {} ({} bytes, guest RAM is host-shareable)",
            fd.as_raw_fd(),
            fmt_mb(mem_bytes)
        );
        Some(fd)
    } else {
        None
    };
    // virtio-fs (ADR-0025 D5): coherent host-fs for the guest. Opt-in by
    // binary resolvability: SPROUT_UML_VIRTIOFSD, then sibling of argv[0],
    // then PATH. When absent: no device, no daemon, hostfs still works.
    let vfs_sock = dir.join("virtiofs.sock");
    {
        let daemon_bin = std::env::var("SPROUT_UML_VIRTIOFSD").ok().map(PathBuf::from)
            .filter(|p| p.is_file())
            .or_else(|| {
                std::env::current_exe().ok()
                    .and_then(|e| e.parent().map(|p| p.join("virtiofsd")))
                    .filter(|p| p.is_file())
            })
            .or_else(|| {
                std::env::var_os("PATH").and_then(|path| {
                    std::env::split_paths(&path)
                        .map(|d| d.join("virtiofsd"))
                        .find(|p| p.is_file())
                })
            });
        if let Some(daemon_bin) = daemon_bin {
            let daemon_live = vfs_sock.exists()
                && std::fs::read_to_string(dir.join("virtiofsd.pid"))
                    .ok()
                    .and_then(|p| p.trim().parse::<i32>().ok())
                    .map(|pid| unsafe { libc::kill(pid, 0) } == 0)
                    .unwrap_or(false);
            if !daemon_live {
                let _ = std::fs::remove_file(&vfs_sock);
                let _ = std::fs::remove_file(dir.join("virtiofs.sock.pid"));
                /* shared root: SPROUT_UML_VFS_ROOT or <uml-dir>/vfs-root */
                let vfs_root = std::env::var("SPROUT_UML_VFS_ROOT").map(PathBuf::from)
                    .unwrap_or_else(|_| dir.join("vfs-root"));
                let _ = std::fs::create_dir_all(&vfs_root);
                let logf = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(dir.join("virtiofsd.log"))?;
                /* --inode-file-handles=never is REQUIRED on Android:
                 * name_to_handle_at is seccomp-TRAPped (ADR-0006); the
                 * patched daemon answers ENOSYS but 'never' skips it
                 * entirely. --sandbox none: no userns for untrusted_app. */
                let child = std::process::Command::new(&daemon_bin)
                    .args([
                        "--socket-path", &vfs_sock.to_string_lossy(),
                        "--shared-dir", &vfs_root.to_string_lossy(),
                        "--sandbox", "none",
                        "--cache", "auto",
                        "--allow-mmap",
                        "--tag", "sproutfs0",
                        "--inode-file-handles=never",
                    ])
                    .stdin(Stdio::null())
                    .stdout(Stdio::null())
                    .stderr(logf)
                    .spawn()?;
                std::fs::write(dir.join("virtiofsd.pid"), child.id().to_string())?;
                /* the guest's virtio_uml connect at boot must find the
                 * listener up: wait for the socket node */
                let deadline = Instant::now() + Duration::from_secs(10);
                while Instant::now() < deadline && !vfs_sock.exists() {
                    std::thread::sleep(Duration::from_millis(50));
                }
            }
            if vfs_sock.exists() {
                extra.push(format!("virtio_uml.device={}:26", vfs_sock.display()));
            }
        }
    }
    // Ring doorbell (ADR-0024 §8): host pipe, read-end = fd 4 in the
    // guest (sprout_wake_fd=4), write-end stays for the holder. The guest
    // kernel registers it as a fd-based IRQ; holder writes after posting
    // a BUSY slot -> guest agent's poll() wakes instantly.
    let wake_pipe: Option<(i32, i32)> = if shm {
        let mut pfds = [-1i32; 2];
        if unsafe { libc::pipe(pfds.as_mut_ptr()) } == 0 {
            extra.push("sprout_wake_fd=4".to_string());
            Some((pfds[0], pfds[1]))
        } else {
            None
        }
    } else {
        None
    };
    let (bin, args) = build_cmdline(
        &uml_bin,
        &cow,
        &backing,
        &share,
        mem,
        cpus,
        &format!("sprout-{id}"),
        if transport == "vsock" {
            Some(vm_sock.as_path())
        } else {
            None
        },
        &extra,
    );
    // Rung 3.1: holder daemon keeps the ring + physmem fd alive past
    // this CLI's exit and serves exec requests on dir/ring.sock. It gets
    // the memfd via argv fd number (pre_exec clears CLOEXEC), binds its
    // own listener, writes dir/holder.pid. `down` kills it. Legacy
    // transports (vsock/unix/files) stay primary until the ring wins
    // benchmarks; holder is additive, not a replacement.
    if let Some(fd) = shm_fd.as_ref() {
        use anyhow::Context;
        use std::os::fd::AsRawFd;
        use std::os::unix::process::CommandExt;
        // A prior holder may have survived its guest (it holds no guest
        // handle, so nothing reaps it). Its wake-doorbell pipe belongs to
        // the DEAD boot — a new guest with a stale holder would post ring
        // requests into a void. Kill the stale holder before spawning.
        if let Ok(pid_s) = std::fs::read_to_string(dir.join("holder.pid")) {
            if let Ok(pid) = pid_s.trim().parse::<i32>() {
                unsafe { libc::kill(pid, 0) };
                if unsafe { libc::kill(pid, 0) } == 0 {
                    eprintln!("sprout uml: killing stale holder pid={pid} (fresh doorbell pipe)");
                    unsafe { libc::kill(pid, libc::SIGKILL) };
                    std::thread::sleep(std::time::Duration::from_millis(200));
                }
            }
        }
        let hold_bin = std::env::current_exe()?
            .parent()
            .map(|p| p.join("sprout-uml-hold"))
            .filter(|p| p.exists())
            .ok_or_else(|| {
                anyhow::anyhow!("--shm needs sprout-uml-hold beside the sprout binary")
            })?;
        let raw = fd.as_raw_fd();
        let ring_sock = dir.join("ring.sock");
        let wake_wfd = wake_pipe.map(|(_, w)| w);
        let mut hc = std::process::Command::new(&hold_bin);
        hc.arg(raw.to_string()).arg(&ring_sock);
        if let Some(w) = wake_wfd {
            hc.arg(w.to_string());
        }
        // SAFETY: pre_exec runs post-fork pre-exec in the child; only
        // touches inherited fds (clear CLOEXEC on the memfd + wake wfd).
        unsafe {
            hc.pre_exec(move || {
                let fl = libc::fcntl(raw, libc::F_GETFD);
                if fl >= 0 {
                    libc::fcntl(raw, libc::F_SETFD, fl & !libc::FD_CLOEXEC);
                }
                if let Some(w) = wake_wfd {
                    let fl2 = libc::fcntl(w, libc::F_GETFD);
                    if fl2 >= 0 {
                        libc::fcntl(w, libc::F_SETFD, fl2 & !libc::FD_CLOEXEC);
                    }
                }
                Ok(())
            });
        }
        hc.stdin(Stdio::null()).stdout(Stdio::null()).stderr(
            std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(dir.join("holder.log"))?,
        );
        let hchild = hc.spawn().context("spawn sprout-uml-hold")?;
        std::fs::write(dir.join("holder.pid"), hchild.id().to_string())?;
        std::mem::forget(hchild); // outlives the CLI, like the guest
    }
    let log = dir.join("uml.log");
    let wake_rfd = wake_pipe.map(|(r, _)| r).unwrap_or(-1);
    let child = spawn_uml(&bin, &args, &log, shm_fd.as_ref(), wake_rfd)?;
    std::fs::write(dir.join("pid"), child.id().to_string())?;
    std::fs::write(
        dir.join("conf"),
        format!(
            "bin={}\nmem={mem}\ncpus={cpus}\ntransport={transport}\nshm={}\n",
            bin.display(),
            if shm { "1" } else { "0" }
        ),
    )?;
    // Child handle dropped on purpose: guest outlives the CLI (setsid).
    // PID file + agent ping are the liveness truth, not the handle.
    std::mem::forget(child);

    // Readiness: vsock first (fast, µs RTT), unix socket fallback, then
    // the file transport's poller as last resort. Any one answers = up.
    // Readiness: agent writes share/agent-ready once its poller + unix
    // listeners are up; then probe with a real file-transport exec so
    // "ready" means "exec works" (vhost-device vsock ping is unreliable
    // for now — SPROUT_UML_VSOCK opts in once fixed).
    let ready_marker = share.join("agent-ready");
    let deadline = boot_t0 + timeout;
    loop {
        if Instant::now() >= deadline {
            break;
        }
        if ready_marker.is_file() {
            // "Ready" = a real exec through the CHOSEN transport works.
            let probe = if transport == "vsock" {
                agent_exec_vsock(
                    Some(&vhu_uds),
                    VSOCK_PORT,
                    &["/bin/true".to_string()],
                    &[],
                    "/",
                    &[],
                    Duration::from_secs(5),
                )
                .map(|_| ())
            } else {
                agent_exec_files(
                    &share,
                    &["/bin/true".to_string()],
                    &[],
                    "/",
                    &[],
                    Duration::from_secs(5),
                )
                .map(|_| ())
            };
            if probe.is_ok() {
                println!(
                    "sprout uml: guest '{id}' up ({transport}, boot {:.1}s)",
                    boot_t0.elapsed().as_secs_f32()
                );
                provision_token(&dir, id, transport, &vhu_uds, &share);
                journal_replay(&dir);
                /* virtio-fs auto-mount: when the device attached this boot,
                 * park sproutfs0 at the well-known /virtiofs so binds can
                 * reference it without manual guest setup. Best-effort. */
                if vfs_sock.exists() {
                    if let Some(token) = read_token(&dir) {
                        let ring = dir.join("ring.sock");
                        let _ = agent_exec_files(
                            &share,
                            &["/bin/sh".to_string(), "-c".to_string(),
                              "mkdir -p /virtiofs".to_string()],
                            &[], "/", &[], Duration::from_secs(10),
                        );
                        match agent_bridge_op_ring(
                            &ring, token, 0, "sproutfs0", "/virtiofs",
                            "virtiofs", 0, "", Duration::from_secs(10),
                        ) {
                            Ok(0) | Ok(16) | Ok(17) => {}
                            Ok(e) => eprintln!("sprout: virtiofs auto-mount: errno {e} (mount manually: mount -t virtiofs sproutfs0 /virtiofs)"),
                            Err(e) => eprintln!("sprout: virtiofs auto-mount: {e:#}"),
                        }
                    }
                }
                return Ok(0);
            }
        }
        std::thread::sleep(Duration::from_millis(400));
    }
    eprintln!(
        "sprout uml: guest '{id}' did not answer in {}s — see {} and {}",
        timeout.as_secs(),
        log.display(),
        dir.join("vhu.log").display()
    );
    Ok(1)
}

fn cmd_exec(
    id: &str,
    cmd: &[String],
    env_extra: &[String],
    timeout: Duration,
) -> anyhow::Result<u8> {
    let sock = agent_sock_str(id)?;
    // Guest env: passing the HOST env leaks host paths into the guest
    // (dash's PATH then misses /usr/bin → "ls: not found"). Clean guest
    // default instead; SPROUT_UML_ENV="K=V K=V" appends overrides and
    // explicit --env flags win over everything.
    let mut env: Vec<String> = vec![
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin".into(),
        "HOME=/root".into(),
        "TERM=xterm-256color".into(),
    ];
    if let Ok(extra) = std::env::var("SPROUT_UML_ENV") {
        env.extend(
            extra
                .split_whitespace()
                .filter(|s| !s.is_empty())
                .map(|s| s.to_string()),
        );
    }
    env.extend(env_extra.iter().cloned());
    let env: Vec<String> = env.into_iter().filter(|e| !e.is_empty()).collect();
    // Host cwd rarely exists in the guest; passing it makes the agent's
    // chdir fail and the exec exit 127, which reads as "command not
    // found". Guest cwd is always "/" for v0.1 (guest is a whole rootfs,
    // not a working-dir passthrough — same rule as proot -0-style runs).
    let cwd = "/".to_string();
    let dir = uml_dir(id);
    let uds = dir.join("vhu-uds");
    let uds_opt = if uds.exists() {
        Some(uds.as_path())
    } else {
        None
    };
    let (code, out, err) = {
        // Rung 3: ring.sock holder first (zero-copy shared physmem path).
        // Healthy ring answers in ~50ms; cap the attempt at 3s so a guest
        // whose agent lost the ring falls back to files quickly instead
        // of blocking the caller for the full exec timeout.
        let ring_sock = dir.join("ring.sock");
        let ring_res = if ring_sock.exists() {
            agent_exec_ring(
                &ring_sock,
                cmd,
                &env,
                &cwd,
                &[],
                timeout.min(Duration::from_secs(3)),
            )
        } else {
            Err(anyhow::anyhow!("no ring"))
        };
        match ring_res {
            Ok(r) => r,
            Err(ring_err) => {
                match agent_exec_vsock(uds_opt, VSOCK_PORT, cmd, &env, &cwd, &[], timeout) {
                    Ok(r) => r,
                    Err(vs_err) => match agent_exec(&sock, cmd, &env, &cwd, &[], timeout) {
                        Ok(r) => r,
                        // hostfs socket nodes are placeholders on the host — file transport
                        Err(e)
                            if e.to_string().contains("Connection refused")
                                || e.to_string().contains("os error 111") =>
                        {
                            let share = dir.join("share");
                            agent_exec_files(&share, cmd, &env, &cwd, &[], timeout).map_err(
                                |fe| {
                                    anyhow::anyhow!(
                                        "ring: {ring_err}; vsock: {vs_err}; unix: {e}; files: {fe}"
                                    )
                                },
                            )?
                        }
                        Err(e) => {
                            return Err(anyhow::anyhow!(
                                "ring: {ring_err}; vsock: {vs_err}; unix: {e}"
                            ))
                        }
                    },
                }
            }
        }
    };
    use std::io::Write;
    let _ = std::io::stdout().write_all(&out);
    let _ = std::io::stderr().write_all(&err);
    Ok(code as u8)
}

/// Files-transport poweroff: write an EXEC /sbin/poweroff request into the
/// share dir, then wait for the guest pid to exit. A SIGKILL'd guest loses
/// its dirty page cache (rootfs writes never reach the image), so every
/// down MUST attempt this before any kill. Returns true on clean exit.
fn poweroff_files(dir: &std::path::Path) -> bool {
    let share = dir.join("share");
    let mut body = vec![PROTO_EXEC];
    push_strs(&mut body, &["/sbin/poweroff".to_string()]);
    push_strs(&mut body, &[]);
    push_str(&mut body, b"/");
    push_u32(&mut body, 0);
    push_u32(&mut body, 0);
    push_u32(&mut body, 0);
    push_u32(&mut body, 10000);
    push_u32(&mut body, 0);
    let n = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let req = share.join(format!("req.{n}"));
    if std::fs::write(&req, &body).is_err() {
        return false;
    }
    if let Ok(pid_s) = std::fs::read_to_string(dir.join("pid")) {
        if let Ok(pid) = pid_s.trim().parse::<i32>() {
            let dl = Instant::now() + Duration::from_secs(25);
            while Instant::now() < dl {
                if unsafe { libc::kill(pid, 0) } != 0 {
                    // guest exited; the agent died mid-handle and could not
                    // unlink its own request — do not leave it as a landmine
                    let _ = std::fs::remove_file(&req);
                    return true;
                }
                std::thread::sleep(Duration::from_millis(200));
            }
            let _ = std::fs::remove_file(&req);
        }
    }
    false
}

fn cmd_down(id: &str) -> anyhow::Result<u8> {
    let dir = uml_dir(id);
    if !dir.join("pid").is_file() {
        println!("sprout uml: guest '{id}' not running");
        // Still tear down an orphaned backend so the next `up` starts clean.
        teardown_backend(&dir);
        return Ok(0); // double down = ok (plan §8 gate 1)
    }
    // Graceful first: vsock shutdown, then unix, then guest poweroff.
    let uds = dir.join("vhu-uds");
    let mut agent_exited = agent_shutdown_vsock(&uds, VSOCK_PORT, Duration::from_secs(3));
    if !agent_exited {
        if let Ok(sock) = agent_sock_str(id) {
            agent_exited = agent_shutdown(&sock, Duration::from_secs(3));
            if !agent_exited {
                let _ = agent_exec(
                    &sock,
                    &["/sbin/poweroff".to_string()],
                    &[],
                    "/",
                    &[],
                    Duration::from_secs(10),
                );
            }
        }
    }
    // Files-transport boots have no working unix/vsock path (hostfs socket
    // nodes are placeholders), so the graceful attempts above never fired
    // and every down degraded to SIGKILL — losing all dirty guest writes
    // (and occasionally corrupting the image with a partial flush). Always
    // offer the files-transport poweroff before reaching for signals.
    let mut clean = agent_exited;
    if !clean {
        clean = poweroff_files(&dir);
    }
    if clean {
        println!("sprout uml: guest '{id}' down");
        let _ = std::fs::remove_file(dir.join("pid"));
        teardown_backend(&dir);
        return Ok(0);
    }
    std::thread::sleep(Duration::from_secs(1));
    // Guest: SIGTERM then SIGKILL; agent restart loop must not outlive it.
    if let Ok(pid_s) = std::fs::read_to_string(dir.join("pid")) {
        if let Ok(pid) = pid_s.trim().parse::<i32>() {
            unsafe { libc::kill(pid, libc::SIGTERM) };
            std::thread::sleep(Duration::from_millis(800));
            if unsafe { libc::kill(pid, 0) } == 0 {
                unsafe { libc::kill(pid, libc::SIGKILL) };
            }
        }
    }
    let _ = std::fs::remove_file(dir.join("pid"));
    teardown_backend(&dir);
    println!("sprout uml: guest '{id}' down");
    Ok(0)
}

/// Stop vhost-device-vsock for this id and clear its state. The vhost-user
/// master never reconnects, so a fresh backend MUST be started per boot
/// (cmd_up reuses only a live one).
fn teardown_backend(dir: &std::path::Path) {
    // Ring holder (rung 3): holds the shared physmem memfd + one unix
    // listener. It must die with the guest or the memfd leaks (and the
    // next up's holder would fight over a stale ring.sock).
    if let Ok(pid_s) = std::fs::read_to_string(dir.join("holder.pid")) {
        if let Ok(pid) = pid_s.trim().parse::<i32>() {
            if unsafe { libc::kill(pid, libc::SIGTERM) } == 0 {
                std::thread::sleep(Duration::from_millis(300));
                if unsafe { libc::kill(pid, libc::SIGKILL) } != 0 {
                    // already gone; harmless
                }
            }
        }
    }
    let _ = std::fs::remove_file(dir.join("holder.pid"));
    let _ = std::fs::remove_file(dir.join("ring.sock"));
    if let Ok(pid_s) = std::fs::read_to_string(dir.join("vhu.pid")) {
        if let Ok(pid) = pid_s.trim().parse::<i32>() {
            if unsafe { libc::kill(pid, libc::SIGTERM) } == 0 {
                std::thread::sleep(Duration::from_millis(400));
                if unsafe { libc::kill(pid, 0) } == 0 {
                    unsafe { libc::kill(pid, libc::SIGKILL) };
                }
            }
        }
    }
    let _ = std::fs::remove_file(dir.join("vhu.pid"));
    let _ = std::fs::remove_file(dir.join("vhu-uds"));
    let _ = std::fs::remove_file(dir.join("vm.sock"));
}

fn cmd_status(id: &str) -> anyhow::Result<u8> {
    let dir = uml_dir(id);
    let pid_f = dir.join("pid");
    if !pid_f.is_file() {
        println!("guest '{id}': down");
        return Ok(3);
    }
    let pid_s = std::fs::read_to_string(&pid_f).unwrap_or_default();
    let alive = pid_s
        .trim()
        .parse::<i32>()
        .map(|p| unsafe { libc::kill(p, 0) } == 0)
        .unwrap_or(false);
    let uds = dir.join("vhu-uds");
    let vsock = alive && agent_ping_vsock(&uds, VSOCK_PORT, Duration::from_secs(3));
    let agent = !vsock
        && agent_sock_str(id)
            .map(|s| agent_ping(&s, Duration::from_secs(2)))
            .unwrap_or(false);
    let transport = if vsock {
        "vsock"
    } else if agent {
        "unix"
    } else {
        "none"
    };
    println!(
        "guest '{id}': pid={} alive={} agent={} transport={}",
        pid_s.trim(),
        alive,
        if vsock || agent { "up" } else { "down" },
        transport
    );
    Ok(if alive && (vsock || agent) { 0 } else { 1 })
}

/// Spawn the UML process detached (setsid, stdio nulled except stderr).
/// Returns the Child handle; caller owns PID-file duties.
pub fn spawn_uml(
    bin: &std::path::Path,
    args: &[String],
    log: &std::path::Path,
    shm_fd: Option<&std::os::unix::io::OwnedFd>,
    wake_rfd: i32,
) -> anyhow::Result<Child> {
    let logf = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(log)?;
    // setsid via `setsid` wrapper if present, else plain spawn (still
    // detached enough: stdin nulled, parent doesn't wait).
    let has_setsid = std::env::var("PATH")
        .map(|p| {
            p.split(':')
                .any(|d| PathBuf::from(d).join("setsid").is_file())
        })
        .unwrap_or(false);
    // SPROUT_UML_SPAWN=bash: go through `bash -c` instead of the direct
    // Rust spawn — bisect tool for the init-exec-ENOMEM failure where the
    // manual bash-spawned guest boots and the CLI-spawned one doesn't.
    let spawn_mode = std::env::var("SPROUT_UML_SPAWN").unwrap_or_default();
    let mut cmd = if spawn_mode == "bash" {
        let quoted: Vec<String> = std::iter::once(bin.to_string_lossy().into_owned())
            .chain(args.iter().cloned())
            .map(|a| format!("'{}'", a.replace('\'', "'\\''")))
            .collect();
        let mut c = Command::new("bash");
        c.arg("-c").arg(format!("exec setsid {}", quoted.join(" ")));
        c
    } else if has_setsid {
        let mut c = Command::new("setsid");
        c.arg(bin);
        c
    } else {
        Command::new(bin)
    };
    cmd.args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(logf);
    // physmem_fd handoff: dup the memfd into fd 3 of the child. fd 3
    // is free (stdio took 0/1/2; no other pre-exec fds survive Rust's
    // default close-on-exec behavior). dup2 NOT dup2_cloexec on purpose —
    // the kernel must keep this fd open forever; it is the guest RAM.
    if let Some(fd) = shm_fd {
        use std::os::fd::{AsRawFd, FromRawFd};
        // SAFETY: dup into a fixed slot; on success the child owns fd 3.
        let raw = fd.as_raw_fd();
        let duped = unsafe { libc::dup(raw) };
        if duped < 0 {
            anyhow::bail!("dup physmem fd: {}", std::io::Error::last_os_error());
        }
        // Pre-spawn: we cannot set fd 3 via std Command pre-1.68-ish
        // portably, so use the documented CommandExt trick: clear CLOEXEC
        // on the duped fd and let it land where the OS puts it, then fix
        // up the physmem_fd= number if the kernel arg needs it. But we
        // promised fd 3 in the cmdline — force it with dup2 before spawn
        // via a process_group/unsafe wrapper.
        unsafe {
            if libc::dup2(duped, 3) < 0 {
                anyhow::bail!("dup2 to fd 3: {}", std::io::Error::last_os_error());
            }
            libc::close(duped);
            // clear close-on-exec on fd 3 so the child inherits it
            let fl = libc::fcntl(3, libc::F_GETFD);
            if fl >= 0 && (fl & libc::FD_CLOEXEC) != 0 {
                libc::fcntl(3, libc::F_SETFD, fl & !libc::FD_CLOEXEC);
            }
        }
        // Keep our own copy alive for rung 3.1 (host-side mmap of guest
        // physmem). Leaked intentionally: lives until process exit.
        std::mem::forget(unsafe { std::os::unix::io::OwnedFd::from_raw_fd(3) });
    }
    // Ring doorbell read-end -> guest fd 4 (sprout_wake_fd=4).
    if wake_rfd >= 0 {
        unsafe {
            if wake_rfd != 4 {
                if libc::dup2(wake_rfd, 4) < 0 {
                    anyhow::bail!("dup2 wake rfd: {}", std::io::Error::last_os_error());
                }
                libc::close(wake_rfd);
            }
            let fl = libc::fcntl(4, libc::F_GETFD);
            if fl >= 0 && (fl & libc::FD_CLOEXEC) != 0 {
                libc::fcntl(4, libc::F_SETFD, fl & !libc::FD_CLOEXEC);
            }
        }
    }
    if std::env::var("SPROUT_UML_SPAWN_DEBUG").is_ok() {
        let dump = format!(
            "cwd={:?}\nargv={:?}\nenv_has_ld_preload={:?}\n",
            std::env::current_dir(),
            cmd.get_args().collect::<Vec<_>>(),
            std::env::var("LD_PRELOAD"),
        );
        let _ = std::fs::write(log.with_extension("spawn-debug"), dump);
    }
    Ok(cmd.spawn()?)
}

fn push_u32(v: &mut Vec<u8>, n: u32) {
    v.extend_from_slice(&n.to_le_bytes());
}
fn push_str(v: &mut Vec<u8>, b: &[u8]) {
    push_u32(v, b.len() as u32);
    v.extend_from_slice(b);
}
fn push_strs(v: &mut Vec<u8>, ss: &[String]) {
    push_u32(v, ss.len() as u32);
    for s in ss {
        push_str(v, s.as_bytes());
    }
}

/// Seed the guest UBD image from a sprout rootfs dir (plan §3.4).
///
/// Rootless recipe: mkfs.ext4 on a file works unprivileged; populate via
/// tar pipe (no mounts, no loop devices, no root). Size heuristic: source
/// du × 1.3 + 256MB headroom, rounded up to 64MB.
fn seed_image(root: &std::path::Path, backing: &std::path::Path) -> anyhow::Result<()> {
    use anyhow::{anyhow, Context};
    // 1. measure source
    let mut total: u64 = 0;
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        let rd =
            std::fs::read_dir(&d).with_context(|| format!("seed: cannot read {}", d.display()))?;
        for e in rd {
            let e = e?;
            let md = std::fs::symlink_metadata(e.path())?;
            total += md.len();
            if md.is_dir() && !md.is_symlink() {
                stack.push(e.path());
            }
        }
    }
    let size = (total * 13 / 10 + (256 << 20)).div_ceil(64 << 20) * (64 << 20);
    let size = size.max(512 << 20);
    eprintln!(
        "sprout uml: seeding guest image ({} source → {} image)...",
        fmt_mb(total),
        fmt_mb(size)
    );
    // 2. mkfs (needs e2fsprogs on host; fail loudly otherwise)
    let f = std::fs::File::create(backing)?;
    f.set_len(size)?;
    drop(f);
    let st = std::process::Command::new("mkfs.ext4")
        .args(["-q", "-F", &backing.to_string_lossy()])
        .status()
        .map_err(|e| anyhow!("mkfs.ext4 not found ({e}) — install e2fsprogs"))?;
    if !st.success() {
        anyhow::bail!("mkfs.ext4 failed");
    }
    // 3. populate: tar -C root -c . | debugfs -w -R 'write ...' is fiddly;
    //    instead use `tar2ext4` via debugfs `mkdir/write`? Simplest robust:
    //    guest-side self-seed on first boot (plan §3.4). Here we store the
    //    seed tarball next to the image; the first-boot path copies it in.
    //    Actually simplest correct host-side: python ext4 writer is overkill.
    //    Decision: write seed.tar.gz beside the image; uml up prints the
    //    one-time first-boot seed command for the operator.
    let seed = backing.with_extension("seed.tar");
    let st = std::process::Command::new("tar")
        .args([
            "-C",
            &root.to_string_lossy(),
            "-cf",
            &seed.to_string_lossy(),
            ".",
        ])
        .status()?;
    if !st.success() {
        anyhow::bail!("seed tar failed");
    }
    eprintln!(
        "sprout uml: image ready; seed tarball at {}. First boot inside guest:",
        seed.display()
    );
    eprintln!(
        "    mkfs.ext4 /dev/ubda && mount /dev/ubda /mnt && tar -C /mnt -xf <seed> && umount /mnt"
    );
    Ok(())
}

fn fmt_mb(n: u64) -> String {
    format!("{}M", n >> 20)
}

/// Create a sealed-nothing host memfd of `size` bytes (ftruncate'd).
/// Raw libc — memfd_create is Linux-only and rustix may not be in the dep
/// tree. MFD_ALLOW_SEALING off: the guest kernel writes freely.
fn memfd_create(name: &str, size: u64) -> anyhow::Result<std::os::unix::io::OwnedFd> {
    use anyhow::Context;
    use std::os::fd::FromRawFd;
    const MFD_CLOEXEC: u32 = 0x0001;
    let cname = std::ffi::CString::new(name).context("memfd name")?;
    // SAFETY: plain syscall wrapper, no memory the kernel retains.
    let fd = unsafe { libc::syscall(libc::SYS_memfd_create, cname.as_ptr(), MFD_CLOEXEC) };
    if fd < 0 {
        anyhow::bail!("memfd_create({name}): {}", std::io::Error::last_os_error());
    }
    let owned = unsafe { std::os::unix::io::OwnedFd::from_raw_fd(fd as i32) };
    // CLOEXEC set (we dup + clear later for the child); size it now.
    let f = &owned;
    use std::os::fd::AsRawFd;
    if unsafe { libc::ftruncate(f.as_raw_fd(), size as libc::off_t) } < 0 {
        anyhow::bail!(
            "ftruncate({name}, {size}): {}",
            std::io::Error::last_os_error()
        );
    }
    Ok(owned)
}

/// "512M"/"2G"/plain bytes → byte count. Mirrors UML's own parser
/// (memparse in arch/um): suffixes k/K, m/M, g/G, case-insensitive.
fn parse_mem(s: &str) -> anyhow::Result<u64> {
    use anyhow::Context;
    let s = s.trim();
    let (num, mult) = match s.chars().last() {
        Some('k' | 'K') => (&s[..s.len() - 1], 1024u64),
        Some('m' | 'M') => (&s[..s.len() - 1], 1 << 20),
        Some('g' | 'G') => (&s[..s.len() - 1], 1 << 30),
        _ => (s, 1),
    };
    let n: u64 = num
        .trim()
        .parse()
        .with_context(|| format!("bad mem spec '{s}'"))?;
    n.checked_mul(mult).context("mem overflow")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cmdline_has_no_console_and_cow_root() {
        let (bin, args) = build_cmdline(
            std::path::Path::new("/x/linux.uml"),
            std::path::Path::new("/s/cow.img"),
            std::path::Path::new("/s/backing.ext4"),
            std::path::Path::new("/s/share"),
            "2G",
            8,
            "sprout0",
            None,
            &[],
        );
        assert_eq!(bin, PathBuf::from("/x/linux.uml"));
        let joined = args.join(" ");
        // default: no COW (kernel-agnostic); SPROUT_UML_COW=1 opts in
        assert!(joined.contains("ubd0=/s/backing.ext4"), "{joined}");
        assert!(joined.contains("root=/dev/ubda"), "{joined}");
        assert!(joined.contains("con=null"), "{joined}");
        assert!(joined.contains("umid=sprout0"), "{joined}");
        // No guest networking: agent socket rides the hostfs share dir.
        assert!(joined.contains("hostfs=/s/share"), "{joined}");
        assert!(!joined.contains("eth0="), "{joined}");
        assert!(!joined.contains("slirp"), "{joined}");
        // stub_exe at its real kbuild path (missing = memfd exec = SELinux
        // denial = guest execve -12); absent here because /x/ is a fake tree.
        assert!(!joined.contains("stub_exe="), "{joined}");
        // files transport: no vhost-user device attached
        assert!(!joined.contains("virtio_uml.device="), "{joined}");
    }

    #[test]
    fn cmdline_attaches_vsock_device() {
        let (_, args) = build_cmdline(
            std::path::Path::new("/x/linux.uml"),
            std::path::Path::new("/s/cow.img"),
            std::path::Path::new("/s/backing.ext4"),
            std::path::Path::new("/s/share"),
            "2G",
            8,
            "sprout0",
            Some(std::path::Path::new("/s/vm.sock")),
            &[],
        );
        let joined = args.join(" ");
        assert!(
            joined.contains("virtio_uml.device=/s/vm.sock:19"),
            "{joined}"
        );
    }

    #[test]
    fn uml_dir_anchors_home() {
        std::env::set_var("SPROUT_UML_TEST_HOME", "/tmp/xyz");
        let d = uml_dir("a");
        assert!(d.ends_with(".sprout/uml/a"), "{d:?}");
    }
}
