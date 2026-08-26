import pandas as pd

OLD_FILE = "tron_snapshot_dataset_1st_Edition.csv"
NEW_FILE = "tron_snapshot_dataset.csv"

old = pd.read_csv(OLD_FILE)
new = pd.read_csv(NEW_FILE)

print("="*90)
print("TRON BEHAVIOUR COMPARISON")
print("="*90)

###############################################################
# Helper
###############################################################

def section(title):
    print("\n" + "="*90)
    print(title)
    print("="*90)

###############################################################
# Dataset size
###############################################################

section("DATASET SIZE")

print(f"Old rows : {len(old):,}")
print(f"New rows : {len(new):,}")

###############################################################
# Event counts (absolute)
###############################################################

section("ABSOLUTE EVENT COUNTS")

events = sorted(set(old.event_type.unique()) | set(new.event_type.unique()))

for e in events:

    o = (old.event_type == e).sum()
    n = (new.event_type == e).sum()

    diff = n-o

    print(f"Event {e:2d} : "
          f"{o:8d} -> {n:8d}   "
          f"Diff {diff:+8d}")

###############################################################
# Status counts
###############################################################

section("STATUS COUNTS")

states = sorted(set(old.status.unique()) | set(new.status.unique()))

for s in states:

    o=(old.status==s).sum()
    n=(new.status==s).sum()

    print(f"Status {s}: {o:8d} -> {n:8d}")

###############################################################
# Incident statistics
###############################################################

section("INCIDENT STATISTICS")

for name,df in [("OLD",old),("NEW",new)]:

    print(f"\n{name}")

    assigned = df[df.assigned_incident!=-1]

    print("Rows assigned to incidents :",len(assigned))

    print("Unique incidents           :",assigned.assigned_incident.nunique())

    print("Unique priorities          :",assigned.incident_priority.unique())

###############################################################
# Watchdog activity
###############################################################

section("WATCHDOG")

for name,df in [("OLD",old),("NEW",new)]:

    print(f"\n{name}")

    anomalies=(df.watchdog_anomaly_detected==1).sum()

    print("Anomalies detected :",anomalies)

    print("Failure modes")

    print(df.watchdog_failure_mode.value_counts().sort_index())

###############################################################
# Recovery activity
###############################################################

section("RECOVERY")

for name,df in [("OLD",old),("NEW",new)]:

    print(f"\n{name}")

    print(df.recovery_recommendation.value_counts().sort_index())

###############################################################
# Duplicate rows
###############################################################

section("DUPLICATE SNAPSHOTS")

key = [
    "cycle",
    "team_id",
    "status",
    "assigned_incident",
    "event_type",
    "loc_x",
    "loc_y"
]

old_dup = old.duplicated(subset=key).sum()
new_dup = new.duplicated(subset=key).sum()

print("Old duplicate snapshots :",old_dup)
print("New duplicate snapshots :",new_dup)

###############################################################
# Cycle coverage
###############################################################

section("CYCLE COVERAGE")

print("Old unique cycles :",old.cycle.nunique())
print("New unique cycles :",new.cycle.nunique())

print("Old avg rows/cycle :",
      round(len(old)/old.cycle.nunique(),2))

print("New avg rows/cycle :",
      round(len(new)/new.cycle.nunique(),2))

###############################################################
# Team activity
###############################################################

section("ROWS PER TEAM")

for team in sorted(old.team_id.unique()):

    o=(old.team_id==team).sum()
    n=(new.team_id==team).sum()

    print(f"Team {team}: {o:8d} -> {n:8d}")

###############################################################
# Assignment transitions
###############################################################

section("ASSIGNMENT RATE")

for name,df in [("OLD",old),("NEW",new)]:

    assigned=(df.assigned_incident!=-1).sum()

    print(f"{name}")

    print("Assigned rows :",assigned)
    print("Percentage    :",round(100*assigned/len(df),2),"%")

###############################################################
# Path cost sanity
###############################################################

section("PATH COST CHECK")

for name,df in [("OLD",old),("NEW",new)]:

    print(f"\n{name}")

    print(df.twin_remaining_path_cost.describe())

    suspicious=df[df.twin_remaining_path_cost>1000]

    print("Values >1000 :",len(suspicious))

###############################################################
# Behaviour summary
###############################################################

section("INTERPRETATION")

print("""
Questions to answer:

1. Are the absolute event counts similar?

2. Are there roughly the same number of unique incidents?

3. Are watchdog anomalies comparable?

4. Are recovery recommendations comparable?

5. Did duplicate snapshots decrease?

6. Did average rows per cycle decrease?

If:

✓ duplicate snapshots decrease
✓ rows/cycle decrease
✓ unique incidents stay similar
✓ watchdog activity stays similar
✓ recovery activity stays similar

then the optimisation probably removed redundant logging.

If event counts, incidents or watchdog activity collapse,
then the simulator behaviour changed rather than just the logging.
""")