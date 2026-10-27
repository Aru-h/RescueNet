#!/usr/bin/env python3
"""Step 3.7 consistency check on one `--diag` stderr log of a run with --causal-fixes=...,hold-repair.

  check_hold_repair.py <diag.log>

  * every started repair is accounted for: started == ok + failed + cancelled + still running at the end (<= 10 teams)
  * the cancellation causes add up to the cancellations
  * every successful repair reset the recovery state exactly once (H3)
  * the cap is exercised (recommendations were capped) and repairs do complete
"""
import re, sys

text = open(sys.argv[1]).read()
field = {int(m.group(1)): tuple(int(x) for x in m.groups()[1:])
         for m in re.finditer(r"\[DIAG FIELD\] kind=(\d+) started=(\d+) ok=(\d+) failed=(\d+) cancelled=(\d+)", text)}
cancel = {int(m.group(1)): [int(x) for x in m.groups()[1:]]
          for m in re.finditer(r"\[DIAG HOLDCANCEL\] kind=(\d+) stamina=(\d+) other_class=(\d+) planner=(\d+) released=(\d+) fault_ended=(\d+) other=(\d+)", text)}
hold = re.search(r"\[DIAG HOLD\] capped_return=(\d+) capped_realloc=(\d+) capped_abort=(\d+) lifted_stamina=(\d+) lifted_other=(\d+) h2_refused=(\d+) h3_resets=(\d+)", text)
errs = []
if not field or not cancel or not hold:
    sys.exit("check_hold_repair: DIAG FIELD / HOLDCANCEL / HOLD lines missing (token not on?)")
for k in (0, 1):
    started, ok, failed, canc = field[k]
    running = started - ok - failed - canc
    if running < 0 or running > 10:
        errs.append(f"kind {k}: started {started} ok {ok} failed {failed} cancelled {canc} (unaccounted {running})")
    if sum(cancel[k]) != canc:
        errs.append(f"kind {k}: cancel causes sum to {sum(cancel[k])}, cancelled {canc}")
h = [int(x) for x in hold.groups()]
if h[6] != field[0][1] + field[1][1]:
    errs.append(f"H3 resets {h[6]} != successful repairs {field[0][1] + field[1][1]}")
if h[0] + h[1] + h[2] == 0:
    errs.append("the cap was never applied")
if field[0][1] == 0:
    errs.append("no mobility repair completed")
if errs:
    sys.exit("check_hold_repair FAILED:\n  " + "\n  ".join(errs))
print(f"check_hold_repair: mobility repairs started {field[0][0]} ok {field[0][1]} failed {field[0][2]} cancelled {field[0][3]}; "
      f"resource started {field[1][0]} ok {field[1][1]}; capped {h[0] + h[1] + h[2]}, resets {h[6]}: consistent")
