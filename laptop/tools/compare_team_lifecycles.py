import pandas as pd

OLD_FILE = "tron_snapshot_dataset_1st_Edition.csv"
NEW_FILE = "tron_snapshot_dataset.csv"

old = pd.read_csv(OLD_FILE)
new = pd.read_csv(NEW_FILE)


def analyse(df, name):

    print("\n" + "="*100)
    print(name)
    print("="*100)

    # Sort chronologically
    df = df.sort_values(["team_id", "cycle"])

    ##########################################################
    # Status transitions
    ##########################################################

    print("\nSTATUS TRANSITIONS")

    transitions = {}

    for team, grp in df.groupby("team_id"):

        prev = None

        for s in grp["status"]:

            if prev is not None:

                key = (prev, s)

                transitions[key] = transitions.get(key, 0) + 1

            prev = s

    for t in sorted(transitions):

        print(f"{t[0]} -> {t[1]} : {transitions[t]}")

    ##########################################################
    # Incident lifecycle
    ##########################################################

    print("\nINCIDENT ASSIGNMENTS")

    assignments = 0
    completions = 0

    for team, grp in df.groupby("team_id"):

        prev = -1

        for inc in grp["assigned_incident"]:

            if prev == -1 and inc != -1:
                assignments += 1

            if prev != -1 and inc == -1:
                completions += 1

            prev = inc

    print("Assignments :", assignments)
    print("Completions :", completions)

    ##########################################################
    # Mission duration
    ##########################################################

    print("\nMISSION DURATIONS")

    durations = []

    for team, grp in df.groupby("team_id"):

        grp = grp.reset_index(drop=True)

        active = False
        start = None

        for _, row in grp.iterrows():

            if not active and row.assigned_incident != -1:
                active = True
                start = row.cycle

            elif active and row.assigned_incident == -1:
                durations.append(row.cycle - start)
                active = False

    if durations:

        print("Completed missions :", len(durations))
        print("Average duration   :", sum(durations)/len(durations))
        print("Minimum duration   :", min(durations))
        print("Maximum duration   :", max(durations))

    ##########################################################
    # State occupancy
    ##########################################################

    print("\nSTATE OCCUPANCY")

    print(df.status.value_counts(normalize=True).sort_index()*100)

    ##########################################################
    # Team utilisation
    ##########################################################

    print("\nTEAM UTILISATION")

    busy = (df.assigned_incident != -1).mean()*100

    print(f"{busy:.2f}% of logged states were assigned.")

    ##########################################################
    # Idle gaps
    ##########################################################

    print("\nLONG GAPS BETWEEN LOGGED STATES")

    gaps = []

    for team, grp in df.groupby("team_id"):

        diff = grp.cycle.diff().dropna()

        gaps.extend(diff.tolist())

    gaps = pd.Series(gaps)

    print(gaps.describe())

    ##########################################################
    # Event sequence lengths
    ##########################################################

    print("\nEVENT RUNS")

    runs = []

    for team, grp in df.groupby("team_id"):

        prev = None
        count = 0

        for e in grp.event_type:

            if e == prev:
                count += 1
            else:

                if count:
                    runs.append(count)

                count = 1
                prev = e

        runs.append(count)

    runs = pd.Series(runs)

    print(runs.describe())


analyse(old, "OLD DATASET")
analyse(new, "NEW DATASET")