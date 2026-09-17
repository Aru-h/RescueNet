#ifndef MISSION_MIRROR_H
#define MISSION_MIRROR_H

/*
 * Laptop side of a board mission (`tron ... --mission-feed`): the
 * dashboard's picture of the teams comes from the boards, not from a
 * second simulation running beside them.
 *
 * The simulator runs as SIM_ROLE_WORLD_MIRROR (core/simulation.h): it
 * keeps generating world events - incidents, hazards, faults, sweep
 * tasks, stage branches - and sends them out (laptop/mission_feed.h),
 * but it does not move teams, do work or drain stamina. Board 0 writes
 * every team's MissionLinkStatus_t to USART1; the bridge
 * (laptop/tools/telemetry_bridge.py) passes each one to this process's
 * stdin as a line
 *
 *     MLSTATUS <hex bytes of the complete wire frame>
 *
 * and mission_mirror_apply_status() copies it into g_teams[] and the
 * matching incident: status, position, stamina, equipment, which task
 * the team is on and that task's remaining work.
 *
 * Boards never report an incident as finished - their reports only say
 * which incident a team is on. When a team's report moves off an
 * incident, the mirror extrapolates that incident's work from the last
 * reports of every team on it (work_remaining - work_rate * elapsed
 * ticks, see mission_link.h section 4). Reaching zero means the stage
 * was done: complete_incident_stage() runs here, where the world is
 * authoritative, and its outcome (next stage or resolved) goes back to
 * the boards as an incident frame. Otherwise the team left early
 * (recovery, reassignment) and the incident is re-opened.
 */

#include <stdint.h>

#include "mission_link.h"

/* Forgets every earlier report. Call once per mission, after the world
   is generated. */
void mission_mirror_begin(void);

/* Applies one board report - see the file comment. */
void mission_mirror_apply_status(const MissionLinkStatus_t *status);

/* Parses one stdin line. Returns 1 if it was a valid MLSTATUS line (and
   applies it), 0 if it was anything else. */
int mission_mirror_ingest_line(const char *line);

/* Counters for diagnostics and tests. */
uint32_t mission_mirror_reports_applied(void);
uint32_t mission_mirror_lines_rejected(void);
uint32_t mission_mirror_stages_completed(void);
uint32_t mission_mirror_incidents_reopened(void);

#endif /* MISSION_MIRROR_H */
