/*
 * Confirms board/STM32N6570-DK/mission_link_task.c's mission_link_
 * status_tx_step() new RECOVERY_ACTION_ABORT_MISSION gate: an aborted
 * team stops sending MissionLinkStatus_t frames, while a still-active
 * team's reporting is unaffected - with REAL frame sequencing, not an
 * isolated stub. This links the REAL board/STM32N6570-DK/mission_
 * link_task.c (the file with the actual fix) and calls its REAL
 * public mission_link_task_step() entry point, exactly as board/
 * STM32N6570-DK/main_board.c's own tick loop does.
 *
 * "Real frame sequencing" here means: the actual wire bytes mission_
 * link_send_status() writes via tm_putchar() (board 0's USART1-to-
 * laptop path - g_comms_transport_this_board_id defaults to 0) are
 * captured by this test's own stub tm/tmonitor.h (laptop/tools/
 * mission_link_gate_test_stubs/) and decoded with the REAL core/
 * mission_link_codec.h mission_link_parse_status() - this test reads
 * back genuine, wire-format-correct MissionLinkStatus_t frames, not a
 * hand-rolled approximation of what the wire protocol looks like.
 *
 * Uses a SEPARATE stub tm/tmonitor.h from laptop/tools/harness_stubs/
 * (two_board_harness.c's own, already-hardened stub) - this test does
 * not touch or depend on that harness at all.
 *
 * Overrides fault_predictor_run()/watchdog_get_status()/watchdog_get_
 * evidence_vector() (core/fault_predictor.c and core/watchdog.c
 * excluded from the link) so a real ABORT_MISSION recommendation can
 * be driven with a synthetic confidence sequence - same technique as
 * laptop/tools/fault_comms_abort_gate_test.c.
 *
 * Build: see Makefile's check-mission-link-status-gate target.
 */
#include "mission_link.h"
#include "mission_link_codec.h"
#include <tm/tmonitor.h> /* MISSION_LINK_GATE_TEST_CAPTURE_MAX, tm_putchar() stub */
#include "fault_predictor.h"
#include "watchdog.h"
#include "recovery_manager.h"
#include "mission_state.h"
#include "comms_transport.h"

#include <stdio.h>
#include <string.h>

/* Referenced by laptop/tools/mission_link_gate_test_stubs/tm/tmonitor.h's
   tm_putchar() - defined here, the test's own translation unit. */
unsigned char g_mission_link_gate_test_captured_bytes[MISSION_LINK_GATE_TEST_CAPTURE_MAX];
int g_mission_link_gate_test_captured_len = 0;

extern void mission_link_task_step(void);

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

/* --- test-controlled stubs, replacing fault_predictor.c/watchdog.c --- */
static FaultPredictorScores_t g_test_scores[NUM_TEAMS];
static WatchdogStatus_t g_test_watchdog[NUM_TEAMS];
static uint8_t g_call_index = 0u;

FaultPredictorScores_t fault_predictor_run(const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES])
{
    (void)evidence;
    FaultPredictorScores_t scores = g_test_scores[g_call_index];
    g_call_index = (uint8_t)((g_call_index + 1u) % NUM_TEAMS);
    return scores;
}

WatchdogStatus_t watchdog_get_status(uint8_t team_id) { return g_test_watchdog[team_id]; }

void watchdog_get_evidence_vector(uint8_t team_id, float out_evidence[7],
    uint8_t *out_confirmed_mobility_impairment)
{
    (void)team_id;
    memset(out_evidence, 0, 7 * sizeof(float));
    *out_confirmed_mobility_impairment = 0u;
}

void watchdog_init(void) { }
void watchdog_step(void) { }

/* board_usart1_read_byte() is __attribute__((weak)) in board/STM32N6570-
   DK/mission_link_task.c itself (default: -1, "nothing available") -
   this test relies on that real weak default, no override needed. */

static void set_confidence_and_watchdog(uint8_t team_id, float confidence, uint8_t watchdog_agrees)
{
    memset(&g_test_scores[team_id], 0, sizeof(g_test_scores[team_id]));
    g_test_scores[team_id].score[0] = confidence;
    g_test_watchdog[team_id].anomaly_detected = watchdog_agrees;
    g_test_watchdog[team_id].anomaly_confidence = watchdog_agrees ? 0.5f : 0.0f;
    g_test_watchdog[team_id].dominant_failure_mode = 0;
}

static const char *action_name(RecoveryAction_t a)
{
    switch (a) {
        case RECOVERY_ACTION_CONTINUE_MONITORING: return "CONTINUE_MONITORING";
        case RECOVERY_ACTION_LOCAL_RECOVERY: return "LOCAL_RECOVERY";
        case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE: return "RETURN_TO_SAFE_ZONE";
        case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION: return "REQUEST_TASK_REALLOCATION";
        case RECOVERY_ACTION_ABORT_MISSION: return "ABORT_MISSION";
        default: return "?";
    }
}

