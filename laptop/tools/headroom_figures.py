#!/usr/bin/env python3
"""headroom_figures.py -- figures of docs/HEADROOM_MAP.md, drawn from the committed per-run tables.

  python3 laptop/tools/headroom_figures.py        writes docs/figures/headroom_team_equivalents.png and time_budget.png
"""
import glob, re, subprocess, sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
csvs = ",".join(sorted(glob.glob(os.path.join(ROOT, "docs/data/headroom/*.csv"))))
out = subprocess.run([sys.executable, os.path.join(ROOT, "laptop/tools/headroom_report.py"), "screen", "--res", "x",
                      "--csv", csvs, "--tiers", "1,2,3,4,5"], capture_output=True, text=True).stdout
PICK = [
    ("Detector oracle (D2)", "D2 detector oracle, no delay (PD)"),
    ("Hazard forecast + prevention (S1)", "S1 hazard forecast + prevention x1 (level ii)"),
    ("Hazard-arrival foresight 50 ticks (M4)", "M4 hazard-arrival foresight, 50 ticks"),
    ("Road information, omniscient vs R1", "R1: value of road information (omniscient minus R1)"),
    ("All five cheaper responses (RR0)", "RR0 all five responses (not jointly priced)"),
    ("Perfect communications (DQ2)", "DQ2 perfect comms (no comm faults, no bus loss)"),
    ("Central optimal assignment (DQ1)", "DQ1 central optimal assignment"),
    ("Discovery oracle, no fog of war (M5)", "M5 discovery oracle (no fog of war)"),
    ("Rest 25% shorter (K1)", "K1 rest duration x0.75"),
    ("Work 25% faster, drain tied (W1b)", "W1b work x1.25 (drain tied to work)"),
    ("Work 25% faster, drain per tick (W1a)", "W1a work x1.25 (drain per tick unchanged)"),
    ("Extraction work x2 (W2)", "W2 extraction work x2"),
    ("Instant travel to tasks (V0)", "V0 instant travel to tasks"),
    ("Team mix a (TM1)", "TM1 team mix a"),
    ("One team offline (K2, loss)", "K2 1 team(s) offline"),
]
# report columns: probe | stress | incidents | team-equivalents | victims | spawned | survives
te = {}
for l in out.splitlines():
    c = [x.strip() for x in l.strip("|").split("|")]
    if len(c) >= 6 and c[1] in ("2", "7"):
        m = re.match(r"([+-][\d.]+) \[([+-][\d.]+), ([+-][\d.]+)\]", c[3])
        if m:
            te[(c[0], int(c[1]))] = tuple(float(v) for v in m.groups())
inc = {}
for l in out.splitlines():
    c = [x.strip() for x in l.strip("|").split("|")]
    if len(c) >= 6 and c[1] in ("2", "7"):
        m = re.match(r"([+-][\d.]+) \[([+-][\d.]+), ([+-][\d.]+)\]", c[2])
        if m:
            inc[(c[0], int(c[1]))] = tuple(float(v) for v in m.groups())

fig, axes = plt.subplots(1, 2, figsize=(11, 6.2), sharey=True)
for ax, st in zip(axes, (2, 7)):
    for i, (lab, key) in enumerate(PICK):
        v = te.get((key, st))
        if v is None:
            continue
        col = "#b03a2e" if v[0] < 0 else "#1f6f8b"
        ax.errorbar(v[0], i, xerr=[[v[0] - v[1]], [v[2] - v[0]]], fmt="o", color=col, capsize=3)
    ax.axvline(0, color="#444", lw=0.8)
    ax.set_title(f"Stress {st} (95% CI)")
    ax.set_xlabel("team-equivalents")
    ax.set_yticks(range(len(PICK)))
    ax.set_yticklabels([p[0] for p in PICK])
    ax.invert_yaxis()
    ax.grid(axis="x", alpha=0.25)
fig.suptitle("Headroom in team-equivalents: information and decisions are small, work speed and capacity are large")
fig.tight_layout()
fig.savefig(os.path.join(ROOT, "docs/figures/headroom_team_equivalents.png"), dpi=140)

# time budget
cats = ["resting", "to rest", "held in repair", "faulted", "on scene", "to task", "available"]
data = {"P stress 2": [49.8, 10.4, 0.0, 3.2, 9.5, 24.5, 2.6], "P stress 7": [53.6, 15.4, 0.0, 6.2, 7.0, 15.9, 1.9],
        "F stress 2": [49.1, 10.4, 0.9, 2.5, 9.9, 24.1, 3.3], "F stress 7": [53.5, 15.4, 1.7, 4.6, 6.8, 15.6, 2.3]}
cols = ["#7a8b99", "#a9b7c2", "#d9a441", "#b03a2e", "#2e7d32", "#1f6f8b", "#8e6bbf"]
fig, ax = plt.subplots(figsize=(8, 3.6))
for r, (name, vals) in enumerate(data.items()):
    left = 0
    for j, v in enumerate(vals):
        ax.barh(r, v, left=left, color=cols[j], label=cats[j] if r == 0 else None)
        left += v
ax.set_yticks(range(len(data)))
ax.set_yticklabels(list(data))
ax.invert_yaxis()
ax.set_xlabel("share of team-ticks (%)")
ax.set_title("Where team time goes (mean over 100 development seeds)")
ax.legend(ncol=4, fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.22))
fig.tight_layout()
fig.savefig(os.path.join(ROOT, "docs/figures/time_budget.png"), dpi=140)
print("ok", len(te))
