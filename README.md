# sprout

**Rootless glibc Linux userspace for Android. Fast, auditable, open.**

`sprout` runs full Ubuntu-style glibc environments (Node.js, Python, Git, Chromium) on
Android without root. It is a drop-in replacement for [proot](https://proot-me.github.io/)
(including the Termux `proot-distro` CLI) with an `LD_PRELOAD` fast path that avoids
ptrace syscall-stop overhead, plus an automatic ptrace fallback for static/Go binaries.

* **Zero-overhead fast path** — path translation happens in-process via `LD_PRELOAD`
  interposition, not `ptrace` (2 context switches per syscall on proot, 0 here).
* **Honest coverage** — dynamically-linked glibc binaries take the fast path; static
  and Go binaries are detected by ELF scan and routed to a ptrace fallback
  automatically, so they *work* instead of silently breaking.
* **Auditable** — open source (MIT/Apache-2.0), documented architecture, ADRs for every
  major decision, reproducible benchmarks. No `.text` binary patching.

> Status: **v0.1 in development.** See [`docs/src/adr/`](docs/src/adr/) for decisions and
> [`docs/src/`](docs/src/) for the book.

## Quick reference

```console
$ sprout -r ~/ubuntu -w /root -0 --link2symlink -- /bin/bash -l
$ sprout -r ~/ubuntu /usr/bin/node server.js
$ sprout --fallback=ptrace -r ~/ubuntu /usr/local/bin/static-go-server
```

`proot` CLI compatibility is a goal: `sprout` accepts `-r -b -q -0 --link2symlink -w`
with the same semantics, so `proot-distro login` can be pointed at it.

## Layout

```
crates/
  sprout-cli      — the `sprout` binary (Rust)
  sprout-core     — rootfs/ELF/strategy library (Rust)
  sprout-preload  — path-translation .so (C11; LD_PRELOAD interposer)
docs/             — mdBook (architecture, ADRs, guides)
```

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE) at your option.
