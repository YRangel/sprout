# Security Policy

## Scope

sprout runs untrusted guest binaries. Issues in scope:

- path-translation bugs that let the guest read or write **outside** the
  declared bindings + rootfs without intending to
- ELF parsing crashes (doS / controlled OOB)
- bugs in config parsing (`SPROUT_*` env) that produce more than the user asked for

Out of scope: the guest binary itself being malicious (that is the use
case, see `docs/src/architecture/threat-model.md`), denial-of-service from
a hostile guest (no sandbox promises), and issues that require the reporter
to already have code execution *inside* the guest.

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
