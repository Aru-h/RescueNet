#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>
#include "mission_state.h"

void snapshot_begin_run(uint32_t seed);
/* The composite run_id computed for the current run (decision 5) -
 * for a caller that needs to tag its own side output with the exact
 * run_id the training CSV rows for this run carry, without recomputing
 * the hash a second time (laptop/tools/feature_pack_dump.c). */
uint32_t snapshot_current_run_id(void);
void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id);
/*
 * For logging a task's own resolution (complete_task() in incident.c).
 * Unlike snapshot_record(), incident-detail fields come directly from
 * `task` rather than a live lookup on the team currently assigned to
 * it - by the time a completion is detected, that team can already
 * have moved on to a new assignment earlier in the same tick, so a
 * live lookup would describe the wrong task.
 */
void snapshot_record_resolved(uint32_t cycle, TaskPool_t pool, const IncidentState_t *task);
void snapshot_end_run(void);
/* Peer-consensus observation log (CONSENSUS_DESIGN.md 5.2), laptop only.
 * Called by simulation_step() under VIRTUAL_BUS_ENABLED; a no-op unless
 * snapshot_peer_log_enable(1) was called first (--peer-log). Not defined
 * by the board snapshot backends, which never build the virtual bus. */
void snapshot_peer_log_enable(uint8_t enable);
void snapshot_record_peer_obs(uint32_t cycle);
void snapshot_close_dataset(void);

#endif
