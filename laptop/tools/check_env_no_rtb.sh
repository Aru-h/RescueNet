#!/bin/bash
# Step 3.2 fix 2: under the causal profile an environment detection never drives
# a team-level reaction. A PERFECT environment detector (class score = the
# environment label, threshold 0.5; built like laptop/tools/causal_actions_oracle.sh)
# must therefore change nothing with --causal-actions=off: the per-team
# throughput diag of the oracle run equals the shipped-thresholds run exactly.
# As a control, the same oracle with --causal-fixes=off must differ (it sends
# flagged teams home), so a green result cannot mean "the detector never fired".
# usage (repo root): bash laptop/tools/check_env_no_rtb.sh [seed ...]
set -e
SEEDS=${*:-1001 1002 1003}
R=$(pwd); W=$R/build/causal-env-no-rtb; rm -rf "$W"; mkdir -p "$W/bin" "$W/oc"
F="-std=gnu11 -O0 -w -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1"
LAP="laptop/main.c laptop/debug.c laptop/snapshot.c laptop/mission_feed.c laptop/mission_mirror.c"
SHIP="0.573f,0.965f,0.705f"
cc $F -Icore -Ilaptop "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$SHIP,1.01f,1.01f}" -o "$W/bin/ship" core/*.c $LAP -lm
cp -r core laptop "$W/oc/"
sed -i "s|scores = fault_predictor_run(feature_vector);|&\n       scores.score[3] = fault_injector_causal_env_label(team->team_id) ? 1.0f : 0.0f;|" "$W/oc/core/recovery_manager.c"
sed -i '1i unsigned char fault_injector_causal_env_label(unsigned char team_id);' "$W/oc/core/recovery_manager.c"
grep -q "fault_injector_causal_env_label(team->team_id)" "$W/oc/core/recovery_manager.c"
(cd "$W/oc" && cc $F -Icore -Ilaptop "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$SHIP,0.5f,1.01f}" -o "$W/bin/oracle" core/*.c $LAP -lm)
run() { # binary tag seed extra...
  local b=$1 tag=$2 s=$3; shift 3
  mkdir -p "$W/$tag/$s"; (cd "$W/$tag/$s" && "$W/bin/$b" $s 1 1500 --fault-profile=causal --fault-stress=7 --causal-actions=off "$@" --diag 2>&1 >/dev/null | grep '^\[DIAG THROUGHPUT\]' > tp.txt; rm -f *.csv)
}
for s in $SEEDS; do
  run ship ship $s; run oracle oracle $s; run oracle oracle_old $s --causal-fixes=off
  cmp -s "$W/ship/$s/tp.txt" "$W/oracle/$s/tp.txt" || { echo "FAIL: seed $s: a perfect environment detector changed the run with fixes on"; exit 1; }
  [ -s "$W/ship/$s/tp.txt" ] || { echo "FAIL: seed $s: no throughput diag"; exit 1; }
  if cmp -s "$W/ship/$s/tp.txt" "$W/oracle_old/$s/tp.txt"; then echo "FAIL: seed $s: control (--causal-fixes=off) identical, detector never acted"; exit 1; fi
  echo "seed $s: oracle == shipped with fixes on; differs with --causal-fixes=off (control)"
done
echo "check-causal-env-no-rtb: a perfect environment detector changes nothing (actions off) over: $SEEDS"
