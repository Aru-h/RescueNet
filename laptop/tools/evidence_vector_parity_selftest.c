/*
 * Standalone self-test confirming that recovery_manager.c's evaluate_
 * fault_predictor() (via its exposed recovery_manager_build_predictor_
 * evidence() helper) and fault_comms.c's fault_comms_broadcast_local_
 * teams() produce byte-identical FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES-
 * wide evidence vectors for the same team on the same tick.
 *
 * This is the "not just eyeballing the two call sites" check for audit
 * #163 now that the vector carries incident_priority/victim_count in
 * addition to the original 7 watchdog slots + equipment_level +
 * confirmed_mobility_impairment: both call sites independently look up
 * the team's assigned incident/system task (by design - see fault_
 * comms.c's own comment on why this isn't factored into a shared
 * helper), so nothing at compile time stops them drifting apart again.
 *
 * fault_comms_broadcast_local_teams() only writes its assembled vector
 * out via fault_buffer_record_local_evidence() (and onto the wire) - it
 * never returns it - so this test reads it back with the public fault_
 * buffer_get_local_sample() accessor rather than reaching into fault_
 * comms.c's private FaultBroadcastPayload_t.
 *
 * Build (same whole-core-directory pattern as this project's other
 * laptop/tools selftest files - recovery_manager.c/fault_comms.c's
 * translation units pull in enough of the rest of the simulator that
 * linking them alone against a hand-picked subset of core .c files is
 * impractical):
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic \
 *       -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -Icore -Ilaptop \
 *       -o evidence_vector_parity_selftest \
 *       laptop/tools/evidence_vector_parity_selftest.c \
 *       $(find core -maxdepth 1 -name "*.c") laptop/debug.c laptop/snapshot.c -lm
 * Run:
 *   ./evidence_vector_parity_selftest
 */

#include "fault_buffer.h"
#include "fault_comms.h"
#include "incident.h"
#include "mission_state.h"
#include "recovery_manager.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* Gives team_id an assigned incident with non-zero priority/victim_count
   so the two new features are actually exercised (not both 0.0f, which
   would pass trivially even if the lookups were wrong). */
static void assign_team_to_incident_slot(uint8_t team_id, uint16_t slot,
    Priority_t priority, uint8_t victim_count)
{
    memset(&g_incidents[slot], 0, sizeof(g_incidents[slot]));
    g_incidents[slot].slot_occupied = 1u;
    g_incidents[slot].priority = priority;
    g_incidents[slot].victim_count = victim_count;

    g_teams[team_id].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[team_id].assigned_task_slot = slot;
}

static void assign_team_to_system_slot(uint8_t team_id, uint16_t slot,
    Priority_t priority, uint8_t victim_count)
{
    memset(&g_system_tasks[slot], 0, sizeof(g_system_tasks[slot]));
    g_system_tasks[slot].slot_occupied = 1u;
    g_system_tasks[slot].priority = priority;
    g_system_tasks[slot].victim_count = victim_count;

    g_teams[team_id].assigned_task_pool = TASK_POOL_SYSTEM;
    g_teams[team_id].assigned_task_slot = slot;
}

static void unassign_team(uint8_t team_id)
{
    g_teams[team_id].assigned_task_pool = TASK_POOL_NONE;
    g_teams[team_id].assigned_task_slot = 0u;
}

/* Runs fault_comms_broadcast_local_teams() (which records team_id's
   freshly-assembled vector into the fault_buffer local ring) and
   recovery_manager_build_predictor_evidence() for the same team back to
   back, then compares the two resulting vectors byte-for-byte. */
static void check_parity_for(const char *label, uint8_t team_id)
{
    float predictor_evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES];
    FaultBufferLocalSample_t broadcast_sample;

    fault_comms_broadcast_local_teams();
    broadcast_sample = fault_buffer_get_local_sample(team_id);
    CHECK(broadcast_sample.known);

    recovery_manager_build_predictor_evidence(team_id, predictor_evidence);

    CHECK(memcmp(predictor_evidence, broadcast_sample.evidence,
                 sizeof(predictor_evidence)) == 0);

    if (memcmp(predictor_evidence, broadcast_sample.evidence,
               sizeof(predictor_evidence)) != 0)
    {
        printf("  [%s] mismatch for team %u:\n", label, team_id);
        for (unsigned i = 0; i < FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES; i++)
        {
            printf("    [%u] predictor=%f broadcast=%f\n", i,
                   (double)predictor_evidence[i],
                   (double)broadcast_sample.evidence[i]);
        }
    }
}

int main(void)
{
    const uint8_t team_id = 0u;

    fault_buffer_init(1u, NUM_TEAMS);

    /* Case 1: unassigned team - both new features must read back 0.0f. */
    unassign_team(team_id);
    check_parity_for("unassigned", team_id);

    /* Case 2: assigned to an incident-pool task with distinct, non-zero
       priority/victim_count values so the lookup is actually exercised. */
    assign_team_to_incident_slot(team_id, 0u, PRIORITY_HIGH, 7u);
    check_parity_for("incident-pool", team_id);

    /* Case 3: assigned to a system-pool task instead. */
    assign_team_to_system_slot(team_id, 0u, PRIORITY_LOW, 3u);
    check_parity_for("system-pool", team_id);

    if (g_failures == 0)
    {
        printf("All evidence-vector parity checks passed "
               "(FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES=%u).\n",
               (unsigned)FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES);
        return 0;
    }

    printf("%d check(s) FAILED.\n", g_failures);
    return 1;
}
