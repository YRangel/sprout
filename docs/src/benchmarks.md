# Benchmarks

> Methodology and the (currently empty) result table. Numbers land in
> v0.6 when the harness is pinned and published; these are the *goals*.

## Hypothesis

For any workload whose cost is dominated by syscalls, **sprout ≈ native**
and **severely faster than proot**, because sprout's path translation
happens in-process with zero context switches.

## Harness

`bench/` in the repo produces a comparison table for:

| Workload | What it exercises |
|---|---|
| `lstat` storm | pure translation cost (worst case for sprout) |
| `node --version` | cold-start ELF load + lib resolution |
| `python3 -c pass` | same, different libc init path |
| `git clone --depth 1` | fork/exec chains + large `read()`/`write()` |
| `curl` to localhost HTTP | socket syscalls (untranslated, serves as control) |

Everything runs on the same device, cold-page-cache, CPU locked to the
fastest cluster (`taskset -c <big>`), 5 runs, median reported.

## Success criteria

| Metric | proot | sprout | Pass |
|---|---|---|---|
| `lstat` storm | baseline | **≥ 3× proot** | TBD |
| `node --version` | baseline | **≥ 1.5× proot** | TBD |
| `python3 -c pass` | baseline | **≥ 1.5× proot** | TBD |
| `git clone` | baseline | **≥ 1.2× proot** | TBD |
| `curl localhost` | baseline | ≈ within 5% | control |

Results must be reproducible: `bench/run.sh` pins every step, dumps the
JSON, and the table above is rendered from that JSON at docs-build time.
