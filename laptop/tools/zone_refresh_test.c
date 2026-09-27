/*
 * Zone refresh convergence test (feature-contract Decision D follow-up):
 * a board that misses mission_feed_begin()'s initial district-map/zone-
 * trust burst (late join, reboot, dropped frames) must still converge to
 * the laptop's zone_id_for_location()/search_zone_trust() answers within
 * one MISSION_LINK_ZONE_TRUST_REFRESH_TICKS period, via mission_feed_
 * tick()'s periodic full resend of both frame types - not just the
 * change-only diff (district ids never change after world generation,
 * so the diff alone would never re-fire for a board that missed the
 * burst - see mission_link.h section 5e's own doc).
 *
 * laptop/tools/zone_parity_test.c does NOT cover this path: it only
 * checks that the initial burst decodes correctly, so the periodic-
 * refresh code could be (and briefly was, during this feature's own
 * development - mission_feed_tick() never called mission_feed_emit_
 * zone_trust() at all) entirely dead and still pass that test.
 *
 * Same two-binaries-from-one-source-file design as zone_parity_test.c:
 *
 *   "gen" (ENVIRONMENT_HAS_WORLD_STORAGE=1): runs a real seeded mission
 *   through the REAL laptop/mission_feed.c scheduling functions -
 *   mission_feed_begin() once, then mission_feed_tick() every tick,
 *   exactly as laptop/main.c's own --mission-feed loop does - capturing
 *   every MLFRAME line mission_feed_emit() prints to frames_path (via
 *   stdout redirection) and the ground-truth zone_id_for_location()/
 *   search_zone_trust() answers to truth_path.
 *
 *   "check" (-DENVIRONMENT_HAS_WORLD_STORAGE=0, the real board port):
 *   reads frames_path, but DROPS every district-row/zone-trust frame
 *   tagged tick 0 - simulating a board that missed mission_feed_begin()'s
 *   burst entirely - applying only what mission_feed_tick() sends from
 *   tick 1 onward. Confirms zone_id_for_location()/search_zone_trust()
 *   still converge to truth within MISSION_LINK_ZONE_TRUST_REFRESH_TICKS.
 *
 * usage:
 *   zone_refresh_gen   <frames_path> <truth_path>
 *   zone_refresh_check <frames_path> <truth_path>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_feed.h"
#include "search_zones.h"
#include "environment.h"
#include "fault_config.h"
#include "sim_runner.h"
#include "simulation.h"

#define ZONE_REFRESH_SEED 11u
#define ZONE_REFRESH_TICKS (2u * MISSION_LINK_ZONE_TRUST_REFRESH_TICKS + 10u)
/* The tick at which the dropped-burst board must have converged: one
   full refresh period after tick 0, plus a small margin for the periodic
   resend's own scheduling. */
#define ZONE_REFRESH_CHECKPOINT_TICK (MISSION_LINK_ZONE_TRUST_REFRESH_TICKS + 5u)
#define ZONE_REFRESH_MAX_LINE 256u

#if ENVIRONMENT_HAS_WORLD_STORAGE

/* Ground-truth generator, using the REAL mission_feed.c scheduling
   functions (not a hand-rolled reimplementation) so a bug shared between
   this test and mission_feed.c's own frame construction can't slip past
   it either. */
