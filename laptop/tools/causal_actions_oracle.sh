#!/bin/bash
# AI_EVALUATION_BRIEF C13 re-run under the causal profile with the Step 3
# recovery actions (core/causal_actions.c). Is it worth enabling the
# environment and mission classes once they have actions of their own?
#
# Builds, in build/causal-actions-oracle/ (repo sources are never modified):
#   ai_ship    real AI, shipped thresholds (environment + mission off)
#   oracle_env real AI for mobility/communication/resource + a PERFECT
#              environment detector (class score = ground truth, threshold 0.5)
#   oracle_mis same with a perfect mission detector
#   oracle_em  both
# and runs each with the new actions on and off (--causal-actions), seeds
# 1001-1020 x 3000 ticks, --fault-profile=causal at $STRESS, then prints mean
# incidents resolved, victims lost, and the paired difference vs
# "ai_ship, actions on" (bootstrap 95% CI, 10000 resamples, seed 0).
#
# usage (repo root): STRESS=7 SEED_FROM=1001 SEED_COUNT=20 EXTRA="--causal-roads=off --causal-env-duration=60-150" bash laptop/tools/causal_actions_oracle.sh
# (consequential roads are on by default under causal; EXTRA can pass any causal flag)
set -e
STRESS=${STRESS:-7}
SEED_FROM=${SEED_FROM:-1001}
SEED_COUNT=${SEED_COUNT:-20}
EXTRA=${EXTRA:-}
R=$(pwd); W=$R/build/causal-actions-oracle; rm -rf "$W"; mkdir -p "$W/bin" "$W/oc"
F="-std=gnu11 -O0 -w -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1"
LAP="laptop/main.c laptop/debug.c laptop/snapshot.c laptop/mission_feed.c laptop/mission_mirror.c"
OFF=1.01f
SHIP="0.573f,0.965f,0.705f"
cc $F -Icore -Ilaptop -o "$W/bin/ai_ship" core/*.c $LAP -lm
cp -r core laptop "$W/oc/"
# Perfect detector for the environment (3) and/or mission (4) class only.
for v in env mis em; do
  case $v in env) K="3";; mis) K="4";; em) K="3 4";; esac
  rm -rf "$W/oc_$v"; mkdir -p "$W/oc_$v"; cp -r "$W/oc/core" "$W/oc/laptop" "$W/oc_$v/"
  PATCH=""
  for k in $K; do
    # Environment (3): the label (fault_injector_causal_env_label: "the route touches a damaged cell"
    # under the Step 3.2 label fix, the per-team fault flag with --causal-fixes=off). Others: the fault flag.
    if [ "$k" = 3 ]; then PATCH="$PATCH scores.score[3] = fault_injector_causal_env_label(team->team_id) ? 1.0f : 0.0f;"
    else PATCH="$PATCH scores.score[$k] = team->fault_state.active[$k] ? 1.0f : 0.0f;"; fi
  done
  sed -i "s|scores = fault_predictor_run(feature_vector);|&\n       $PATCH|" "$W/oc_$v/core/recovery_manager.c"
  sed -i '1i unsigned char fault_injector_causal_env_label(unsigned char team_id);' "$W/oc_$v/core/recovery_manager.c"
  grep -q "scores.score\[" "$W/oc_$v/core/recovery_manager.c"
  grep -q "fault_injector_causal_env_label(team->team_id)\|team->fault_state.active\[" "$W/oc_$v/core/recovery_manager.c"
  case $v in env) T="$SHIP,0.5f,$OFF";; mis) T="$SHIP,$OFF,0.5f";; em) T="$SHIP,0.5f,0.5f";; esac
  (cd "$W/oc_$v" && cc $F -Icore -Ilaptop "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$T}" -o "$W/bin/oracle_$v" core/*.c $LAP -lm)
done
cat > "$W/run1.sh" <<EOS
#!/bin/bash
# args: binary actions(on|off) seed
d=$W/runs/\$1_\$2/\$3; mkdir -p \$d; cd \$d
$W/bin/\$1 \$3 1 3000 --fault-profile=causal --fault-stress=$STRESS --causal-actions=\$2 $EXTRA --diag > out.log 2> diag.log; rm -f tron_snapshot_dataset.csv tron_causal_truth.csv; echo done > ok
EOS
chmod +x "$W/run1.sh"
(for s in $(seq $SEED_FROM $((SEED_FROM + SEED_COUNT - 1))); do for c in ai_ship oracle_env oracle_mis oracle_em; do for a in on off; do echo "$c $a $s"; done; done; done) \
  | xargs -P"$(getconf _NPROCESSORS_ONLN)" -L1 "$W/run1.sh"
python3 - "$W/runs" <<'PY'
import glob, os, re, sys, random
W = sys.argv[1]
def inc(d): return sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)", open(d + "/diag.log").read()))
def lost(d):
    m = re.search(r"victims_lost=(\d+)", open(d + "/diag.log").read()); return int(m.group(1)) if m else 0
def runs(c, f): return {int(os.path.basename(d)): f(d) for d in glob.glob(f"{W}/{c}/*") if os.path.exists(d + "/ok")}
def ci(v, n=10000):
    random.seed(0); m = sorted(sum(random.choice(v) for _ in v) / len(v) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n)]
base_i = runs("ai_ship_on", inc); base_l = runs("ai_ship_on", lost)
print(f"{'condition':22s} {'n':>3s} {'incidents':>9s} {'vs ai_ship/on':>14s}  95% CI              {'victims lost':>12s} {'vs ai_ship/on':>14s}  95% CI")
for c in ["ai_ship_on", "ai_ship_off", "oracle_env_on", "oracle_env_off", "oracle_mis_on", "oracle_mis_off", "oracle_em_on", "oracle_em_off"]:
    ri, rl = runs(c, inc), runs(c, lost)
    s = sorted(set(ri) & set(base_i))
    di = [ri[k] - base_i[k] for k in s]; dl = [rl[k] - base_l[k] for k in s]
    li, hi = ci(di) if c != "ai_ship_on" else (0.0, 0.0)
    ll, hl = ci(dl) if c != "ai_ship_on" else (0.0, 0.0)
    print(f"{c:22s} {len(s):3d} {sum(ri[k] for k in s)/len(s):9.2f} {sum(di)/len(di):+14.2f}  [{li:+.2f}, {hi:+.2f}]   {sum(rl[k] for k in s)/len(s):12.2f} {sum(dl)/len(dl):+14.2f}  [{ll:+.2f}, {hl:+.2f}]")
PY
