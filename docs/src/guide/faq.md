# FAQ

**Why not just use proot?**
proot is a mature, well-designed tool. sprout exists because proot pays
for generality with two context switches per syscall, and on phones that
cost shows up in every git clone, every node cold-start, every editor
plugin. sprout keeps proot's coverage goals but changes the hot path.

**Why not proroot?**
proroot has the right architecture and ships closed-source binaries with
no docs. sprout is the open, auditable, documented version of the same
design, plus static/Go coverage.

**Is this a sandbox?**
No. See the [threat model](../architecture/threat-model.md).

**Does it need root?**
No. Everything runs as the Termux (or embedding app's) UID.

**Why C for the .so, Rust for the rest?**
The `.so` must interpose libc symbols without allocations, panics, or
runtime init. The launcher is the opposite problem (CLI UX, error
messages, plan construction). ADR-0001 has the full rationale.

**What about Go / static binaries?**
Detected by ELF scan and routed to an automatic ptrace fallback (v0.3).
Nothing is silently broken; you either get the fast path or a clear error.

**Alpine / musl?**
v0.4. The rootfs model is libc-agnostic so this is additive.

**x86_64 on arm64?**
Outside v1.0 scope (an emulation problem, not a path problem). Watch
Box64 integration after 1.0.

**An app SEGFAULTs in my guest — sprout's fault?**
Check the control lane first: if the same binary under `proot` (or
`proot-distro login ...`) crashes identically, the app is simply broken in
containerized environments and no interposer can fix that. Confirmed
upstream-broken examples (2026-08, ubuntu-resolute guest, crash identical in
both lanes): `kcm-touchpad-list-devices` (null-deref when no display/dbus),
`glxdemo`/`glxinfo`-family without `DISPLAY`, `aa-features-abi --version`
(misuse path), all the `aa-*` AppArmor tools on kernels without AppArmor
FS. The other proot-distro control-lane doctrine (`proot` is the
is-functional baseline) is the canonical split: identical-in-proot ⇒
not a sprout bug; clean-in-proot ⇒ file it on us.
