#!/bin/bash
# check-two-board-cbba-harness.sh
#
# Builds laptop/tools/two_board_cbba_harness.c and runs it repeatedly
# to check for flakiness, per this project's own verification bar
# ("write your own check-*-style verification, run it 20+ times, zero
# flakiness before calling it done" - Track 4 prompt). Default run
# count here is 40, above that 20+ floor, since the harness's own
# history (see two_board_cbba_harness.c's build comment) already
# found real flakiness that only showed up after dozens of runs, not
# a handful.
#
# No Makefile target for this exists yet (this repo's own check-board/
# check-pico targets are the established convention - see Makefile) -
# this script is deliberately standalone so it doesn't need to guess
# at how a future check-two-board-cbba-harness Makefile target should
# be wired in; folding it in as `make check-two-board-cbba-harness`
# later is a small, mechanical change (see check-board: / check-pico:
# in the Makefile for the pattern), not attempted here to avoid
# touching a shared file Track 4 wasn't scoped to change.
#
# Usage:
#   ./check-two-board-cbba-harness.sh [run_count]
#
# Exit code: 0 if every run passed, 1 otherwise. Individual run logs
# are kept under a temp directory printed on failure, for post-mortem.

set -u

RUN_COUNT="${1:-40}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
BINARY="${BUILD_DIR}/two_board_cbba_harness"
LOG_DIR="${BUILD_DIR}/logs"
mkdir -p "${LOG_DIR}"

cleanup() {
    if [ "${KEEP_LOGS:-0}" != "1" ]; then
        rm -rf "${BUILD_DIR}"
    fi
}
trap cleanup EXIT

echo "[check] building two_board_cbba_harness ..."
gcc -std=gnu11 -Wall -Wextra -O1 \
    -I"${REPO_ROOT}/core" -I"${REPO_ROOT}/laptop" \
    -o "${BINARY}" \
    "${REPO_ROOT}/laptop/tools/two_board_cbba_harness.c" \
    "${REPO_ROOT}/laptop/debug.c" \
    "${REPO_ROOT}/laptop/snapshot.c" \
    "${REPO_ROOT}/core/cbba.c" \
    "${REPO_ROOT}/core/comms_transport.c" \
    "${REPO_ROOT}/core/lora_frame.c" \
    "${REPO_ROOT}/core/mission_state.c" \
    "${REPO_ROOT}/core/team.c" \
    "${REPO_ROOT}/core/random.c" \
    "${REPO_ROOT}/core/scoring.c" \
    "${REPO_ROOT}/core/path.c" \
    "${REPO_ROOT}/core/allocator.c" \
    "${REPO_ROOT}/core/mission_state_utils.c" \
    "${REPO_ROOT}/core/incident.c" \
    "${REPO_ROOT}/core/mission_link.c" \
    "${REPO_ROOT}/core/mission_link_handshake.c" \
    "${REPO_ROOT}/core/mission_link_override.c" \
    "${REPO_ROOT}/core/environment.c" \
    "${REPO_ROOT}/core/fault_config.c" \
    "${REPO_ROOT}/core/fault_injector.c" \
    "${REPO_ROOT}/core/grid.c" \
    "${REPO_ROOT}/core/watchdog.c" \
    "${REPO_ROOT}/core/recovery_manager.c" \
    "${REPO_ROOT}/core/search_zones.c" \
    "${REPO_ROOT}/core/mission_execution_twin.c" \
    "${REPO_ROOT}/core/safezone.c" \
    "${REPO_ROOT}/core/test_metrics.c" \
    -lm

if [ $? -ne 0 ]; then
    echo "[check] BUILD FAILED"
    exit 1
fi

echo "[check] running ${RUN_COUNT} iterations ..."
fail_count=0
for i in $(seq 1 "${RUN_COUNT}"); do
    "${BINARY}" > "${LOG_DIR}/run_${i}.log" 2>&1
    rc=$?
    if [ "${rc}" -ne 0 ]; then
        fail_count=$((fail_count + 1))
        echo "[check] run ${i} FAILED (exit ${rc}) - log: ${LOG_DIR}/run_${i}.log"
    fi
done

echo "[check] ${RUN_COUNT} runs complete: $((RUN_COUNT - fail_count)) passed, ${fail_count} failed"

if [ "${fail_count}" -ne 0 ]; then
    echo "[check] FAIL - logs kept at: ${LOG_DIR}"
    KEEP_LOGS=1
    exit 1
fi

echo "[check] PASS - zero flakiness across ${RUN_COUNT} runs"
exit 0
