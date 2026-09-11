//! ADR-0024 §9: intent journal.
//!
//! The holder (single writer to the shadow table) also records intent for
//! every mount-ish op it performed. On `sprout uml up`, `fn journal_replay`
//! forwards any rows whose state is `intent=true` to the guest agent,
//! which re-executes them inside the guest, finally marks rows state=applied.
//! A crash between 'scribe' and 'confirm' yields a replay of an already-applied
//! intent (mount returns EBUSY / umount ENOENT) — that is a no-op.
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct Row {
    pub intent: bool,
    pub op: String,           // mount|umount|proc-read|...
    pub args: Vec<String>,
}

// Guest-hostfs view of the holder's share dir. The guest mounts the
// AGENT's share dir at /run/sprout (init script, per kernel cmdline).
// Therefore a ctl arg "/foo" → guest path "/run/sprout/foo". Relative
// ctl args are also guest-side (never against host absolute).
pub fn to_guest_hostfs(a: &str) -> String {
    if a.is_empty() {
        String::new()
    } else {
        format!("/run/sprout/share/{}", a.trim_start_matches('/'))
    }
}

pub struct Journal {
    pub path: PathBuf,
}

impl Journal {
    pub fn open(dir: &Path) -> Self {
        Self { path: dir.join("journal.log") }
    }

    pub fn append(&self, r: &Row) -> std::io::Result<()> {
        let mut body = String::new();
        body.push_str(if r.intent { "I\t" } else { "." });
        body.push_str(&r.op);
        for a in &r.args {
            body.push('\t');
            body.push_str(a);
        }
        body.push('\n');
        let mut f = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)?;
        use std::io::Write;
        f.write_all(body.as_bytes())
    }

    pub fn pending(&self) -> std::io::Result<Vec<Row>> {
        /* Fresh boot -> no file -> empty pending (no Err). */
        let s = std::fs::read_to_string(&self.path).unwrap_or_default();
        let mut out = Vec::new();
        for line in s.lines() {
            if line.is_empty() || !line.starts_with("I\t") { continue; }
            let parts: Vec<&str> = line.split('\t').collect();
            if parts.len() < 2 { continue; }
            out.push(Row {
                intent: true,
                op: parts[1].to_string(),
                args: parts[2..].iter().map(|s| s.to_string()).collect(),
            });
        }
        Ok(out)
    }

    /// Remove exactly one `I` row matching (op,args) — used after the
    /// agent CONFIRMS an intent post-replay.
    pub fn confirm(&self, op: &str, args: &[String]) -> std::io::Result<()> {
        let s = std::fs::read_to_string(&self.path).unwrap_or_default();
        let mut out = String::new();
        let mut done = false;
        for line in s.lines() {
            if !done && !line.is_empty() && line.starts_with("I\t") {
                let parts: Vec<&str> = line.split('\t').collect();
                if parts.len() >= 2 && parts[1] == op &&
                   parts[2..].iter().zip(args.iter()).all(|(a, b)| a == b)
                   && parts.len() - 2 == args.len()
                { done = true; continue; } // drop
            }
            out.push_str(line); out.push('\n');
        }
        std::fs::write(&self.path, out)
    }

    pub fn rotate_applied(&self) -> std::io::Result<()> {
        let s = std::fs::read_to_string(&self.path).unwrap_or_default();
        let mut out = String::new();
        for line in s.lines() {
            if line.is_empty() { continue; }
            if line.starts_with("I\t") { continue; }   // drop intents
            out.push_str(line); out.push('\n');
        }
        std::fs::write(&self.path, out)
    }
}
