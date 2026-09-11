import pandas as pd
df = pd.read_csv("tron_snapshot_dataset.csv")
rest_events = df[df["event_type"] == 5]
print(rest_events.groupby(["world_seed", "team_id"]).size().sort_values(ascending=False))