static int run_gen(const char *frames_path, const char *truth_path)
{
    FILE *ft = fopen(truth_path, "w");
    uint8_t x, y, z;
    uint32_t t;

    if(ft == NULL)
    {
        fprintf(stderr, "zone_refresh_test: cannot open %s\n", truth_path);
        return 1;
    }

    g_simulation_trace_enabled = 0u; /* keep frames_path free of debug_trace() noise */

    if(sim_runner_init() != 0)
    {
        fprintf(stderr, "zone_refresh_test: sim_runner_init failed\n");
        return 1;
    }
    sim_start_seed(ZONE_REFRESH_SEED, FAULT_PROFILE_RANDOM);

    for(y = 0u; y < GRID_HEIGHT; y++)
        for(x = 0u; x < GRID_WIDTH; x++)
            fprintf(ft, "D,%u,%u,%u\n", (unsigned)x, (unsigned)y,
                    (unsigned)zone_id_for_location((int16_t)x, (int16_t)y));

    /* Every MLFRAME line mission_feed_begin()/mission_feed_tick() print
       from here on goes to frames_path, not the real stdout. */
    if(freopen(frames_path, "w", stdout) == NULL)
    {
        fprintf(stderr, "zone_refresh_test: cannot open %s\n", frames_path);
        return 1;
    }

    mission_feed_begin(); /* the tick-0 burst run_check() will drop */

    for(t = 1u; t <= ZONE_REFRESH_TICKS; t++)
    {
        sim_tick();
        mission_feed_tick(t);

        for(z = 0u; z < NUM_SEARCH_ZONES; z++)
            fprintf(ft, "T,%u,%u,%u\n", t, (unsigned)z, (unsigned)search_zone_trust(z, t));
    }

    fflush(stdout);
    fclose(ft);
    fprintf(stderr, "zone_refresh_test (gen): wrote %s and %s (%u ticks, seed %u)\n",
            frames_path, truth_path, ZONE_REFRESH_TICKS, ZONE_REFRESH_SEED);
    return 0;
}

int main(int argc, char **argv)
{
    if(argc != 3)
    {
        fprintf(stderr, "usage: %s <frames_path> <truth_path>\n", argv[0]);
        return 1;
    }
    return run_gen(argv[1], argv[2]);
}

#else /* !ENVIRONMENT_HAS_WORLD_STORAGE - board build */

/* Same no-op link-satisfying stubs as zone_parity_test.c - see that
   file's own comment. Not redefining g_simulation_trace_enabled itself
   (core/simulation.c's global) - just declared extern above so the gen
   side can silence it; this side never ticks the simulation at all. */
void debug_trace(const char *fmt, ...) { (void)fmt; }

void debug_print_generator_selection(const uint32_t *weights, uint32_t weight_count,
    uint32_t total_weight, uint32_t type_roll, uint8_t selected_type)
{
    (void)weights; (void)weight_count; (void)total_weight; (void)type_roll; (void)selected_type;
}

void debug_print_generator_evaluation(uint32_t tick, uint8_t load, uint32_t probability,
    uint32_t roll, uint8_t spawned, uint8_t available_teams, uint8_t en_route_teams,
    uint8_t on_scene_teams, uint8_t resting_teams, uint8_t open_incidents,
    uint8_t assigned_incidents, uint8_t in_progress_incidents, uint8_t active_system_tasks,
    uint16_t degraded_cells, uint32_t next_evaluation_tick)
{
    (void)tick; (void)load; (void)probability; (void)roll; (void)spawned;
    (void)available_teams; (void)en_route_teams; (void)on_scene_teams; (void)resting_teams;
    (void)open_incidents; (void)assigned_incidents; (void)in_progress_incidents;
    (void)active_system_tasks; (void)degraded_cells; (void)next_evaluation_tick;
}

