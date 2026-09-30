#!/usr/bin/env python3
"""
laptop/tools/evaluate.py

Reproducible evaluation harness: does peer consensus (and, later, the AI
fault predictor) actually improve fault detection and mission outcome over
a rules-only baseline? Builds each requested variant of the laptop `tron`
binary, runs the seeds x1 each in an isolated working directory (so their
tron_snapshot_dataset.csv files never collide), computes per-class
detection/false-alarm/mission-outcome metrics (see eval_metrics.py's
docstring for exactly what "a detection" means and why), and writes a
results folder (per-seed CSV, a summary CSV, a paired-comparison CSV, and
a markdown report) - by default also copied to docs/evaluation/RESULTS.md.

Usage (matches `make evaluate`'s defaults - seeds 1001-1020, 3000 ticks,
fault-profile=random):

    python3 laptop/tools/evaluate.py \\
        --variants rules,rules+consensus \\
        --seeds-start 1001 --seeds-count 20 --ticks 3000 \\
        --profiles random \\
        --out-dir build/evaluate

Determinism (make check-evaluate-determinism exercises this): the same
seeds/ticks/variants produce byte-identical summary_metrics.csv and
report.md regardless of --workers, because (1) the simulator itself is a
pure function of (seed, ticks, fault-profile, build flags), (2) every
aggregation below iterates variants/profiles/seeds/classes/metrics in a
fixed, sorted order rather than worker-completion order, and (3) the
bootstrap CIs are seeded from a stable hash of (variant, class, metric) -
not from wall-clock time or PYTHONHASHSEED-randomized str hashing.

Exam/benchmark seeds are 1001-1020 (TASK.md); seeds 1-300 are the training
range and are refused outright (--force-training-seeds overrides, for
deliberate methodology experiments only - never for the real report).
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import random
import statistics
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eval_metrics as em  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

CC = os.environ.get("CC", "cc")
CFLAGS = ["-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS"]
LDFLAGS = ["-lm"]
INCLUDES = ["-Icore", "-Ilaptop"]
LAPTOP_TAIL_SOURCES = [
    "laptop/main.c",
    "laptop/debug.c",
    "laptop/snapshot.c",
    "laptop/mission_feed.c",
    "laptop/mission_mirror.c",
]

# Extensible: add a new variant here (and, if it needs build flags beyond a
# straight -D list, teach build_variant() about it) to evaluate it.
BASE_DEFINES = ["-DVIRTUAL_BUS_ENABLED=1"]
VARIANT_DEFINES = {
    "rules": BASE_DEFINES + ["-DCONSENSUS_ENFORCE=0"],
    "rules+consensus": BASE_DEFINES + ["-DCONSENSUS_ENFORCE=1"],
}
AI_VARIANT = "rules+consensus+ai"
ALL_KNOWN_VARIANTS = list(VARIANT_DEFINES) + [AI_VARIANT]

TRAINING_SEED_MAX = 300

CLASS_KEYS = list(em.FAULT_CLASSES) + ["mobility<=2", "mobility>2", "communication<=2", "communication>2"]
DETECTION_MODES = ("system", "any")

RATE_METRICS = ["detected_at_all_rate"] + [f"detected_within_{w}_rate" for w in em.DETECTION_WINDOWS]
SCALAR_METRICS = RATE_METRICS + [
    "action_taken_rate",
    "latency_median",
    "latency_p90",
    "false_alarms_per_1000_team_ticks",
    "false_alarm_mean_duration",
    "wrong_class_per_1000_team_ticks",
    "episodes_total",
]
MISSION_METRICS = ["incidents_resolved", "stage_completions"]
RACE_METRICS = [
    "episodes_total",
    "both_flagged",
    "consensus_first_rate",
    "mean_lead_ticks",
    "median_lead_ticks",
    "consensus_only_rate",
    "local_only_rate",
    "neither_rate",
]

BOOTSTRAP_SAMPLES = 10000


def class_mode_key(cls: str, mode: str) -> str:
    return f"{cls}|{mode}"


def race_key(cls: str) -> str:
    return f"race|{cls}"


def log(msg: str) -> None:
    print(f"[evaluate] {msg}", file=sys.stderr, flush=True)


def git_commit() -> str:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, capture_output=True, text=True, check=True
        )
        return out.stdout.strip()
    except Exception:
        return "unknown"


def build_variant(name: str, bin_path: Path, model_header: str | None) -> None:
    core_sources = sorted(glob.glob(str(REPO_ROOT / "core" / "*.c")))
    sources = core_sources + [str(REPO_ROOT / p) for p in LAPTOP_TAIL_SOURCES]

    if name == AI_VARIANT:
        # Same flags as `make tron-ai` (./tron_ai): the trained weights live in
        # core/fault_predictor_weights.h, which fault_predictor.c includes when
        # FAULT_MODEL_LOADED=1. --model-header, if given, is force-included
        # instead to override those weights (e.g. a candidate retrain).
        defines = BASE_DEFINES + ["-DCONSENSUS_ENFORCE=1", "-DFAULT_MODEL_LOADED=1"]
        if model_header is not None:
            header_path = Path(model_header).resolve()
            if not header_path.is_file():
                log(f"ERROR: --model-header '{model_header}' is not a file.")
                sys.exit(1)
            defines += ["-include", str(header_path)]
    else:
        defines = VARIANT_DEFINES[name]

    cmd = [CC] + CFLAGS + defines + INCLUDES + ["-o", str(bin_path)] + sources + LDFLAGS
    log(f"building variant '{name}': {' '.join(defines)}")
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


def _worker(task: dict) -> dict:
    """Runs one seed of one variant/profile in its own directory, parses
    the resulting CSV and diag text, and returns a fully scalar (JSON-
    serializable) metrics record. Executed in a worker process."""
    run_dir = Path(task["run_dir"])
    run_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        task["bin_path"],
        str(task["seed"]),
        "1",
        str(task["ticks"]),
        f"--fault-profile={task['profile']}",
        "--diag",
    ]
    result = subprocess.run(cmd, cwd=run_dir, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"tron exited {result.returncode} for variant={task['variant']} "
            f"seed={task['seed']} profile={task['profile']}\nstderr:\n{result.stderr}"
        )
    csv_path = run_dir / "tron_snapshot_dataset.csv"
    (run_dir / "diag.log").write_text(result.stderr)

    rows = em.parse_tick_rows(str(csv_path))
    class_metrics, race_metrics = em.compute_run_metrics(rows)
    mission = em.parse_mission_outcome(result.stderr)

    record: dict = {"variant": task["variant"], "profile": task["profile"], "seed": task["seed"]}
    for cls in CLASS_KEYS:
        for mode in DETECTION_MODES:
            m = class_metrics[(cls, mode)]
            record[class_mode_key(cls, mode)] = {
                "detected_at_all_rate": m.detected_at_all_rate(),
                **{f"detected_within_{w}_rate": m.detected_within_rate(w) for w in em.DETECTION_WINDOWS},
                "action_taken_rate": m.action_taken_rate(),
                "latency_median": m.latency_median(),
                "latency_p90": m.latency_p90(),
                "false_alarms_per_1000_team_ticks": m.false_alarms_per_1000_team_ticks(),
                "false_alarm_mean_duration": m.false_alarm_mean_duration(),
                "wrong_class_per_1000_team_ticks": m.wrong_class_per_1000_team_ticks(),
                "episodes_total": m.episodes_total,
            }
    record["mission_outcome"] = mission
    for cls in ("mobility", "communication"):
        r = race_metrics[cls]
        total = r.episodes_total
        record[race_key(cls)] = {
            "episodes_total": total,
            "both_flagged": r.both_flagged,
            "consensus_first_rate": r.consensus_first_rate(),
            "mean_lead_ticks": r.mean_lead_ticks(),
            "median_lead_ticks": r.median_lead_ticks(),
            "consensus_only_rate": (r.consensus_only / total) if total else None,
            "local_only_rate": (r.local_only / total) if total else None,
            "neither_rate": (r.neither / total) if total else None,
        }
    return record


def stable_seed(*parts: str) -> int:
    digest = hashlib.sha256("|".join(parts).encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


def bootstrap_ci(diffs: list, key: str) -> tuple:
    """95% bootstrap CI of the mean of `diffs`. Seeded deterministically
    from `key` (independent of process/worker/order) so results are
    reproducible across re-runs and worker counts."""
    if not diffs:
        return (None, None, None)
    rng = random.Random(stable_seed(key, "bootstrap"))
    n = len(diffs)
    means = []
    for _ in range(BOOTSTRAP_SAMPLES):
        sample = [diffs[rng.randrange(n)] for _ in range(n)]
        means.append(sum(sample) / n)
    means.sort()
    lo = means[int(0.025 * BOOTSTRAP_SAMPLES)]
    hi = means[min(BOOTSTRAP_SAMPLES - 1, int(0.975 * BOOTSTRAP_SAMPLES))]
    mean_diff = sum(diffs) / n
    return (mean_diff, lo, hi)


def mean_ignoring_none(values: list) -> float | None:
    present = [v for v in values if v is not None]
    if not present:
        return None
    return sum(present) / len(present)


def build_report(
    variants: list,
    baseline: str,
    profiles: list,
    seeds: list,
    ticks: int,
    records: dict,
    out_dir: Path,
) -> tuple:
    """Returns (summary_rows, paired_rows, markdown_text). summary_rows and
    paired_rows are lists of dicts written verbatim to CSV, in a fixed
    iteration order so re-runs are byte-identical."""
    all_keys = (
        [class_mode_key(cls, mode) for cls in CLASS_KEYS for mode in DETECTION_MODES]
        + ["mission_outcome"]
        + [race_key(cls) for cls in ("mobility", "communication")]
    )

    def metrics_for(key: str) -> list:
        if key == "mission_outcome":
            return MISSION_METRICS
        if key.startswith("race|"):
            return RACE_METRICS
        return SCALAR_METRICS

    summary_rows = []
    for profile in profiles:
        for variant in variants:
            for key in all_keys:
                for metric in metrics_for(key):
                    values = [records[(variant, profile, s)][key][metric] for s in seeds]
                    summary_rows.append(
                        {
                            "profile": profile,
                            "variant": variant,
                            "fault_class": key,
                            "metric": metric,
                            "mean": mean_ignoring_none(values),
                            "n_seeds": sum(1 for v in values if v is not None),
                        }
                    )

    paired_rows = []
    for profile in profiles:
        for variant in variants:
            if variant == baseline:
                continue
            for key in all_keys:
                for metric in metrics_for(key):
                    diffs = []
                    for s in seeds:
                        a = records[(variant, profile, s)][key][metric]
                        b = records[(baseline, profile, s)][key][metric]
                        if a is not None and b is not None:
                            diffs.append(a - b)
                    mean_diff, lo, hi = bootstrap_ci(diffs, f"{profile}|{variant}|{key}|{metric}")
                    if mean_diff is None:
                        verdict = "no_data"
                    elif lo is not None and hi is not None and (lo > 0) == (hi > 0) and lo != hi:
                        verdict = "real"
                    else:
                        verdict = "noise"
                    paired_rows.append(
                        {
                            "profile": profile,
                            "variant": variant,
                            "baseline": baseline,
                            "fault_class": key,
                            "metric": metric,
                            "n_seeds_paired": len(diffs),
                            "mean_diff": mean_diff,
                            "ci95_low": lo,
                            "ci95_high": hi,
                            "verdict": verdict,
                        }
                    )

    md = render_markdown(variants, baseline, profiles, seeds, ticks, summary_rows, paired_rows)
    return summary_rows, paired_rows, md


def fmt(value, digits=3) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def render_markdown(variants, baseline, profiles, seeds, ticks, summary_rows, paired_rows) -> str:
    summary_by_key = {(r["profile"], r["variant"], r["fault_class"], r["metric"]): r for r in summary_rows}
    paired_by_key = {(r["profile"], r["variant"], r["fault_class"], r["metric"]): r for r in paired_rows}

    lines = []
    lines.append("# Consensus evaluation results")
    lines.append("")
    lines.append(
        "Regenerated by `make evaluate` (laptop/tools/evaluate.py). "
        "Do not hand-edit - re-run the tool instead."
    )
    lines.append("")
    lines.append(f"- Commit: `{git_commit()}`")
    lines.append(f"- Variants: {', '.join(variants)} (baseline: `{baseline}`)")
    lines.append(f"- Seeds: {seeds[0]}-{seeds[-1]} ({len(seeds)} seeds), {ticks} ticks each")
    lines.append(f"- Fault profiles: {', '.join(profiles)}")
    lines.append("- Exact command:")
    lines.append("  ```")
    lines.append(
        "  python3 laptop/tools/evaluate.py --variants "
        f"{','.join(variants)} --baseline {baseline} --seeds-start {seeds[0]} "
        f"--seeds-count {len(seeds)} --ticks {ticks} --profiles {','.join(profiles)}"
    )
    lines.append("  ```")
    lines.append("- Build flags per variant:")
    for v in variants:
        if v in VARIANT_DEFINES:
            lines.append(f"  - `{v}`: `{' '.join(VARIANT_DEFINES[v])}`")
        else:
            lines.append(f"  - `{v}`: `{' '.join(BASE_DEFINES)} -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1` (same as `make tron-ai`)")
    lines.append("")
    lines.append(
        "See `docs/evaluation/README.md` for what counts as a \"detection\" "
        "and the known limitations (environment class has no detector; "
        "resource is a proxy; consensus only ever votes on mobility/"
        "communication)."
    )
    lines.append("")

    other_variants = [v for v in variants if v != baseline]

    def headline_table(mode: str, title: str, description: str) -> None:
        lines.append(f"### {title}, profile=`{profile}`")
        lines.append("")
        lines.append(description)
        lines.append("")
        header = ["fault class", f"{baseline} detected%"]
        for v in other_variants:
            header += [f"{v} detected%", "mean diff", "95% CI", "real or noise?"]
        lines.append("| " + " | ".join(header) + " |")
        lines.append("|" + "|".join(["---"] * len(header)) + "|")
        for cls in em.FAULT_CLASSES:
            key = class_mode_key(cls, mode)
            base_rate = summary_by_key[(profile, baseline, key, "detected_at_all_rate")]["mean"]
            row = [cls, fmt(base_rate)]
            for v in other_variants:
                v_rate = summary_by_key[(profile, v, key, "detected_at_all_rate")]["mean"]
                p = paired_by_key[(profile, v, key, "detected_at_all_rate")]
                ci = f"[{fmt(p['ci95_low'])}, {fmt(p['ci95_high'])}]" if p["mean_diff"] is not None else "n/a"
                row += [fmt(v_rate), fmt(p["mean_diff"]), ci, p["verdict"]]
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")

    def action_taken_table() -> None:
        lines.append(f"### Action taken (of detected episodes), profile=`{profile}`")
        lines.append("")
        lines.append(
            "Of episodes detected under the \"any evaluator flagged\" notion, the "
            "fraction where the fused `recovery_recommendation` actually reached "
            "RETURN_TO_SAFE_ZONE or higher at or after the first detection - i.e. "
            "detection that led to a dispatched action, not just an internal "
            "verdict."
        )
        lines.append("")
        header = ["fault class"] + variants
        lines.append("| " + " | ".join(header) + " |")
        lines.append("|" + "|".join(["---"] * len(header)) + "|")
        for cls in em.FAULT_CLASSES:
            key = class_mode_key(cls, "any")
            row = [cls]
            for v in variants:
                row.append(fmt(summary_by_key[(profile, v, key, "action_taken_rate")]["mean"]))
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")

    def race_table() -> None:
        lines.append(f"### Consensus vs. local evaluator: who flags first, profile=`{profile}`")
        lines.append("")
        lines.append(
            "Per ground-truth episode (mobility/communication only - the only two "
            "classes the peer-consensus evaluator can ever vote on), compares "
            "evaluate_peer_consensus()'s own first-flagged tick against the "
            "matching local evaluator's (evaluate_mobility()/evaluate_"
            "communication()) own first-flagged tick, independent of which one "
            "won the fused decision. `consensus_first_rate` is of episodes where "
            "BOTH eventually flagged; `mean/median_lead_ticks` is how many ticks "
            "earlier consensus was, in those cases. `consensus_only_rate`/`local_"
            "only_rate` are episodes only one of the two ever flagged at all."
        )
        lines.append("")
        header = ["metric"] + variants
        for cls in ("mobility", "communication"):
            lines.append(f"**{cls}**")
            lines.append("")
            lines.append("| " + " | ".join(header) + " |")
            lines.append("|" + "|".join(["---"] * len(header)) + "|")
            for metric in RACE_METRICS:
                row = [metric]
                for v in variants:
                    row.append(fmt(summary_by_key[(profile, v, race_key(cls), metric)]["mean"]))
                lines.append("| " + " | ".join(row) + " |")
            lines.append("")

    for profile in profiles:
        lines.append(f"## {baseline} vs. consensus, profile=`{profile}`")
        lines.append("")

        headline_table(
            "system",
            "Headline A: \"system decision\" detection",
            "Detection rate = fraction of ground-truth fault episodes where the "
            "fused `recovery_reason` (what recovery_dispatch() actually acts on) "
            "named this class at least once. Mean difference and 95% bootstrap CI "
            "are paired per-seed (`variant - baseline`). Concurrent faults can "
            "hide a real detection here - see Headline B.",
        )
        headline_table(
            "any",
            "Headline B: \"any evaluator flagged\" detection",
            "Same episodes, but detected if ANY of the 7 evaluators (core/"
            "recovery_manager.h's per-evaluator verdict columns) flagged this "
            "class that tick, whether or not it won the tick's fused decision. "
            "This is the fairer comparison when faults overlap - see docs/"
            "evaluation/README.md.",
        )
        lines.append(
            "**Note on the communication regression**: cause not yet "
            "determined; a likely factor is teams resting more often under "
            "consensus (evaluators are silent while resting)."
        )
        lines.append("")
        action_taken_table()
        race_table()

        lines.append("Mission outcome (mean total per run, all teams/incidents):")
        lines.append("")
        header2 = ["metric", f"{baseline}"]
        for v in other_variants:
            header2 += [v, "mean diff", "95% CI", "real or noise?"]
        lines.append("| " + " | ".join(header2) + " |")
        lines.append("|" + "|".join(["---"] * len(header2)) + "|")
        for metric in MISSION_METRICS:
            base_v = summary_by_key[(profile, baseline, "mission_outcome", metric)]["mean"]
            row = [metric, fmt(base_v, 1)]
            for v in other_variants:
                v_v = summary_by_key[(profile, v, "mission_outcome", metric)]["mean"]
                p = paired_by_key[(profile, v, "mission_outcome", metric)]
                ci = f"[{fmt(p['ci95_low'],1)}, {fmt(p['ci95_high'],1)}]" if p["mean_diff"] is not None else "n/a"
                row += [fmt(v_v, 1), fmt(p["mean_diff"], 1), ci, p["verdict"]]
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")

        lines.append(f"### Detail, profile=`{profile}`")
        lines.append("")
        for cls in CLASS_KEYS:
            for mode in DETECTION_MODES:
                key = class_mode_key(cls, mode)
                lines.append(f"**{cls}** (detection mode: {mode})")
                lines.append("")
                header3 = ["metric"] + variants
                lines.append("| " + " | ".join(header3) + " |")
                lines.append("|" + "|".join(["---"] * len(header3)) + "|")
                for metric in SCALAR_METRICS:
                    row = [metric]
                    for v in variants:
                        row.append(fmt(summary_by_key[(profile, v, key, metric)]["mean"]))
                    lines.append("| " + " | ".join(row) + " |")
                lines.append("")

    return "\n".join(lines) + "\n"


def write_csv(path: Path, rows: list, fieldnames: list) -> None:
    import csv

    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variants", default="rules,rules+consensus")
    parser.add_argument("--baseline", default="rules")
    parser.add_argument("--seeds-start", type=int, default=1001)
    parser.add_argument("--seeds-count", type=int, default=20)
    parser.add_argument("--ticks", type=int, default=3000)
    parser.add_argument("--profiles", default="random")
    parser.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--out-dir", default=str(REPO_ROOT / "build" / "evaluate"))
    parser.add_argument("--model-header", default=None)
    parser.add_argument("--force-training-seeds", action="store_true")
    parser.add_argument("--no-docs-copy", action="store_true", help="skip copying report.md to docs/evaluation/RESULTS.md")
    args = parser.parse_args()

    variants = [v.strip() for v in args.variants.split(",") if v.strip()]
    profiles = [p.strip() for p in args.profiles.split(",") if p.strip()]
    for v in variants:
        if v not in ALL_KNOWN_VARIANTS:
            log(f"ERROR: unknown variant '{v}'. Known variants: {', '.join(ALL_KNOWN_VARIANTS)}")
            return 1
    if args.baseline not in variants:
        log(f"ERROR: baseline '{args.baseline}' must be one of --variants ({variants})")
        return 1

    seeds = list(range(args.seeds_start, args.seeds_start + args.seeds_count))
    if not args.force_training_seeds and any(s <= TRAINING_SEED_MAX for s in seeds):
        log(
            f"ERROR: seed range includes seeds <= {TRAINING_SEED_MAX} (the training "
            "range - TASK.md forbids evaluating on it). Pass --force-training-seeds "
            "if this is a deliberate methodology experiment."
        )
        return 1

    out_dir = Path(args.out_dir).resolve()
    bin_dir = out_dir / "bin"
    runs_dir = out_dir / "runs"
    bin_dir.mkdir(parents=True, exist_ok=True)
    runs_dir.mkdir(parents=True, exist_ok=True)

    bin_paths = {}
    for variant in variants:
        slug = variant.replace("+", "_").replace("<", "_").replace(">", "_")
        bin_path = bin_dir / f"tron_{slug}"
        build_variant(variant, bin_path, args.model_header)
        bin_paths[variant] = str(bin_path)

    tasks = []
    for profile in profiles:
        for variant in variants:
            for seed in seeds:
                slug = variant.replace("+", "_")
                run_dir = runs_dir / slug / profile / f"seed_{seed}"
                tasks.append(
                    {
                        "variant": variant,
                        "profile": profile,
                        "seed": seed,
                        "ticks": args.ticks,
                        "bin_path": bin_paths[variant],
                        "run_dir": str(run_dir),
                    }
                )

    log(f"running {len(tasks)} seed-runs across {args.workers} worker(s)")
    records = {}
    if args.workers <= 1:
        for task in tasks:
            record = _worker(task)
            records[(record["variant"], record["profile"], record["seed"])] = record
    else:
        with ProcessPoolExecutor(max_workers=args.workers) as pool:
            for record in pool.map(_worker, tasks):
                records[(record["variant"], record["profile"], record["seed"])] = record

    summary_rows, paired_rows, md = build_report(variants, args.baseline, profiles, seeds, args.ticks, records, out_dir)

    write_csv(
        out_dir / "summary_metrics.csv",
        summary_rows,
        ["profile", "variant", "fault_class", "metric", "mean", "n_seeds"],
    )
    write_csv(
        out_dir / "paired_diff.csv",
        paired_rows,
        ["profile", "variant", "baseline", "fault_class", "metric", "n_seeds_paired", "mean_diff", "ci95_low", "ci95_high", "verdict"],
    )
    per_seed_path = out_dir / "per_seed_metrics.json"
    per_seed_path.write_text(
        json.dumps({f"{v}|{p}|{s}": records[(v, p, s)] for (v, p, s) in sorted(records)}, indent=2, sort_keys=True)
    )
    report_path = out_dir / "report.md"
    report_path.write_text(md)

    if not args.no_docs_copy:
        docs_dir = REPO_ROOT / "docs" / "evaluation"
        docs_dir.mkdir(parents=True, exist_ok=True)
        (docs_dir / "RESULTS.md").write_text(md)

    log(f"wrote {out_dir}/summary_metrics.csv, paired_diff.csv, per_seed_metrics.json, report.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
