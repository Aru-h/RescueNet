#ifndef FAULT_PREDICTOR_FEATURES_H
#define FAULT_PREDICTOR_FEATURES_H

#include "mission_state.h"

/*
 * Track A, Sections 5.1 + 5.2: the two "history" features the fault
 * predictor's feature vector needs that a single current-tick read
 * can't provide - stall_frac_last_5/_10 (5.1) and delta_priority/
 * delta_victim_count (5.2). Everything in this file is new, owned
 * entirely by the ML side - it does not edit or depend on any
 * function in core/recovery_manager.c or core/simulation.c.
 *
 * Fixed-size, no dynamic allocation - same convention as
 * TeamFaultState_t in mission_state.h. One FaultPredictorTeamState_t
 * per team (g_fault_predictor_team_state[NUM_TEAMS], private to the
 * .c file) and one FaultPredictorIncidentBaseline_t per incident slot
 * (g_fault_predictor_incident_baseline[NUM_INCIDENTS], also private).
 */

/* Ring-buffer window sizes - must match data_prep.py's
 * rolling(5)/rolling(10) windows exactly, since the trained model
 * expects features computed over these exact spans. */
#define FAULT_PREDICTOR_STALL_WINDOW_SHORT 5
#define FAULT_PREDICTOR_STALL_WINDOW_LONG  10

/*
 * Per-team persistent state for 5.1. moved_history is a single
 * circular buffer sized to the LONGER window (10) - the 5-tick
 * fraction just reads the newest 5 entries of the same buffer, there
 * is no second buffer.
 */
typedef struct
{
    uint8_t moved_history[FAULT_PREDICTOR_STALL_WINDOW_LONG]; /* 1 = moved that tick, 0 = stalled */
    uint8_t history_count; /* ticks recorded so far this run, caps at WINDOW_LONG */
    uint8_t history_head;  /* next slot to write */
    int16_t last_loc_x;
    int16_t last_loc_y;
    uint8_t has_last_loc;  /* 0 until this team's first observed tick this run */
} FaultPredictorTeamState_t;

/*
 * Per-incident baseline memory for 5.2. Indexed by g_incidents[]
 * SLOT, with the slot's incident_id stored alongside: incidents DO
 * spawn mid-run (generate_incidents_if_needed(), called every tick from
 * core/simulation.c), and a freed slot is reused for a new incident, so
 * the id check below is what keeps a recycled slot from reusing the
 * previous incident's baseline.
 *
 * Captured the first time ANY team is observed assigned to that slot
 * - this matches data_prep.py's groupby(run_id, assigned_incident)
 * .first() exactly (the earliest logged row for that incident across
 * ALL teams, not the earliest row for one specific team). Do not key
 * this per-team - a per-team baseline would silently diverge from
 * what the model was trained against whenever a support_team is also
 * assigned to the same incident.
 *
 * NOT safe to assume a slot's logical incident identity is fixed for
 * the whole run - confirmed directly against core/incident.c: once an
 * incident resolves and frees its slot (slot_occupied = 0),
 * generate_incident_at()/find_free_incident_slot() can and routinely
 * does hand that same slot to a brand-new incident with a different
 * incident_id. incident_id is stored here specifically to detect that
 * recycling (data_prep.py never needed this check, since it groups by
 * the real incident_id directly rather than by array position).
 */
typedef struct
{
    uint8_t captured;
    uint32_t incident_id;           /* which incident this baseline was
                                      * captured for - used to detect a
                                      * slot being recycled for a new,
                                      * unrelated incident mid-run. */
    uint8_t baseline_priority;      /* raw Priority_t value, stored as
                                      * plain uint8_t rather than the
                                      * enum itself - see mission_
                                      * state.h's TaskPool_t/CBBAAgent
                                      * State_t note on enum size
                                      * varying across host vs. ARM
                                      * EABI; storing the raw value
                                      * sidesteps that entirely. */
    uint8_t baseline_victim_count;
} FaultPredictorIncidentBaseline_t;

/* Call once, wherever the sim resets per-run state (same place
 * g_teams[]/g_incidents[] themselves get reinitialized for a new
 * run/seed) - clears all history and baselines so nothing leaks
 * across runs. */
void fault_predictor_features_init(void);

/*
 * Call once per team per tick, AFTER that team's position/status/
 * assignment has been updated for this tick and BEFORE the full
 * feature vector is assembled or fault_predictor_run() is called.
 * See core/simulation.c's tick loop for the intended call site
 * (Track A's one required edit to a file that isn't this one).
 */
void fault_predictor_features_update_tick(uint8_t team_id);

/*
 * Fraction of the last `window` ticks (FAULT_PREDICTOR_STALL_WINDOW_
 * SHORT or _LONG - any other value returns 0.0f) this team did NOT
 * move, 0.0-1.0. Matches data_prep.py's stall_frac_last_5/_10 given
 * the same per-tick update-call sequence, including its min_periods=1
 * behavior (divides by ticks actually observed so far when fewer than
 * `window` ticks have happened yet this run, not always by `window`).
 */
float fault_predictor_stall_frac(uint8_t team_id, uint8_t window);

/*
 * current value minus this team's currently-assigned incident's
 * captured baseline. Returns 0.0f if the team has no incident
 * currently assigned (assigned_task_pool != TASK_POOL_INCIDENT or
 * assigned_task_slot == NO_ASSIGNMENT), matching data_prep.py's
 * fillna(0) for unassigned rows.
 */
float fault_predictor_delta_priority(uint8_t team_id);
float fault_predictor_delta_victim_count(uint8_t team_id);

#endif
