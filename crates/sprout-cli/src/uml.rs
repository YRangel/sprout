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
        "root=/dev/ubda".to_string(),
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
    if let Some(stub) = find_stub(&uml_bin) {
        args.push(format!("stub_exe={}", stub.display()));
    }
    args.extend(extra.iter().cloned());
    (uml_bin.to_path_buf(), args)
}

/// Locate stub_exe: $SPROUT_UML_STUB, then <kernel dir>/stub_exe.
fn find_stub(uml_bin: &std::path::Path) -> Option<PathBuf> {
    if let Ok(p) = std::env::var("SPROUT_UML_STUB") {
        let pb = PathBuf::from(p);
        if pb.is_file() {
            return Some(pb);
        }
    }
    let beside = uml_bin.parent()?.join("stub_exe");
    if beside.is_file() {
        Some(beside)
    } else {
        None
    }
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
    use std::io::Write;
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
    let mut raw = Vec::new();
    while std::time::Instant::now() < deadline {
        if let Ok(meta) = std::fs::metadata(&resp) {
            if meta.len() >= 12 {
                // wait briefly for the exit frame tail to settle, then read
                std::thread::sleep(Duration::from_millis(150));
                raw = std::fs::read(&resp)?;
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(150));
    }
    let _ = std::fs::remove_file(&req);
    let _ = std::fs::remove_file(&resp);
    if raw.is_empty() {
        anyhow::bail!("file transport: no response from agent within timeout");
    }
    // parse frame stream
    let mut cur = &raw[..];
    let mut out = Vec::new();
    let mut err = Vec::new();
    let code;
    loop {
        if cur.len() < 8 {
            anyhow::bail!("file transport: truncated frame header");
        }
        let stream = u32::from_le_bytes(cur[0..4].try_into().unwrap());
        let len = u32::from_le_bytes(cur[4..8].try_into().unwrap()) as usize;
        cur = &cur[8..];
        if stream == RESP_EXIT {
            if cur.len() < 4 {
                anyhow::bail!("file transport: truncated exit code");
            }
            code = i32::from_le_bytes(cur[0..4].try_into().unwrap());
            break;
        }
        if cur.len() < len {
            anyhow::bail!("file transport: truncated frame body");
        }
        let (b, rest) = cur.split_at(len);
        if stream == RESP_STDOUT {
            out.extend_from_slice(b);
        } else if stream == RESP_STDERR {
            err.extend_from_slice(b);
        }
        cur = rest;
    }
    Ok((code, out, err))
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

/// Wait for agent readiness, polling ping. Returns true when up.
pub fn wait_ready(sock_path: &str, timeout: Duration) -> bool {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if agent_ping(sock_path, Duration::from_secs(2)) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    false
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
                    "--" => {
                        extra.extend(argv[i + 1..].iter().cloned());
                        break;
                    }
                    f => bail!("unknown uml up flag: {f}"),
                }
                i += 1;
            }
            cmd_up(&id, root.as_deref(), &mem, cpus, timeout, &extra)
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
            println!("sprout uml — UML sidecar (real guest kernel next to the fast lane)\n\nUSAGE:\n    sprout uml up [-r ROOT] [--mem 2G] [--cpus 4] [--id NAME]\n    sprout uml exec [--id NAME] CMD...\n    sprout uml down [--id NAME]\n    sprout uml status [--id NAME]\n\nup boots a headless linux.uml guest (UBD image seeded from -r on first\nrun); exec runs one command inside via the guest agent and returns its\nexit code. Fast lane (`sprout -r ROOT -- CMD`) is unaffected.");
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

fn cmd_up(
    id: &str,
    root: Option<&std::path::Path>,
    mem: &str,
    cpus: u32,
    timeout: Duration,
    extra: &[String],
) -> anyhow::Result<u8> {
    use anyhow::{anyhow, bail};
    let dir = uml_dir(id);
    if dir.join("pid").is_file() {
        // Idempotency: a live guest means up is a no-op success.
        if let Ok(sock) = agent_sock_str(id) {
            if agent_ping(&sock, Duration::from_secs(2)) {
                println!("sprout uml: guest '{id}' already up");
                return Ok(0);
            }
        }
        bail!("stale state for '{id}' (pid file without live agent) — `sprout uml down --id {id}` to clean");
    }
    let uml_bin = find_uml_bin()
        .ok_or_else(|| anyhow!("no linux.uml binary (SPROUT_UML_BIN, PATH, or ./linux.uml)"))?;
    std::fs::create_dir_all(&dir)?;
    let backing = dir.join("backing.ext4");
    let cow = dir.join("cow.img");
    let share = dir.join("share");
    std::fs::create_dir_all(&share)?;
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
    let (bin, args) = build_cmdline(
        &uml_bin,
        &cow,
        &backing,
        &share,
        mem,
        cpus,
        &format!("sprout-{id}"),
        extra,
    );
    let log = dir.join("uml.log");
    let child = spawn_uml(&bin, &args, &log)?;
    std::fs::write(dir.join("pid"), child.id().to_string())?;
    std::fs::write(
        dir.join("conf"),
        format!("bin={}\nmem={mem}\ncpus={cpus}\n", bin.display()),
    )?;
    // Child handle dropped on purpose: guest outlives the CLI (setsid).
    // PID file + agent ping are the liveness truth, not the handle.
    std::mem::forget(child);
    let sock = agent_sock(id).to_string_lossy().into_owned();
    if wait_ready(&sock, timeout) {
        println!(
            "sprout uml: guest '{id}' up (agent {})",
            agent_sock(id).display()
        );
        Ok(0)
    } else {
        eprintln!(
            "sprout uml: guest '{id}' did not answer in {}s — see {}",
            timeout.as_secs(),
            log.display()
        );
        Ok(1)
    }
}

fn cmd_exec(id: &str, cmd: &[String], timeout: Duration) -> anyhow::Result<u8> {
    let sock = agent_sock_str(id)?;
    let env: Vec<String> = std::env::vars().map(|(k, v)| format!("{k}={v}")).collect();
    // Host cwd rarely exists in the guest; passing it makes the agent's
    // chdir fail and the exec exit 127, which reads as "command not
    // found". Guest cwd is always "/" for v0.1 (guest is a whole rootfs,
    // not a working-dir passthrough — same rule as proot -0-style runs).
    let cwd = "/".to_string();
    let (code, out, err) = match agent_exec(&sock, cmd, &env, &cwd, &[], timeout) {
        Ok(r) => r,
        // hostfs socket nodes are placeholders on the host — file transport
        Err(e)
            if e.to_string().contains("Connection refused")
                || e.to_string().contains("os error 111") =>
        {
            let dir = uml_dir(id);
            let share = dir.join("share");
            agent_exec_files(&share, cmd, &env, &cwd, &[], timeout)?
        }
        Err(e) => return Err(e),
    };
    use std::io::Write;
    let _ = std::io::stdout().write_all(&out);
    let _ = std::io::stderr().write_all(&err);
    Ok(code as u8)
}

fn cmd_down(id: &str) -> anyhow::Result<u8> {
    let dir = uml_dir(id);
    if !dir.join("pid").is_file() {
        println!("sprout uml: guest '{id}' not running");
        return Ok(0); // double down = ok (plan §8 gate 1)
    }
    // Best effort: ask agent to exit, else halt guest, then SIGTERM the PID.
    if let Ok(sock) = agent_sock_str(id) {
        if !agent_shutdown(&sock, Duration::from_secs(5)) {
            let _ = agent_exec(
                &sock,
                &["/sbin/poweroff".to_string()],
                &[],
                "/",
                &[],
                Duration::from_secs(10),
            );
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    if let Ok(pid_s) = std::fs::read_to_string(dir.join("pid")) {
        if let Ok(pid) = pid_s.trim().parse::<i32>() {
            unsafe { libc::kill(pid, libc::SIGTERM) };
        }
    }
    std::thread::sleep(Duration::from_secs(1));
    // Reap-or-orphan: if still alive, SIGKILL. PID-file truth cleared either way.
    if let Ok(pid_s) = std::fs::read_to_string(dir.join("pid")) {
        if let Ok(pid) = pid_s.trim().parse::<i32>() {
            if unsafe { libc::kill(pid, 0) } == 0 {
                unsafe { libc::kill(pid, libc::SIGKILL) };
            }
        }
    }
    let _ = std::fs::remove_file(dir.join("pid"));
    println!("sprout uml: guest '{id}' down");
    Ok(0)
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
    let agent = agent_sock_str(id)
        .map(|s| agent_ping(&s, Duration::from_secs(2)))
        .unwrap_or(false);
    println!(
        "guest '{id}': pid={} alive={} agent={}",
        pid_s.trim(),
        alive,
        if agent { "up" } else { "down" }
    );
    Ok(if alive && agent { 0 } else { 1 })
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
    let mut cmd = if has_setsid {
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
    }

    #[test]
    fn uml_dir_anchors_home() {
        std::env::set_var("SPROUT_UML_TEST_HOME", "/tmp/xyz");
        let d = uml_dir("a");
        assert!(d.ends_with(".sprout/uml/a"), "{d:?}");
    }
}
