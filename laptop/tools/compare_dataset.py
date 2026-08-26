import pandas as pd

EVENT_NAMES = {
    1: "EVENT_NEW_INCIDENT",
    2: "EVENT_TEAM_ASSIGNED",
    3: "EVENT_TEAM_ON_SCENE",
    4: "EVENT_STAGE_COMPLETE",
    5: "EVENT_TEAM_REST",
    6: "EVENT_TEAM_AVAILABLE",
    7: "EVENT_INCIDENT_RESOLVED",
}

df = pd.read_csv("tron_snapshot_dataset.csv")

counts = df["event_type"].value_counts().sort_index()
total = counts.sum()

print(f"Total events: {total:,}\n")

print(f"{'ID':<4}{'Event':<25}{'Count':>10}{'Percent':>12}")
print("-" * 55)

for event_id, count in counts.items():
    name = EVENT_NAMES.get(event_id, "UNKNOWN")
    pct = count / total * 100
    print(f"{event_id:<4}{name:<25}{count:>10}{pct:>11.4f}%")