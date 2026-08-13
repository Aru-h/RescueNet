# CONCORD / REMCE local test dashboard

A one-file Flask app that wraps your `tron` binary so you can run the
sim and see results (diag summary, rest-cycle verification, telemetry
charts) in two clicks instead of scrolling terminal output.

> **Layout note:** the project was split for laptop/board portability.
> `core/` is the platform-neutral simulator, `platform/laptop/` and
> `platform/board/` are the two entry points, and this dashboard
> (`app.py`, `templates/`, `static/`) now lives in `tools/` alongside
> the other laptop-only tooling (`label_snapshots.c`, the compare
> scripts).
> Run it as `python3 tools/app.py` from the repo root, or `cd tools &&
> python3 app.py` — either works, since it resolves the project root
> as its own parent directory rather than assuming co-location with
> the Makefile.

## Setup (one time)

1. Nothing to copy — `app.py` and `templates/` already live in
   `tools/` in this repo layout.
2. `pip install flask --break-system-packages` (or use a venv)

## Run

```
python3 app.py
```

Open http://127.0.0.1:5050

## Use

- Set seed / ticks / fault profile / tick delay, then either:
  - **Start Live** — streams the sim tick-by-tick over SSE. The Live
    Ops tab shows a 36×36 map (teams as colored dots, incidents as
    colored squares by priority, a ring around a team means it's
    resting, a red dot means an active fault) plus a status table per
    team and a live incident queue. Tick delay controls pacing — try
    100-300ms to actually watch it instead of it flashing by. **Stop**
    aborts the run and kills the underlying process cleanly.
  - **Run Full Speed (batch)** — the original non-live mode: runs to
    completion as fast as possible, then shows the same
    Overview/Recovery/Telemetry breakdown it always did. Use this for
    long runs (10k+ ticks) where watching every tick isn't the point.
- Either way, the app runs `make` (rebuilds only if needed) then
  `./tron <args>`, and parses:
  - stderr `[DIAG]` lines → rest-cycle durations, per-team throughput,
    safezone fallback / discarded-signal counts
  - `tron_snapshot_dataset.csv` → stamina-over-time per team, event
    type counts, recovery recommendation counts, watchdog anomalies
- **Overview** tab: run summary + per-team throughput chart.
- **Recovery / Rest** tab: rest-cycle duration histogram — this is
  the direct check for whether the ABORT_MISSION rest-guard fix is
  holding (short 1-tick cycles = interrupted rest, the old bug;
  long/sustained cycles = teams actually resting).
- **Telemetry** tab: stamina curves, event & recovery-action counts.
- **Raw Output** tab: full stdout/stderr tail if something looks off.
- **History** tab: every run this session, so you can compare seeds
  or fault profiles without re-running from scratch. Stored in
  `.dashboard_history.json` in your project dir.

## Notes

- This is a local dev server for your own laptop, not meant to be
  exposed to a network.
