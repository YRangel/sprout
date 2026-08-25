# Security Policy

## Threat premise

sprout is a **launcher**, not a sandbox. Its job is to make a guest Linux
rootfs *usable*, not to contain it. The app-uid boundary that Android
already enforces (every Termux process runs under the same unprivileged
app id) is the security boundary — sprout stays inside it.

Concretely: **anything a guest binary can do under sprout is achievable by
a native Termux binary** the user could run by hand. There is no privilege
escalation; the guest never gains a capability the host shell didn't
already have. Guests that are deliberately malicious are the user's own
responsibility — convincing yourself to run a binary is the decision that
carries the risk, not sprout.

This shapes what "a sprout security bug" even means below.

## Scope

In scope — bugs where sprout grants MORE than running the same thing
natively under Termux would:

- path-translation bugs that let the guest read or write **outside** the
  declared bindings + rootfs without intending to
- ELF parsing crashes (controlled OOB against the sprout process itself)
- bugs in config parsing (`SPROUT_*` env) that produce more than the user asked for
- the fake-id (`--user`) malfunctioning so the guest gains an identity it
  wasn't configured for

Out of scope — the threat premise itself:

- anything the guest binary does, by design ("guest is malicious" is the
  use case, not a bug)
- denial-of-service from a hostile guest (no sandbox promises; kill it
  from Termux)
- confinement failures (there is no confinement to fail)
- issues that require code execution *inside* the guest to demonstrate

If it would be equally runnable under Termux by hand, it isn't a sprout
security bug. See `docs/src/architecture/threat-model.md` for the
full rationale.

## Reporting

**Email:** <to-be-registered> — until then, open a *private* GitHub
Security Advisory on the repo. Do **not** open a public issue.

You should get an acknowledgment within 72 hours and a triage decision
within 7 days. We follow coordinated disclosure with a 90-day default
embargo, negotiated down when a fix is trivially ready.

## Securing sprout itself

- No `.text` patching: if someone proposes it, reject the change.
- No `unsafe` in Rust without the exact preconditions documented at the
  unsafe block.
- New interposed symbols land with a test that fails if the translation
  diverges from the syscall's real path.
- The config is fixed-size and bounds-checked; adding a new field requires
  review of the implied attack surface.
