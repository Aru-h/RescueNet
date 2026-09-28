#!/usr/bin/env bash
# laptop/tools/check-evaluate-determinism.sh
#
# make check-evaluate-determinism: a tiny evaluate.py run (2 seeds x 300
# ticks) executed once with --workers 1 and once with --workers 4, asserting
# byte-identical summary_metrics.csv, paired_diff.csv and report.md. Catches
# any worker-count-dependent nondeterminism in evaluate.py's aggregation or
# bootstrap CIs (see evaluate.py's module docstring for why it should never
# have any) without paying for a full 20-seed/3000-tick run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PYTHON="${PYTHON:-python3}"
OUT_A="build/evaluate-determinism/workers1"
OUT_B="build/evaluate-determinism/workers4"

rm -rf build/evaluate-determinism
mkdir -p "$OUT_A" "$OUT_B"

COMMON_ARGS=(--variants rules,rules+consensus --seeds-start 1001 --seeds-count 2 --ticks 300 --profiles random --no-docs-copy)

"$PYTHON" laptop/tools/evaluate.py "${COMMON_ARGS[@]}" --workers 1 --out-dir "$OUT_A"
"$PYTHON" laptop/tools/evaluate.py "${COMMON_ARGS[@]}" --workers 4 --out-dir "$OUT_B"

status=0
for f in summary_metrics.csv paired_diff.csv report.md; do
	if ! diff -u "$OUT_A/$f" "$OUT_B/$f"; then
		echo "check-evaluate-determinism: $f differs between --workers 1 and --workers 4"
		status=1
	fi
done

if [ "$status" -ne 0 ]; then
	exit 1
fi

echo "check-evaluate-determinism: summary_metrics.csv, paired_diff.csv and report.md are byte-identical regardless of worker count"
