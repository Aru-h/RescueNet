#!/bin/bash
# Step 4 final evaluation harness (model-agnostic).  One command runs the whole evaluation of the v1 causal world:
#
#   laptop/tools/final_eval.sh                       # shipped model, writes build/final_eval/shipped/
#   laptop/tools/final_eval.sh --model-header my_weights.h [--thresholds 0.57,0.96,0.70] [--calib-header my_cal.h]
#   laptop/tools/final_eval.sh calibrate --model-header my_weights.h     # refit the planner tables for a model
#   laptop/tools/final_eval.sh report [--tag T]                          # re-analyse an existing sweep
#
# Systems (rows) per stress level, on development seeds 1001-1020 and held-out seeds 3001-3020:
#   F   fixed gates, no AI              P   planner (watchdog detector), no AI
#   PD  planner + detector oracle (the headroom ceiling)
#   FA  fixed gates + AI                PA  planner + AI (planner reads the AI's scores)
# The AI rows use the shipped model unless --model-header is given; they are labelled accordingly in the report.
#
# Nothing in the repository is modified: both binaries are built from the repository sources into $OUT/bin, with the
# candidate weights header (and calibration header) force-included with -include, exactly as laptop/tools/evaluate.py does
# for its --model-header.  A header must define the same symbols as core/fault_predictor_weights.h (75 inputs, 128-64-5).
#
# Options
#   --model-header PATH   weights header for the AI rows (default: core/fault_predictor_weights.h, the shipped model)
#   --thresholds a,b,c    per-class decision thresholds (mobility, communication, resource; environment and mission stay
#                         off); default: the shipped FAULT_PREDICTOR_CLASS_THRESHOLDS in core/fault_predictor.h
#   --calib-header PATH   planner calibration tables fitted for that model (output of the `calibrate` command); applies
#                         to the AI build only, so the no-AI planner rows never change with the model
#   --tag NAME            output sub-directory (default: shipped, or custom when a model header is given)
#   --out DIR             output directory (default build/final_eval/<tag>)
#   --stress "1 2 3.5 7"  stress levels (default as shown)
#   --extra "ARG ..."     extra simulator arguments for every run (e.g. a --causal-fixes list); default none = the v1 world
#   --no-reference        skip the no-fault reference runs (fixed gates, --fault-profile=none)
#   --figures DIR         also write PNG figures there (default: $OUT/figures)
#   --ref-3.6 DIR         compare stress 2 and 7 rows with Step 3.6 result files (D_/H_ f p pdet f_ai p_ai) per seed
#   --jobs N              worker processes per run (default: all cores)
#   --dev-seeds A-B, --held-seeds A-B   seed ranges (the calibrate command refuses seeds outside 1001-1020)
#   --sets D|H|DH        which seed sets to run (default DH). `--dev-seeds 5301-5340 --sets D` evaluates on validation seeds only,
#                         e.g. to pick thresholds without touching the held-out seeds
#
# `calibrate` refits core/planner_calibration.h-style tables for the candidate model on DEVELOPMENT seeds only and writes
# $OUT/planner_calibration.h (it never touches core/).  Run it, then pass that file as --calib-header.  It is documented
# here and in docs/V1_TRAINING_HANDOFF.md; it is not run for any new model by anyone but the model's owner.
set -euo pipefail

R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$R"
CMD=run
if [[ $# -gt 0 && "$1" =~ ^(run|calibrate|report)$ ]]; then CMD=$1; shift; fi

MODEL_HEADER=""; THRESHOLDS=""; CALIB_HEADER=""; TAG=""; OUT=""; STRESS="1 2 3.5 7"; EXTRA=""
REFERENCE=1; FIGDIR=""; REF36=""; JOBS="$(getconf _NPROCESSORS_ONLN)"; CALIB_STRESS="7"
SETS="DH"
DEV_SEEDS="1001-1020"; HELD_SEEDS="3001-3020"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --model-header) MODEL_HEADER="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2;;
    --thresholds) THRESHOLDS="$2"; shift 2;;
    --calib-header) CALIB_HEADER="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2;;
    --tag) TAG="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --stress) STRESS="$2"; shift 2;;
    --calib-stress) CALIB_STRESS="$2"; shift 2;;
    --extra) EXTRA="$2"; shift 2;;
    --no-reference) REFERENCE=0; shift;;
    --figures) FIGDIR="$2"; shift 2;;
    --ref-3.6) REF36="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --sets) SETS="$2"; shift 2;;
    --dev-seeds) DEV_SEEDS="$2"; shift 2;;
    --held-seeds) HELD_SEEDS="$2"; shift 2;;
    -h|--help) sed -n '2,45p' "$0"; exit 0;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
[[ -z "$TAG" ]] && { [[ -n "$MODEL_HEADER" ]] && TAG=custom || TAG=shipped; }
[[ -z "$OUT" ]] && OUT="build/final_eval/$TAG"
mkdir -p "$OUT/bin"
OUT="$(cd "$OUT" && pwd)"

CF="-std=gnu11 -O0 -w -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -Icore -Ilaptop"
SRC="core/*.c laptop/main.c laptop/debug.c laptop/snapshot.c laptop/mission_feed.c laptop/mission_mirror.c"

