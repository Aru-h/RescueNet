#!/bin/bash
# Diagnostic (not part of any check): where are the incidents lost after
# perfect fault detection?  Builds no-AI and "oracle" binaries in
# build/headroom/ (the oracle overwrites the predictor scores with the
# simulator's ground truth; it lives only in a scratch copy of core/, the
# repo sources are never modified), runs seeds 1001-1020 x 3000 ticks for
# profiles none/mobility/communication/resource/random, then prints the tables.
# usage (repo root): bash laptop/tools/ai_headroom_investigation.sh
set -e
R=$(pwd); W=$R/build/headroom; mkdir -p $W/bin $W/oc
F="-std=gnu11 -O0 -w -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1"
LAP="laptop/main.c laptop/debug.c laptop/snapshot.c laptop/mission_feed.c laptop/mission_mirror.c"
make -s tron
cp tron $W/bin/noai
rm -rf $W/oc/core $W/oc/laptop; cp -r core laptop $W/oc/
sed -i 's|scores = fault_predictor_run(feature_vector);|&\n        for(int k = 0; k < NUM_OPERATIONAL_FAULT_CLASSES; k++) scores.score[k] = team->fault_state.active[k] ? 1.0f : 0.0f;|' $W/oc/core/recovery_manager.c
grep -q "team->fault_state.active\[k\] ? 1.0f" $W/oc/core/recovery_manager.c
(cd $W/oc && cc $F -Icore -Ilaptop -o ../bin/oracle core/*.c $LAP -lm)
cat > $W/run1.sh <<EOS
#!/bin/bash
d=$W/runs/\$1/\$4; mkdir -p \$d; cd \$d
$W/bin/\$2 \$4 1 3000 --fault-profile=\$3 --diag > out.log 2> diag.log; echo done > ok
EOS
chmod +x $W/run1.sh
(for s in $(seq 1001 1020); do echo "free noai none $s"; echo "noai noai random $s"; echo "oracle oracle random $s"
  for p in mobility communication resource; do echo "noai_$p noai $p $s"; echo "oracle_$p oracle $p $s"; done; done) | xargs -P"$(nproc)" -L1 $W/run1.sh
python3 laptop/tools/ai_headroom_analysis.py $W
