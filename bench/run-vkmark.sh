#!/data/data/com.termux/files/usr/bin/bash
# vkmark matrix — Turnip GPU vs llvmpipe CPU on sprout vs proot.
#
# Bare X :0 (user can watch), uniform canvas, vkmark default present mode
# (mailbox). Override per-cell via env:
#   B=<rootfs>  ICD=<icd.json>  SIZE=1280x720  PM=mailbox
#
# Example:  bench/run-vkmark.sh
#           PM=immediate bench/run-vkmark.sh
set -u
B=${B:-$PREFIX/var/lib/proot-distro/containers/debian/rootfs}
SIZE=${SIZE:-1280x720}
PM=${PM:-}
OUT=${OUT:-$PREFIX/tmp/vkmark-results-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

run() { # $1 label $2 launcher $3 icd
    local label=$1 launcher=$2 icd=$3
    local log="$OUT/$label.log"
    local winsys="--winsys xcb --size $SIZE"
    [ -n "$PM" ] && winsys="$winsys --present-mode $PM"
    echo "== $label ($icd) ==" | tee -a "$OUT/SUMMARY"
    if [ "$launcher" = sprout ]; then
        timeout 300 sprout -r "$B" --shared-tmp /bin/bash -lc \
            "export DISPLAY=:0 VK_ICD_FILENAMES=$icd; vkmark $winsys" >"$log" 2>&1
    else
        timeout 300 proot-distro login debian --shared-tmp -- bash -lc \
            "export DISPLAY=:0 VK_ICD_FILENAMES=$icd; vkmark $winsys" >"$log" 2>&1
    fi
    grep -E "vkmark Score" "$log" | tee -a "$OUT/SUMMARY" || echo "FAIL: no score" | tee -a "$OUT/SUMMARY"
}

run sprout-turnip  sprout /usr/share/vulkan/icd.d/freedreno_icd.aarch64.json
run proot-turnip   proot  /usr/share/vulkan/icd.d/freedreno_icd.aarch64.json
run sprout-lvp     sprout /usr/share/vulkan/icd.d/lvp_icd.json
run proot-lvp      proot  /usr/share/vulkan/icd.d/lvp_icd.json
echo
echo "--- summary ---"
cat "$OUT/SUMMARY"
echo "(logs: $OUT)"