thr_define() {   # a,b,c -> -D...={a,b,c,off,off}
  local t="$1"; IFS=',' read -ra v <<< "$t"
  [[ ${#v[@]} -eq 3 ]] && t="$t,1.01f,1.01f"
  [[ ${#v[@]} -eq 3 || ${#v[@]} -eq 5 ]] || { echo "--thresholds needs 3 or 5 comma-separated values" >&2; exit 2; }
  echo "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$t}"
}

build() {
  # shellcheck disable=SC2086
  cc $CF -o "$OUT/bin/noai" $SRC -lm
  local ai_flags="-DFAULT_MODEL_LOADED=1"
  [[ -n "$MODEL_HEADER" ]] && ai_flags="$ai_flags -include $MODEL_HEADER"
  [[ -n "$CALIB_HEADER" ]] && ai_flags="$ai_flags -include $CALIB_HEADER"
  # shellcheck disable=SC2086
  if [[ -n "$THRESHOLDS" ]]; then cc $CF $ai_flags "$(thr_define "$THRESHOLDS")" -o "$OUT/bin/ai" $SRC -lm
  else cc $CF $ai_flags -o "$OUT/bin/ai" $SRC -lm; fi
  {
    echo "date: $(date -u +%FT%TZ)"
    echo "commit: $(git rev-parse HEAD) $(git diff --quiet && git diff --cached --quiet && echo clean || echo DIRTY)"
    echo "model_header: ${MODEL_HEADER:-core/fault_predictor_weights.h (shipped)}"
    [[ -n "$MODEL_HEADER" ]] && echo "model_header_md5: $(md5sum "$MODEL_HEADER" | cut -c1-32)"
    echo "thresholds: ${THRESHOLDS:-shipped}"
    echo "calib_header: ${CALIB_HEADER:-core/planner_calibration.h (shipped)}"
    echo "extra_args: ${EXTRA:-none (v1 world)}"
  } > "$OUT/provenance.txt"
  if [[ -n "$MODEL_HEADER" ]]; then
    echo "candidate model $(basename "$MODEL_HEADER") (md5 $(md5sum "$MODEL_HEADER" | cut -c1-8))" > "$OUT/model_label.txt"
  else
    echo "shipped model (trained on the old fault model - context only)" > "$OUT/model_label.txt"
  fi
}

# name | binary | simulator args
systems() {
  echo "F|noai|"
  echo "P|noai|--recovery=utility --planner-p=watchdog"
  echo "PD|noai|--recovery=utility --planner-p=oracle"
  echo "FA|ai|"
  echo "PA|ai|--recovery=utility --planner-p=ai"
}

run_one() {  # stress name binary sets args profile
  local st=$1 name=$2 bin=$3 set=$4 args=$5 profile=$6 seeds out ea=() a
  [[ $set == D ]] && seeds=$DEV_SEEDS || seeds=$HELD_SEEDS
  mkdir -p "$OUT/s$st"; out="$OUT/s$st/${set}_${name}.json"
  [[ -f "$out" ]] && return 0
  for a in $args $EXTRA; do ea+=("--extra-arg=$a"); done
  echo "== stress $st $set $name $(date +%T)"
  local sa=(); [[ $profile == causal ]] && sa=(--stress "$st")
  python3 laptop/tools/fault_exposure.py --tron "$OUT/bin/$bin" --profile "$profile" "${sa[@]}" --seeds "$seeds" \
    --jobs "$JOBS" "${ea[@]}" --json "$out" | grep -E "incidents resolved|victims lost" | cut -c1-120
}

sweep() {
  local st line name bin args set
  for st in $STRESS; do
    echo "### stress $st"
    while IFS='|' read -r name bin args; do
      for set in $(echo "$SETS" | grep -o .); do run_one "$st" "$name" "$bin" "$set" "$args" causal; done
    done < <(systems)
  done
  if [[ $REFERENCE -eq 1 ]]; then
    echo "### reference: no faults (fixed gates)"
    for set in $(echo "$SETS" | grep -o .); do run_one 0 F noai "$set" "" none; done
  fi
}

report() {
  local fig=${FIGDIR:-$OUT/figures}
  python3 laptop/tools/final_eval_report.py "$OUT" --stress "$STRESS" --figures "$fig" ${REF36:+--ref-3.6 "$REF36"}
}

calibrate() {
  # Refit the planner calibration tables for the candidate AI model, on development seeds only.
  [[ "${DEV_SEEDS%-*}" -ge 1001 && "${DEV_SEEDS#*-}" -le 1020 ]] || { echo "calibrate uses development seeds 1001-1020 only" >&2; exit 2; }
  mkdir -p "$OUT/calib"
  build
  local st seed d
  for st in $CALIB_STRESS; do
    for seed in $(seq "${DEV_SEEDS%-*}" "${DEV_SEEDS#*-}"); do
      for b in ai noai; do
        d="$OUT/calib/$b/s${st}_$seed"; mkdir -p "$d"
        [[ -f "$d/ok" ]] && continue
        echo "$b $st $seed"
      done
    done
  done | xargs -P"$JOBS" -L1 bash -c '
    b=$0; st=$1; seed=$2; d="'"$OUT"'/calib/$b/s${st}_$seed"
    cd "$d" && "'"$OUT"'/bin/$b" $seed 1 3000 --fault-profile=causal --fault-stress=$st --recovery=utility --planner=none --planner-calib --diag '"$EXTRA"' > out.log 2> diag.log
    rm -f tron_snapshot_dataset.csv tron_causal_truth.csv; echo done > ok'
  cp core/planner_calibration.h "$OUT/planner_calibration.h"
  python3 laptop/tools/planner_calibration_fit.py fit --ai "$OUT/calib/ai" --off "$OUT/calib/noai" --header "$OUT/planner_calibration.h"
  echo "wrote $OUT/planner_calibration.h; evaluate with:  $0 --model-header ${MODEL_HEADER:-<header>} --calib-header $OUT/planner_calibration.h${THRESHOLDS:+ --thresholds $THRESHOLDS}"
}

case "$CMD" in
  run) build; sweep; report;;
  report) report;;
  calibrate) calibrate;;
esac
