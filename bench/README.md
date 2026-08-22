# bench — harness inventory

All numbers land under `bench/results/<name>-<ts>/`.

| harness | sections it covers | default workgrain |
|---|---|---|
| **run-all.sh** (master) | smoke gates + CPU/workload + statics + vkmark GPU matrix + machine-info | ONE runner, ONE summary.md |
| run.sh | sprout vs proot-distro, median clock-fonts | MODE=quick(4 cells) \| MODE=full |
| run-statics.sh | static-ELF fast-path (notify vs ptrace vs proot) | 3-iteration medians |
| run-vkmark.sh | GPU matrix: turnip (HW) vs lvp (CPU), sprout vs proot | 4 cells × ~3 scenes |
| run-hyperfine.sh | independent crosscheck (in-guest hyperfine) | spot cells |
| run-alpine.sh | same suite against musl rootfs | mirror of run.sh |
| run-extensive.sh | deeper one-shot sweeps (historical) | mixed |
| flags-matrix.sh | correctness cells (not timing) | 34 rows |
| flags-matrix-extended.sh | parity-cell extension | 73 rows |
| fuzz-raw.sh | raw-arg fuzz battery | 10k iterations |
| soak-long-tail.sh | long-run leak hunt | nightly minutes |

## The comprehensive run (what CI + release tags should run)

```
bench/run-all.sh [rootfs] [n]             # full suite, all sections
SECTIONS="cpu statics" bench/run-all.sh   # subset
VK_PM=immediate bench/run-all.sh          # mailbox→immediate swap of vkmark
```

Result dir layout:

```
bench/results/comprehensive-YYYYMMDD-HHMMSS/
├── machine-info.md     # kernel/SKU/git SHA/artifact md5/versions (this is what makes
│                       #  two runs comparable)
├── summary.md          # smoke gate lines + speedup table + vkmark Score lines, tagged
├── cpu/ , statics/ , vkmark/ , proben/   # raw logs per section
```

Re-audit a claim: `grep '= FAIL' bench/results/comprehensive-*/summary.md` — any FAIL is authoritative and should gate the release.

## Discipline notes

- Iterations: `--iterations`/`ITER` is a MEDIAN argument. Odd number, ≥ 3.
- The vkmark lane REQUIRES a live X display ($DISPLAY + socket). Bare termux-x11
  works; virpipe/virgl aren't scored this way.
- proot is measured via `proot-distro login` (identical env posture, no flags
  the rootfs-side can't see).
- All runners fork + don't trust ambient PID trees: kill stray sprout/super
  procs before timing.
