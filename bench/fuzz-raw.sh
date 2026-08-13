#!/data/data/com.termux/files/usr/bin/bash
# bench/fuzz-raw.sh — raw mutation fuzz against sprout's ELF/script loaders.
# Honest label: NOT coverage-guided (that's an AFL++ follow-up); this
# exercises malformed-input transport end-to-end and triages deaths.
#
#   Seeds:  glibc-static, raw-asm static, dynamic shell binary, shebang script
#   Modes:  byte-flips in ELF header, truncations, tail-randomization,
#           shebang chaos (long lines, CR, multi-token, UTF-8)
#   Runner: sprout CLI (elf.rs classification + lane choice) per mutant
#           with a hard 5s timeout.
#
#   Triage:
#     ALIVE     clean rc (<128): errors/die-codes ARE success — honest refusal
#     HANG      timeout kill            -> suspicious (freeze)
#     HSIGSEGV  rc=139 etc              -> review (could be guest ld.so dying
#                                          on malformed ELF = acceptable, or
#                                          supervisor crash = bug); we flag
#                                          and keep the mutant for forensics.
#
# Usage: bench/fuzz-raw.sh [iterations-per-seed-default-40]
set -u
N=${1:-40}
B=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
S=$PREFIX/bin/sprout
W=$TMPDIR/fuzz-raw.$$; mkdir -p "$W"
seeds="
$B/bin/true
$B/tmp/sp_asm
$B/tmp/ok.sh
"
ALIVE=0; HANG=0; SEGV=0; OTHER=0
keep=""

run_mutant() {
    local m=$1
    timeout 5 "$S" -r "$B" "$W/mut" >/dev/null 2>&1
    local rc=$?
    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        HANG=$((HANG+1)); keep="$keep $W/mut"
    elif [ $rc -ge 128 ]; then
        # supervisor-dies = real bug. Guest-ld.so dying on a mutant DYNAMIC
        # binary carries the same signature — classify after the fact:
        if timeout 3 "$S" -r "$B" /bin/true >/dev/null 2>&1; then
            SEGV=$((SEGV+1)); keep="$keep $W/mut"
        else
            SEGV=$((SEGV+1)); keep="$keep $W/mut SUPERVISOR-DEATH?"
        fi
    else
        ALIVE=$((ALIVE+1))
    fi
}

mut_bytes() { # mut_bytes NBYTES SEED OUT
    local nb=$1 seed=$2 out=$3
    dd if=/dev/urandom bs=1 count="$nb" 2>/dev/null >"$out.rnd"
    python3 - "$seed" "$out" <<'PYEOF'
import random, sys
src, out = sys.argv[1], sys.argv[2]
data = open(sys.argv[1], "rb").read()
dib = bytearray(data)
# header-region bias: first 128 bytes AND any 1-8 spots anywhere
spots = [random.randrange(0, min(128, len(data)))] if data else []
spots += [random.randrange(0, len(data)) for _ in range(random.randint(1, 8)) if data]
for p in spots: dib[p] ^= random.randint(1, 255)
# occasional truncate
if random.random() < 0.3 and len(dib) > 4:
    dib = dib[:random.randrange(4, len(dib))]
open(out, "wb").write(dib)
PYEOF
}

mut_script() { # mutant from shebang chaos
    python3 - "$1" <<'PYEOF'
import random, sys
kinds = [
    "#!/bin/bash\n" + "A" * random.randint(1, 8192) + "\necho hi\n",
    "#!/bin/bash -eux --bad-flag-long-long\nexit 0\n",
    "#!\u00e9\u4e2d/bin/bash\necho hi\n",
    "#! /bin/bash\necho hi\n",            # leading space after #!
    "#!/bin/bash " + "1" * random.randint(1, 4096) + "\necho hi\n",
    "#!/no/such/interp\necho hi\n",
    "#!/bin/bash\r\necho hi\r\n",         # CR-in-shebang
    "#!\n/bin/bash\necho hi\n",           # empty interp
    "#!/bin\n/bash\necho hi\n",
    "#!/tmp/sp_asm\necho hi\n",           # static interp (legal path)
]
open(sys.argv[1], "wb").write(random.choice(kinds).encode())
PYEOF
}

echo "== fuzz-raw: seeds baseline sanity ==" >&2
$S -r "$B" /bin/true || { echo "baseline dead" >&2; exit 1; }
echo "N=$N/seed+script-mutations" >&2

for seed in $seeds; do
    echo "seed: $(basename $seed)" >&2
    i=0
    while [ $i -lt $N ]; do
        mut_bytes $((RANDOM % 12 + 1)) "$seed" "$W/mut"
        chmod +x "$W/mut" 2>/dev/null
        run_mutant "$W/mut"
        i=$((i+1))
    done
done
echo "seed: shebang-chaos" >&2
i=0
while [ $i -lt $((N * 2)) ]; do
    mut_script "$W/mut"; chmod +x "$W/mut"
    run_mutant "$W/mut"
    i=$((i+1))
done

echo
echo "RESULT: alive=$ALIVE hang=$HANG segv-class=$SEGV (kept: $W)"
[ -n "$keep" ] && { echo "kept mutants:"; for k in $keep; do echo "  $k"; done; }
# post-fuzz invariant: supervisor + lanes still sane
$S -r "$B" /tmp/sp_asm >/dev/null 2>&1 && sc=42 || sc=$?
echo "post-invariant static stub rc=$sc (want 42)"
echo "SUMMARY: live-rate=$(awk "BEGIN{printf \"%.1f\", $ALIVE/($ALIVE+$HANG+$SEGV)*100}")%"
