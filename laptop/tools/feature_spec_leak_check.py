#!/usr/bin/env python3
"""
Fast leak guard for feature_spec.json's committed "inputs" list - no CSV, no
simulator run. data_prep.py already asserts this every time it runs (belt);
this is the always-cheap, always-runnable check on just the committed
contract (suspenders) - e.g. after a hand-edit to feature_spec.json that
skipped a full data_prep.py run.

GROUND_TRUTH_COLUMNS/CONSENSUS_OUTPUT_PREFIXES are duplicated from
data_prep.py rather than imported - data_prep.py is a script that runs its
whole pipeline against a real CSV at import time, not an importable module.
Keep the two lists in sync by hand if either changes.
"""
import json
import sys

FAULT_CLASS_NAMES = ["mobility", "communication", "resource", "environment", "mission"]
FAULT_LABEL_COLUMNS = [f"fault_active_{name}" for name in FAULT_CLASS_NAMES]
GROUND_TRUTH_COLUMNS = (
    FAULT_LABEL_COLUMNS
    + [f"fault_severity_{name}" for name in FAULT_CLASS_NAMES]
    + [f"fault_ticks_remaining_{name}" for name in FAULT_CLASS_NAMES]
    + ["fault_active_mask"]
    + ["gt_link_dropped", "gt_loss_model"]
)
CONSENSUS_OUTPUT_PREFIXES = ("consensus_", "peer_", "beacon_emitted")


def main():
    spec_path = sys.argv[1] if len(sys.argv) > 1 else "feature_spec.json"
    with open(spec_path) as f:
        spec = json.load(f)

    inputs = spec["inputs"]
    leaked = set(inputs) & set(GROUND_TRUTH_COLUMNS)
    leaked |= {c for c in inputs if c.startswith("gt_")}
    leaked |= {c for c in inputs if c.startswith(CONSENSUS_OUTPUT_PREFIXES)}

    if leaked:
        print(f"FAIL: {spec_path} inputs include ground-truth/consensus column(s): "
              f"{sorted(leaked)}", file=sys.stderr)
        return 1

    if len(inputs) != len(set(inputs)):
        seen = set()
        dupes = [x for x in inputs if x in seen or seen.add(x)]
        print(f"FAIL: {spec_path} inputs has duplicate name(s): {dupes}", file=sys.stderr)
        return 1

    print(f"feature_spec_leak_check: OK - {len(inputs)} inputs, "
          f"none are ground-truth/gt_*/consensus_* columns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
