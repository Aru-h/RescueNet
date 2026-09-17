/*
 * Standalone self-test for core/mission_link_override.c (no hardware
 * required - desktop gcc only, same pattern as every other selftest
 * in this project's laptop/tools/ directory). This is the "simulation mode
 * is fine, doesn't need real hardware" proof the acceptance criterion
 * asks for: it runs the REAL core simulator (generate_world(),
 * allocate_all_incidents() - the same functions laptop/
 * main.c and board/STM32N6570-DK/main_board.c both call every tick) and
 * checks g_incidents[].assigned_team directly, exactly the way the
 * acceptance criterion is phrased ("not just a toast message saying
 * it happened").
 *
 * The important test here is test_override_beats_a_real_competing_bid()
 * below - see its own comment for why a naive "does the pinned team
 * get *an* incident" test would be too weak to actually prove
 * anything: it deliberately manufactures a genuine adversarial
 * scenario (a DIFFERENT team that would win the incident naturally)
 * and proves the override changes the outcome away from that natural
 * winner, not just that nothing else happened to be competing.
 *
 * Note on incident timing: generate_world() does NOT populate any
 * incidents itself (initialize_incidents() only clears the pools) -
 * they're created lazily by generate_incidents_if_needed(), called
 * from simulation_step(), which sim_tick() only reaches AFTER
 * allocate_all_incidents() runs for that same tick. So immediately
 * after sim_start_seed() there are reliably zero incidents to bid on;
 * the test below advances g_current_tick and calls
 * generate_incidents_if_needed() directly (not the full sim_tick(),
 * to avoid also moving teams/changing stamina before the snapshot it
 * takes) until at least one incident exists, before taking its
 * baseline snapshot.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o override_selftest laptop/tools/override_selftest.c \
 *       core/allocator.c core/cbba.c core/comms_transport.c \
 *       core/environment.c core/fault_buffer.c core/fault_comms.c \
 *       core/fault_config.c core/fault_injector.c core/fault_predictor.c \
 *       core/grid.c core/incident.c core/lora_frame.c \
 *       core/lora_crypto.c core/aead_chacha20poly1305.c \
 *       core/chacha20.c core/poly1305.c \
 *       core/mission_execution_twin.c core/mission_link.c \
 *       core/mission_link_handshake.c core/mission_link_override.c \
 *       core/mission_state.c core/mission_state_utils.c core/path.c \
 *       core/random.c core/recovery_manager.c core/safezone.c \
 *       core/scoring.c core/search_zones.c core/sim_runner.c \
 *       core/simulation.c core/team.c core/test_metrics.c \
 *       core/watchdog.c laptop/debug.c \
 *       laptop/snapshot.c -lm
 * Run (from a scratch directory - it writes tron_snapshot_dataset.csv,
 * same as a normal run of the `tron` binary does):
 *   ./override_selftest
 */

#include "mission_state.h"
#include "mission_link_override.h"
#include "sim_runner.h"
#include "allocator.h"
#include "incident.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* ---------------------------------------------------------------
 * Codec round-trip, same shape as every other mission-link-family
 * selftest in this project (laptop/tools/mission_link_selftest.c,
 * laptop/tools/handshake_selftest.c) - proves the USART1 magic-byte channel
 * and the bare comms_transport/LoRa payload channel decode identical
 * MissionLinkOverride_t contents, since that's the thing most likely
 * to silently drift if the two ever stop sharing the same underlying
 * *_payload() functions.
 * ---------------------------------------------------------------
 */
static void test_codec_both_channels_agree(void)
{
    MissionLinkOverride_t original;
    uint8_t usart1_wire[MISSION_LINK_OVERRIDE_WIRE_SIZE];
    MissionLinkOverride_t via_usart1;
    uint8_t relay_payload[MISSION_LINK_OVERRIDE_PAYLOAD_SIZE];
    MissionLinkOverride_t via_relay;
    uint32_t packed_len;
    int rc;

    original.team_id = 4u;
    original.incident_id = 0xCAFEF00Du;

    packed_len = mission_link_override_pack(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_OVERRIDE_WIRE_SIZE);

    rc = mission_link_override_parse(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    mission_link_override_pack_payload(&original, relay_payload);
    mission_link_override_parse_payload(relay_payload, &via_relay);

    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkOverride_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkOverride_t)) == 0);
    CHECK(memcmp(&via_usart1, &via_relay, sizeof(MissionLinkOverride_t)) == 0);
}

static void test_wrong_magic_and_corrupt_checksum_rejected(void)
{
    MissionLinkOverride_t original;
    uint8_t wire[MISSION_LINK_OVERRIDE_WIRE_SIZE];
    MissionLinkOverride_t out;
    int rc;

    original.team_id = 1u;
    original.incident_id = 42u;
    mission_link_override_pack(&original, wire);

    /* Wrong magic. */
    uint8_t not_ours[MISSION_LINK_OVERRIDE_WIRE_SIZE];
    memcpy(not_ours, wire, sizeof(wire));
    not_ours[0] ^= 0xFFu;
    rc = mission_link_override_parse(not_ours, sizeof(not_ours), &out);
    CHECK(rc == -1);

    /* Corrupted checksum. */
    uint8_t corrupt[MISSION_LINK_OVERRIDE_WIRE_SIZE];
    memcpy(corrupt, wire, sizeof(wire));
    corrupt[2] ^= 0x01u;
    rc = mission_link_override_parse(corrupt, sizeof(corrupt), &out);
    CHECK(rc == -1);

    /* Incomplete buffer. */
    rc = mission_link_override_parse(wire, sizeof(wire) - 1u, &out);
    CHECK(rc == 0);
}

static void test_apply_sets_pin(void)
{
    MissionLinkOverride_t msg;
    msg.team_id = 2u;
    msg.incident_id = 999u;

    g_cbba_agent_state[2].override_active = 0u;
    g_cbba_agent_state[2].override_incident_id = 0u;

    mission_link_override_apply(&msg);

    CHECK(g_cbba_agent_state[2].override_active == 1u);
    CHECK(g_cbba_agent_state[2].override_incident_id == 999u);

    /* Out-of-range team_id is ignored, not a crash. */
    MissionLinkOverride_t bad;
    bad.team_id = (uint8_t)(NUM_TEAMS + 5);
    bad.incident_id = 1u;
    mission_link_override_apply(&bad); /* must not crash */
}

/* ---------------------------------------------------------------
 * THE acceptance test: proves the override beats a REAL competing
 * bid from a different team, using the actual core simulator
 * (generate_world + allocate_all_incidents - the same functions the
 * real board/laptop tick loop calls every tick).
 *
 * A weaker test - just checking the pinned team gets *some* incident
 * assigned - wouldn't actually prove the override does anything: if
 * nothing else was contesting that incident anyway, the "override"
 * would be indistinguishable from doing nothing. This test instead:
 *   1. Runs one real allocation pass on a freshly generated world and
 *      records whichever (incident, team) pairing CBBA produces
 *      naturally, with no override at all - a genuine competing claim
 *      from the real bidding process, not a hand-waved one.
 *   2. Rewinds all simulator state back to right after world
 *      generation (a plain memcpy of the relevant global arrays - see
 *      below), so scenario 2 starts from the exact same conditions
 *      scenario 1 did.
 *   3. Pins a DIFFERENT team onto that same incident and re-runs the
 *      same allocation pass.
 *   4. Confirms the incident now goes to the pinned team, not the
 *      team that would have (and, in scenario 1, actually did) win it
 *      naturally.
 * ---------------------------------------------------------------
 */