/* Scans the captured byte stream for every valid MISSION_LINK_STATUS_
   WIRE_SIZE-aligned frame that mission_link_parse_status() accepts,
   and records which team_ids actually appeared - a real decode of the
   real wire bytes, not a byte-count guess. */
static void decode_captured_status_frames(uint8_t seen_team[NUM_TEAMS])
{
    memset(seen_team, 0, NUM_TEAMS);
    for (int offset = 0; offset + (int)MISSION_LINK_STATUS_WIRE_SIZE <= g_mission_link_gate_test_captured_len; offset++)
    {
        MissionLinkStatus_t status;
        if (mission_link_parse_status(&g_mission_link_gate_test_captured_bytes[offset],
                MISSION_LINK_STATUS_WIRE_SIZE, &status) == 1) /* 1 = parsed OK; 0 = short buffer; -1 = bad magic/checksum */
        {
            if (status.team_id < NUM_TEAMS)
                seen_team[status.team_id] = 1u;
            offset += (int)MISSION_LINK_STATUS_WIRE_SIZE - 1; /* skip past this frame */
        }
    }
}

int main(void)
{
    g_comms_transport_this_board_id = 0u; /* board 0 -> mission_link_send_status() takes the USART1/tm_putchar path */

    memset(g_teams, 0, sizeof(g_teams));
    memset(g_incidents, 0, sizeof(g_incidents));
    memset(g_system_tasks, 0, sizeof(g_system_tasks));
    memset(g_mission_execution_twin, 0, sizeof(g_mission_execution_twin));
    memset(g_test_watchdog, 0, sizeof(g_test_watchdog));
    memset(g_test_scores, 0, sizeof(g_test_scores));

    for (uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        g_teams[t].team_id = t;
        g_teams[t].stamina = 100.0f;
        g_teams[t].equipment_level = 100u;
        g_teams[t].status = 1u; /* STATUS_EN_ROUTE */
        g_teams[t].degraded = 1u; /* lets team 0 reach ABORT_MISSION, not just RETURN */
        g_mission_execution_twin[t].team_id = t;
        g_mission_execution_twin[t].mission_stage = 1u; /* MISSION_STAGE_TRAVEL */
    }

    recovery_manager_init();
    comms_transport_init();
    g_current_tick = 0u;

    printf("=== Driving team 0 to a REAL ABORT_MISSION recommendation; team 1 stays healthy ===\n");
    RecoveryAction_t team0_reached = RECOVERY_ACTION_CONTINUE_MONITORING;
    for (int tick = 1; tick <= 30; tick++)
    {
        g_current_tick = (uint32_t)tick;
        set_confidence_and_watchdog(0, 0.90f, 1u);
        set_confidence_and_watchdog(1, 0.0f, 0u);
        for (uint8_t t = 2; t < NUM_TEAMS; t++)
            set_confidence_and_watchdog(t, 0.0f, 0u);

        recovery_manager_step();

        RecoveryAction_t r0 = recovery_manager_get_recommendation(0);
        if (r0 == RECOVERY_ACTION_ABORT_MISSION)
        {
            team0_reached = r0;
            break;
        }
    }
    printf("  team0=%s team1=%s\n", action_name(recovery_manager_get_recommendation(0)),
        action_name(recovery_manager_get_recommendation(1)));
    CHECK(team0_reached == RECOVERY_ACTION_ABORT_MISSION);
    CHECK(recovery_manager_get_recommendation(1) == RECOVERY_ACTION_CONTINUE_MONITORING);

    printf("\n=== Calling the REAL mission_link_task_step() once, capturing real USART1 wire bytes ===\n");
    g_mission_link_gate_test_captured_len = 0;
    mission_link_task_step();

    printf("  Captured %d raw wire bytes\n", g_mission_link_gate_test_captured_len);
    CHECK(g_mission_link_gate_test_captured_len > 0); /* something real was sent - proves this isn't a no-op run */

    uint8_t seen_team[NUM_TEAMS];
    decode_captured_status_frames(seen_team);

    printf("  Decoded real MissionLinkStatus_t frames - team_ids seen: ");
    for (uint8_t t = 0; t < NUM_TEAMS; t++)
        if (seen_team[t]) printf("%u ", t);
    printf("\n");

    CHECK(seen_team[0] == 0u); /* team 0 (ABORT_MISSION) must NOT appear */
    CHECK(seen_team[1] == 1u); /* team 1 (healthy) must appear - real reporting still works */

    printf("\n%s: %d check(s) failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
