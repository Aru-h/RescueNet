import pandas as pd

# Keep in sync with EventType_t in core/mission_state.h
EVENT_NAMES = {
    0: "EVENT_NONE",
    1: "EVENT_NEW_INCIDENT",
    2: "EVENT_TEAM_ASSIGNED",
    3: "EVENT_TEAM_ON_SCENE",
    4: "EVENT_STAGE_COMPLETE",
    5: "EVENT_TEAM_REST",
    6: "EVENT_TEAM_AVAILABLE",
    7: "EVENT_INCIDENT_RESOLVED",
    8: "EVENT_FAULT_INJECTED",
    9: "EVENT_FAULT_RECOVERED",
    10: "EVENT_WATCHDOG_ANOMALY_CHANGED",
    11: "EVENT_RECOVERY_ACTION_CHANGED",
    12: "EVENT_TICK",
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