static void test_override_beats_a_real_competing_bid(void)
{
    if (sim_runner_init() != 0)
    {
        printf("FAIL: sim_runner_init() failed (equipment matrix validation)\n");
        g_failures++;
        return;
    }

    /* A handful of candidate seeds, in case a particular seed's
       freshly generated world happens to have no natural assignment
       at all on the very first allocation pass (e.g. no incident has
       been discovered yet) - try a few rather than depending on one
       specific seed's generation quirks. */
    const uint32_t candidate_seeds[] = {7u, 42u, 101u, 2024u, 55555u};
    const size_t num_candidate_seeds = sizeof(candidate_seeds) / sizeof(candidate_seeds[0]);

    for (size_t seed_index = 0; seed_index < num_candidate_seeds; seed_index++)
    {
        sim_start_seed(candidate_seeds[seed_index], FAULT_PROFILE_NONE);

        /* generate_world() (called by sim_start_seed()) does NOT
           populate any incidents itself - initialize_incidents() (see
           core/incident.c) only clears the incident/system-task
           pools. Actual incidents are created lazily by
           generate_incidents_if_needed(), called from
           simulation_step() (core/simulation.c), which sim_tick()
           only reaches AFTER allocate_all_incidents() runs for that
           same tick - so immediately after sim_start_seed() there are
           reliably zero incidents to bid on at all (confirmed via a
           throwaway diagnostic build during this test's own
           development: 0/NUM_INCIDENTS occupied slots right after
           generate_world()). Advance g_current_tick and call
           generate_incidents_if_needed() directly (not the full
           sim_tick(), which would also invoke fault_injector_step()/
           simulation_step() and start moving teams and changing
           stamina/equipment before the snapshot below - unwanted
           noise for a test that's about override precedence, not
           about tick-by-tick world evolution) until at least one
           incident exists. */
        for (uint32_t tick = 1u; tick <= 200u; tick++)
        {
            g_current_tick = tick;
            generate_incidents_if_needed();

            uint8_t any_occupied = 0u;
            for (uint8_t i = 0; i < NUM_INCIDENTS; i++)
            {
                if (g_incidents[i].slot_occupied)
                {
                    any_occupied = 1u;
                    break;
                }
            }
            if (any_occupied)
            {
                break;
            }
        }

        /* Force every generated incident visible immediately. A fresh
           world's incidents are revealed gradually by fog-of-war (see
           IncidentState_t.discovered / team_can_accept_incident()'s
           own check) as ticks advance, not all at once at generation
           time - this test is about override precedence, not about
           fog-of-war timing, so it sidesteps that by making every
           incident biddable from the start rather than advancing an
           arbitrary number of ticks first (which would also start
           moving teams around and changing stamina/equipment before
           the snapshot below, making the two scenarios harder to
           reason about identically). */
        for (uint8_t i = 0; i < NUM_INCIDENTS; i++)
        {
            if (g_incidents[i].slot_occupied)
            {
                g_incidents[i].discovered = 1u;
            }
        }

        /* Snapshot right after world generation, before any
           allocation has run - this is the common starting point
           both scenario 1 (baseline) and scenario 2 (overridden)
           replay from. */
        TeamState_t saved_teams[NUM_TEAMS];
        IncidentState_t saved_incidents[NUM_INCIDENTS];
        CBBAAgentState_t saved_cbba[NUM_TEAMS];
        memcpy(saved_teams, g_teams, sizeof(saved_teams));
        memcpy(saved_incidents, g_incidents, sizeof(saved_incidents));
        memcpy(saved_cbba, g_cbba_agent_state, sizeof(saved_cbba));

        /* Scenario 1: baseline, no override. */
        allocate_all_incidents();

        uint8_t natural_slot = 0xFFu;
        uint8_t natural_winner = 0xFFu;
        for (uint8_t i = 0; i < NUM_INCIDENTS; i++)
        {
            if (g_incidents[i].slot_occupied &&
                g_incidents[i].status == INC_ASSIGNED &&
                g_incidents[i].assigned_team != NO_ASSIGNMENT)
            {
                natural_slot = i;
                natural_winner = g_incidents[i].assigned_team;
                break;
            }
        }

        if (natural_slot == 0xFFu)
        {
            /* This seed's fresh world didn't produce any natural
               assignment on the first pass - try the next candidate
               seed rather than treating this as a failure of the
               override mechanism itself. */
            continue;
        }

        uint32_t pinned_incident_id = saved_incidents[natural_slot].incident_id;

        /* Pick a team to pin that is NOT the natural winner, and is
           AVAILABLE (allocate_all_incidents() only considers
           AVAILABLE/RESTING teams - see core/allocator.c). */
        uint8_t override_team = 0xFFu;
        for (uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            if (t != natural_winner && saved_teams[t].status == STATUS_AVAILABLE)
            {
                override_team = t;
                break;
            }
        }

        if (override_team == 0xFFu)
        {
            continue; /* try the next candidate seed */
        }

        /* Scenario 2: rewind to right after world generation, apply
           the override, replay the same allocation pass. */
        memcpy(g_teams, saved_teams, sizeof(saved_teams));
        memcpy(g_incidents, saved_incidents, sizeof(saved_incidents));
        memcpy(g_cbba_agent_state, saved_cbba, sizeof(saved_cbba));

        MissionLinkOverride_t msg;
        msg.team_id = override_team;
        msg.incident_id = pinned_incident_id;
        mission_link_override_apply(&msg);

        CHECK(g_cbba_agent_state[override_team].override_active == 1u);

        allocate_all_incidents();

        printf("  seed=%u: incident #%u naturally went to team %u; "
               "with team %u pinned, it went to team %u\n",
               candidate_seeds[seed_index], pinned_incident_id,
               natural_winner, override_team,
               g_incidents[natural_slot].assigned_team);

        CHECK(g_incidents[natural_slot].assigned_team == override_team);
        CHECK(g_incidents[natural_slot].assigned_team != natural_winner);
        CHECK(g_incidents[natural_slot].status == INC_ASSIGNED);

        /* The pin clears lazily, not synchronously within the same
           allocate_all_incidents() call that fulfilled it:
           cbba_apply_override_pin() (core/cbba.c) only re-evaluates
           "is this still actionable" the next time cbba_build_bundle()
           runs for this team, which is the NEXT tick's cbba_run() -
           within THIS tick, status only flips to INC_ASSIGNED in
           assign_incident(), called after cbba_run() (and therefore
           after every cbba_build_bundle() call this tick) has already
           finished. So: one more allocate_all_incidents() pass, same
           as the next real simulation tick would do, is what actually
           observes the now-INC_ASSIGNED status and clears the pin -
           see that function's own comment for why leaving it "active"
           until then is fine (nothing re-forces anything in the
           meantime; cbba_bundle_contains() already guards against a
           duplicate injection even before the pin formally clears). */
        allocate_all_incidents();

        CHECK(g_incidents[natural_slot].assigned_team == override_team);
        CHECK(g_cbba_agent_state[override_team].override_active == 0u);

        return; /* found a working seed and completed the real test - done */
    }

    printf("FAIL: none of the candidate seeds produced a usable natural "
           "assignment to build an adversarial test from\n");
    g_failures++;
}

int main(void)
{
    test_codec_both_channels_agree();
    test_wrong_magic_and_corrupt_checksum_rejected();
    test_apply_sets_pin();
    test_override_beats_a_real_competing_bid();

    if (g_failures == 0)
    {
        printf("override_selftest: all checks passed\n");
        return 0;
    }

    printf("override_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}