static uint32_t hex_decode(const char *in, uint8_t *out, uint32_t max_len)
{
    uint32_t n = 0;

    while(in[0] != '\0' && in[0] != '\n' && in[1] != '\0' && n < max_len)
    {
        unsigned v = 0;
        if(sscanf(in, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
        in += 2;
    }
    return n;
}

static uint8_t g_truth_district[GRID_WIDTH][GRID_HEIGHT];
static uint8_t g_truth_district_loaded[GRID_WIDTH][GRID_HEIGHT];

#define ZONE_REFRESH_MAX_TRUST_ROWS (ZONE_REFRESH_TICKS * NUM_SEARCH_ZONES + 16u)
static uint32_t g_truth_trust_tick[ZONE_REFRESH_MAX_TRUST_ROWS];
static uint8_t g_truth_trust_zone[ZONE_REFRESH_MAX_TRUST_ROWS];
static uint8_t g_truth_trust_value[ZONE_REFRESH_MAX_TRUST_ROWS];
static uint32_t g_truth_trust_rows;

static int load_truth(const char *truth_path)
{
    FILE *ft = fopen(truth_path, "r");
    char line[128];

    if(ft == NULL)
    {
        fprintf(stderr, "zone_refresh_test: cannot open %s\n", truth_path);
        return 1;
    }

    while(fgets(line, sizeof(line), ft) != NULL)
    {
        if(line[0] == 'D' && line[1] == ',')
        {
            unsigned x, y, d;
            if(sscanf(line + 2, "%u,%u,%u", &x, &y, &d) == 3 &&
               x < GRID_WIDTH && y < GRID_HEIGHT)
            {
                g_truth_district[x][y] = (uint8_t)d;
                g_truth_district_loaded[x][y] = 1u;
            }
        }
        else if(line[0] == 'T' && line[1] == ',')
        {
            unsigned tick, zone, trust;
            if(sscanf(line + 2, "%u,%u,%u", &tick, &zone, &trust) == 3 &&
               g_truth_trust_rows < ZONE_REFRESH_MAX_TRUST_ROWS)
            {
                g_truth_trust_tick[g_truth_trust_rows] = tick;
                g_truth_trust_zone[g_truth_trust_rows] = (uint8_t)zone;
                g_truth_trust_value[g_truth_trust_rows] = (uint8_t)trust;
                g_truth_trust_rows++;
            }
        }
    }
    fclose(ft);
    return 0;
}

static int lookup_truth_trust(uint32_t tick, uint8_t zone, uint8_t *out)
{
    uint32_t i;

    for(i = 0u; i < g_truth_trust_rows; i++)
    {
        if(g_truth_trust_tick[i] == tick && g_truth_trust_zone[i] == zone)
        {
            *out = g_truth_trust_value[i];
            return 1;
        }
    }
    return 0;
}

/* Checks every district cell and every zone's trust against truth at
   check_tick, printing the same style of mismatch as zone_parity_test.c.
   Returns 1 if district and trust both fully match. */
static int check_convergence(const char *label, uint32_t check_tick)
{
    uint32_t district_checked = 0u, district_mismatches = 0u;
    uint32_t trust_checked = 0u, trust_mismatches = 0u;
    int16_t x, y;
    uint8_t z;

    for(x = 0; x < GRID_WIDTH; x++)
    {
        for(y = 0; y < GRID_HEIGHT; y++)
        {
            uint8_t got, expected;

            if(!g_truth_district_loaded[x][y])
                continue;

            got = zone_id_for_location(x, y);
            expected = g_truth_district[x][y];
            district_checked++;
            if(got != expected)
            {
                district_mismatches++;
                if(district_mismatches <= 5u)
                    fprintf(stderr, "%s: district mismatch: x=%d y=%d board=%u laptop=%u\n",
                            label, x, y, (unsigned)got, (unsigned)expected);
            }
        }
    }

    for(z = 0u; z < NUM_SEARCH_ZONES; z++)
    {
        uint8_t expected, got;

        if(!lookup_truth_trust(check_tick, z, &expected))
            continue;

        got = search_zone_trust(z, check_tick);
        trust_checked++;
        if(got != expected)
        {
            trust_mismatches++;
            if(trust_mismatches <= 5u)
                fprintf(stderr, "%s: trust mismatch: tick=%u zone=%u board=%u laptop=%u\n",
                        label, check_tick, (unsigned)z, (unsigned)got, (unsigned)expected);
        }
    }

    printf("zone_refresh_test (%s, tick %u): district %u/%u match, trust %u/%u match\n",
           label, check_tick, district_checked - district_mismatches, district_checked,
           trust_checked - trust_mismatches, trust_checked);

    return district_mismatches == 0u && trust_mismatches == 0u &&
           district_checked > 0u && trust_checked > 0u;
}

static int run_check(const char *frames_path, const char *truth_path)
{
    FILE *ff;
    char line[ZONE_REFRESH_MAX_LINE];
    uint32_t dropped_burst_frames = 0u, applied_frames = 0u;
    int checkpoint_ok = 0, checkpoint_done = 0, final_ok;

    if(load_truth(truth_path) != 0)
        return 1;

    ff = fopen(frames_path, "r");
    if(ff == NULL)
    {
        fprintf(stderr, "zone_refresh_test: cannot open %s\n", frames_path);
        return 1;
    }

    while(fgets(line, sizeof(line), ff) != NULL)
    {
        unsigned tick;
        int hex_offset;
        uint8_t wire[ZONE_REFRESH_MAX_LINE / 2u];
        uint32_t n;

        if(strncmp(line, "MLFRAME ", 8) != 0)
            continue; /* not a mission-feed frame line at all */

        if(sscanf(line + 8, "%u%n", &tick, &hex_offset) != 1)
            continue;

        /* Frames within one mission_feed_tick() call are written together
           before the next tick's call begins, so seeing a frame tagged
           past the checkpoint tick means every checkpoint-tick frame has
           already been applied above - the right moment to snapshot
           board-side state and confirm it converged BY the checkpoint,
           not merely by the end of the run. */
        if(!checkpoint_done && tick > ZONE_REFRESH_CHECKPOINT_TICK)
        {
            checkpoint_ok = check_convergence("checkpoint", ZONE_REFRESH_CHECKPOINT_TICK);
            checkpoint_done = 1;
        }

        n = hex_decode(line + 8 + hex_offset + 1, wire, sizeof(wire));
        if(n < 2u)
            continue;

        if(wire[0] == MISSION_LINK_DISTRICT_ROW_MAGIC0 && wire[1] == MISSION_LINK_DISTRICT_ROW_MAGIC1)
        {
            MissionLinkDistrictRow_t row;

            if(tick == 0u)
            {
                dropped_burst_frames++;
                continue; /* simulate a board that missed the initial burst */
            }
            if(n == MISSION_LINK_DISTRICT_ROW_WIRE_SIZE &&
               mission_link_parse_district_row(wire, n, &row) == 1)
            {
                (void)mission_link_apply_district_row(&row);
                applied_frames++;
            }
        }
        else if(wire[0] == MISSION_LINK_ZONE_TRUST_MAGIC0 && wire[1] == MISSION_LINK_ZONE_TRUST_MAGIC1)
        {
            MissionLinkZoneTrust_t zt;

            if(tick == 0u)
            {
                dropped_burst_frames++;
                continue; /* simulate a board that missed the initial burst */
            }
            if(n == MISSION_LINK_ZONE_TRUST_WIRE_SIZE &&
               mission_link_parse_zone_trust(wire, n, &zt) == 1)
            {
                mission_link_apply_zone_trust(&zt);
                applied_frames++;
            }
        }
        /* else: some other frame type (roster/incident/fault/sites/map
           row/clock) this test doesn't care about. */
    }
    fclose(ff);

    if(!checkpoint_done)
    {
        /* frames_path ended before any frame past the checkpoint tick -
           ZONE_REFRESH_TICKS is smaller than the checkpoint, which would
           be a test bug, not a product one. */
        fprintf(stderr, "zone_refresh_test: FAIL - frames_path never reached the "
                        "checkpoint tick (%u) - ZONE_REFRESH_TICKS is too small\n",
                        (unsigned)ZONE_REFRESH_CHECKPOINT_TICK);
        return 1;
    }

    printf("zone_refresh_test: dropped %u tick-0 frame(s) (simulated missed burst), "
           "applied %u frame(s) from tick 1 onward\n",
           dropped_burst_frames, applied_frames);

    if(dropped_burst_frames == 0u)
    {
        fprintf(stderr, "zone_refresh_test: FAIL - no tick-0 burst frames found to drop; "
                        "this test is not exercising anything\n");
        return 1;
    }

    /* checkpoint_ok was captured mid-stream, above, right after the
       checkpoint tick's own frames were applied and before any later
       tick's frames could still be pending - it reflects convergence BY
       that tick, not just by the end of the run. final_ok is a sanity
       check that nothing regresses afterward. */
    final_ok = check_convergence("final", ZONE_REFRESH_TICKS);

    if(!checkpoint_ok || !final_ok)
    {
        printf("zone_refresh_test: FAIL\n");
        return 1;
    }
    printf("zone_refresh_test: PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    if(argc != 3)
    {
        fprintf(stderr, "usage: %s <frames_path> <truth_path>\n", argv[0]);
        return 1;
    }
    return run_check(argv[1], argv[2]);
}

#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */
