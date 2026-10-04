# Measurement patches and reports (not part of the simulator)

Used for the investigations in `docs/CONSEQUENTIAL_ROADS.md` and
`docs/SPECIALIZATION_LOAD.md`. The patches add counters or a trace file to a
**scratch copy** of `core/`; they are never applied to the repo and change no
behaviour (each instrumented build was checked to reproduce the recorded per-seed
results exactly).

| Patch | Adds | Report |
|---|---|---|
| `trace_team_status.patch` | per team per tick CSV (`TRACE_CSV=<file>`): flag, status before/after dispatch, recommendation, reason | `trace_status_report.py`, `trace_outofservice_report.py` |
| `reroute_probe.patch` | reroute detour/stall counters (`RRDIAG`), legacy recommendation counters (`RRREC`) | `reroute_probe_report.py`, `reroute_recovery_report.py` |
| `spec_load.patch` | per-specialization queue, wait, utilisation, stages, victims (`SD_*` lines) | `spec_load_report.py` |

`dose_response_report.py` reads runs of the unmodified oracle builds
(`laptop/tools/causal_actions_oracle.sh`) with `--roads-persist` varied.

Reproduce: `cp -r core laptop /tmp/x/ && cd /tmp/x && patch -p1 < <patch>`, build with
the flags in `laptop/tools/causal_actions_oracle.sh` (drop `-DFAULT_MODEL_LOADED=1`
for the AI-off runs), run `<binary> <seed> 1 3000 --fault-profile=causal
--fault-stress=7 --diag` into one directory per run (`diag.log` is read; a file
`ok` marks a finished run), then pass the runs directory to the report script.
The trace reports expect `trace.csv` and `diag.log` in each run directory. The
oracle environment detector is the `scores.score[3]` patch in
`causal_actions_oracle.sh`.
