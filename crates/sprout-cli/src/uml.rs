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
                &extra,
            )
        }
        "exec" => {
            let mut id = "default".to_string();
            let mut timeout = Duration::from_secs(60);
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
                bail!("usage: sprout uml exec [--id NAME] [--timeout S] CMD...");
            }
            cmd_exec(&id, &cmd, timeout)
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
    let log = dir.join("uml.log");
    let child = spawn_uml(&bin, &args, &log)?;
    std::fs::write(dir.join("pid"), child.id().to_string())?;
    std::fs::write(
        dir.join("conf"),
        format!(
            "bin={}\nmem={mem}\ncpus={cpus}\ntransport={transport}\n",
            bin.display()
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

fn cmd_exec(id: &str, cmd: &[String], timeout: Duration) -> anyhow::Result<u8> {
    let sock = agent_sock_str(id)?;
    let env: Vec<String> = std::env::vars().map(|(k, v)| format!("{k}={v}")).collect();
    // Host cwd rarely exists in the guest; passing it makes the agent's
    // chdir fail and the exec exit 127, which reads as "command not
    // found". Guest cwd is always "/" for v0.1 (guest is a whole rootfs,
    // not a working-dir passthrough — same rule as proot -0-style runs).
    let cwd = "/".to_string();
    let uds = uml_dir(id).join("vhu-uds");
    let uds_opt = if uds.exists() {
        Some(uds.as_path())
    } else {
        None
    };
    let (code, out, err) =
        match agent_exec_vsock(uds_opt, VSOCK_PORT, cmd, &env, &cwd, &[], timeout) {
            Ok(r) => r,
            Err(vs_err) => match agent_exec(&sock, cmd, &env, &cwd, &[], timeout) {
                Ok(r) => r,
                // hostfs socket nodes are placeholders on the host — file transport
                Err(e)
                    if e.to_string().contains("Connection refused")
                        || e.to_string().contains("os error 111") =>
                {
                    let dir = uml_dir(id);
                    let share = dir.join("share");
                    agent_exec_files(&share, cmd, &env, &cwd, &[], timeout)
                        .map_err(|fe| anyhow::anyhow!("vsock: {vs_err}; unix: {e}; files: {fe}"))?
                }
                Err(e) => return Err(anyhow::anyhow!("vsock: {vs_err}; unix: {e}")),
            },
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
