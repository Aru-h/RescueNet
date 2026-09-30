#!/bin/bash
# Diagnostic (not part of any check): is switching the AI's environment and
# mission classes off a model problem or a recovery-layer problem?
# Builds, in build/env-mission-oracle/ (repo sources are never modified):
#   noai        ./tron (rules + consensus, AI off)
#   ai_ship     AI on, shipped thresholds
#   ai_em       AI on, only environment + mission acting (0.706 / 0.731)
#   oracle_3    perfect detector (ground truth), shipped thresholds
#   oracle_em   perfect detector, only environment + mission acting
#   oracle_env  perfect detector, only environment acting
#   oracle_mis  perfect detector, only mission acting
# and runs each on seeds 1001-1020 x 3000 ticks, profile random, then prints
# mean incidents resolved and the paired difference vs noai (bootstrap 95% CI).
# usage (repo root): bash laptop/tools/ai_env_mission_oracle.sh
set -e
R=$(pwd); W=$R/build/env-mission-oracle; rm -rf "$W"; mkdir -p "$W/bin" "$W/oc"
F="-std=gnu11 -O0 -w -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1"
LAP="laptop/main.c laptop/debug.c laptop/snapshot.c laptop/mission_feed.c laptop/mission_mirror.c"
OFF=1.01f
make -s tron
cp tron "$W/bin/noai"
cc $F -Icore -Ilaptop -o "$W/bin/ai_ship" core/*.c $LAP -lm
cc $F -Icore -Ilaptop -o "$W/bin/ai_em" "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$OFF,$OFF,$OFF,0.706f,0.731f}" core/*.c $LAP -lm
cp -r core laptop "$W/oc/"
sed -i 's|scores = fault_predictor_run(feature_vector);|&\n        for(int k = 0; k < NUM_OPERATIONAL_FAULT_CLASSES; k++) scores.score[k] = team->fault_state.active[k] ? 1.0f : 0.0f;|' "$W/oc/core/recovery_manager.c"
grep -q "team->fault_state.active\[k\] ? 1.0f" "$W/oc/core/recovery_manager.c"
(cd "$W/oc" && \
 cc $F -Icore -Ilaptop -o "$W/bin/oracle_3" core/*.c $LAP -lm && \
 cc $F -Icore -Ilaptop -o "$W/bin/oracle_em"  "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$OFF,$OFF,$OFF,0.5f,0.5f}" core/*.c $LAP -lm && \
 cc $F -Icore -Ilaptop -o "$W/bin/oracle_env" "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$OFF,$OFF,$OFF,0.5f,$OFF}" core/*.c $LAP -lm && \
 cc $F -Icore -Ilaptop -o "$W/bin/oracle_mis" "-DFAULT_PREDICTOR_CLASS_THRESHOLDS={$OFF,$OFF,$OFF,$OFF,0.5f}" core/*.c $LAP -lm)
cat > "$W/run1.sh" <<EOS
#!/bin/bash
d=$W/runs/\$1/\$2; mkdir -p \$d; cd \$d
$W/bin/\$1 \$2 1 3000 --fault-profile=random --diag > out.log 2> diag.log; rm -f tron_snapshot_dataset.csv; echo done > ok
EOS
chmod +x "$W/run1.sh"
(for s in $(seq 1001 1020); do for c in noai ai_ship ai_em oracle_3 oracle_em oracle_env oracle_mis; do echo "$c $s"; done; done) \
  | xargs -P"$(getconf _NPROCESSORS_ONLN)" -L1 "$W/run1.sh"
python3 - "$W/runs" <<'PY'
import glob, os, re, sys, random
W = sys.argv[1]
def inc(d): return sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)", open(d + "/diag.log").read()))
def runs(c): return {int(os.path.basename(d)): inc(d) for d in glob.glob(f"{W}/{c}/*") if os.path.exists(d + "/ok")}
def ci(v, n=10000):
    random.seed(0); m = sorted(sum(random.choice(v) for _ in v) / len(v) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n)]
base = runs("noai")
print(f"{'condition':12s} {'n':>3s} {'mean':>7s} {'vs noai':>8s}  95% CI")
for c in ["noai", "ai_ship", "oracle_3", "ai_em", "oracle_em", "oracle_env", "oracle_mis"]:
    r = runs(c); s = sorted(set(r) & set(base)); d = [r[k] - base[k] for k in s]
    lo, hi = ci(d) if c != "noai" else (0.0, 0.0)
    print(f"{c:12s} {len(s):3d} {sum(r[k] for k in s)/len(s):7.2f} {sum(d)/len(d):+8.2f}  [{lo:+.2f}, {hi:+.2f}]")
PY
