#!/bin/bash
# stress_test.sh - run from the repo root (same dir as Makefile).
# Usage: ./stress_test.sh [run_count] [induce_load]
#   run_count:   number of iterations per phase (default 400)
#   induce_load: "load" to also run a second phase with background
#                CPU load, anything else/omitted to skip it
set -u
N="${1:-400}"
DO_LOAD="${2:-load}"
BUILD_DIR="$(mktemp -d)"
BIN="${BUILD_DIR}/two_board_cbba_harness"
LOGDIR="${BUILD_DIR}/logs"
mkdir -p "$LOGDIR"
echo "[stress] build dir: ${BUILD_DIR}"

echo "[stress] building with -DCBBA_DEBUG_TIMING ..."
gcc -std=gnu11 -Wall -Wextra -O1 -DCBBA_DEBUG_TIMING \
    -Icore -Ilaptop -o "$BIN" \
    laptop/tools/two_board_cbba_harness.c laptop/debug.c laptop/snapshot.c \
    core/*.c -lm
if [ $? -ne 0 ]; then echo "[stress] BUILD FAILED"; exit 1; fi

run_phase() {
    local label="$1" outdir="$2" n="$3"
    mkdir -p "$outdir"
    local pass=0 fail=0
    for i in $(seq 1 "$n"); do
        "$BIN" > "${outdir}/run_${i}.log" 2>&1
        if [ $? -ne 0 ]; then
            fail=$((fail+1))
            echo "  [${label}] run ${i} FAILED"
        else
            pass=$((pass+1))
        fi
        if [ $((i % 50)) -eq 0 ]; then echo "  [${label}] ${i}/${n} done (pass=${pass} fail=${fail})"; fi
    done
    echo "[stress] ${label}: ${pass} passed, ${fail} failed out of ${n}"
}

echo "[stress] === Phase 1: no induced load, ${N} runs ==="
run_phase "noload" "${LOGDIR}/noload" "$N"

if [ "$DO_LOAD" = "load" ]; then
    echo "[stress] === Phase 2: induced CPU load, ${N} runs ==="
    NPROC=$(nproc)
    LOADERS=()
    for j in $(seq 1 "$NPROC"); do
        yes > /dev/null &
        LOADERS+=($!)
    done
    echo "[stress] started ${#LOADERS[@]} 'yes' loaders (nproc=${NPROC})"
    run_phase "load" "${LOGDIR}/load" "$N"
    for pid in "${LOADERS[@]}"; do kill "$pid" 2>/dev/null; done
fi

echo ""
echo "[stress] === [cbba] timeout log line breakdown ==="
for phase_dir in "${LOGDIR}/noload" "${LOGDIR}/load"; do
    [ -d "$phase_dir" ] || continue
    echo "--- $(basename "$phase_dir") ---"
    grep -h "\[cbba\] round timeout expired" "${phase_dir}"/*.log 2>/dev/null | \
        awk -F'waited=' '{split($2,a,"ms"); w=a[1]+0; if (w > 100000) huge++; else plausible++} END {print "  genuine-looking (<=100000ms): " plausible+0; print "  garbage/huge (>100000ms): " huge+0}'
    grep -lc "\[cbba\] round timeout expired" "${phase_dir}"/*.log 2>/dev/null | wc -l | xargs -I{} echo "  files containing the line: {}"
done

echo ""
echo "[stress] Everything is under: ${BUILD_DIR}"
echo "[stress] To package for sending back:"
echo "  tar czf stress_results.tar.gz -C ${BUILD_DIR} logs"
echo "  (plus paste the summary lines printed above)"