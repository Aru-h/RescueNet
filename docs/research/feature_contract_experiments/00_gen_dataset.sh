#!/usr/bin/env bash
# THROWAWAY. Generates the experiment dataset.
#   40 seeds (1..40) x 3000 ticks, --fault-profile=combined, 4 shards of 10 seeds.
# Uses a scratch COPY of the tree with one extra CSV column (heartbeat_emitted =
# team->last_heartbeat_tick == cycle, the same predicate docs/architecture/CONSENSUS_DESIGN.md
# calls team_radio_emitted()). No tracked file is modified.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
WORK=${TRON_G_WORK:-$PWD/work}
mkdir -p "$WORK"
rm -rf "$WORK/simcopy"; mkdir -p "$WORK/simcopy"
cp -R "$REPO/core" "$REPO/laptop" "$REPO/Makefile" "$WORK/simcopy/"
python3 - "$WORK/simcopy/laptop/snapshot.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = s.replace('"incident_floor,incident_building_id\\n");',
              '"incident_floor,incident_building_id,heartbeat_emitted\\n");')
a = a.replace('%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d\\n"',
              '%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u\\n"')
a = a.replace('        floor,\n        building_id);\n}',
              '        floor,\n        building_id,\n        (unsigned)(team->last_heartbeat_tick == cycle));\n}', 1)
assert a.count('heartbeat_emitted') == 1 and a != s
open(p, 'w').write(a)
PY
(cd "$WORK/simcopy" && make tron >/dev/null)
for s in 0 1 2 3; do
  mkdir -p "$WORK/shard$s"
  (cd "$WORK/shard$s" && "$WORK/simcopy/tron" $((s*10+1)) 10 3000 --fault-profile=combined > run.log 2>&1) &
done
wait
echo "shards done: $WORK/shard*/tron_snapshot_dataset.csv"
