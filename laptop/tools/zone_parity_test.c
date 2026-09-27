/*
 * Zone parity test (feature-contract Decision D): checks that a board fed
 * only mission_link.h's district-map and zone-trust frames (sections
 * 5e/5f) computes exactly the same zone_id_for_location()/search_zone_
 * trust() answers the laptop (the world authority) does.
 *
 * zone_id_for_location() and search_zone_trust() are pure functions of
 * (x, y) / (district_id, tick) plus whatever's in g_environment[][]/
 * g_search_zones[] - there is no need to run a full second board
 * process (laptop/tools/two_board_mission_harness.c's job, for the
 * whole mission link) just to check these two functions agree. Instead
 * this one source file builds into two binaries, selected by the same
 * ENVIRONMENT_HAS_WORLD_STORAGE macro that gates the real board port:
 *
 *   "gen" build (default, ENVIRONMENT_HAS_WORLD_STORAGE=1): runs a real
 *   seeded mission, and for every tick writes (a) the ground-truth
 *   zone_id_for_location()/search_zone_trust() values to truth.csv and
 *   (b) the exact wire frames a real board would receive to frames.txt.
 *
 *   "check" build (-DENVIRONMENT_HAS_WORLD_STORAGE=0, same board port
 *   real firmware uses): never generates a world - reads frames.txt,
 *   applies each frame via the same mission_link_apply_district_row()/
 *   mission_link_apply_zone_trust() a real board calls, and compares
 *   zone_id_for_location()/search_zone_trust() against truth.csv.
 *
 * usage:
 *   zone_parity_gen   <frames_path> <truth_path>
 *   zone_parity_check <frames_path> <truth_path>
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "search_zones.h"
#include "environment.h"
#include "fault_config.h"
#include "sim_runner.h"

#define ZONE_PARITY_SEED 7u
#define ZONE_PARITY_TICKS 400u
#define ZONE_PARITY_MAX_WIRE 64u

#if ENVIRONMENT_HAS_WORLD_STORAGE

static void hex_encode(const uint8_t *buf, uint32_t len, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    uint32_t i;

    for(i = 0; i < len; i++)
    {
        out[2u * i] = hexd[(buf[i] >> 4) & 0x0Fu];
        out[2u * i + 1u] = hexd[buf[i] & 0x0Fu];
    }
    out[2u * len] = '\0';
}

/* Ground-truth + frame-stream generator. Mirrors laptop/mission_feed.c's
   own district/zone-trust frame construction exactly (same helper calls),
   deliberately not a second, independent reimplementation of the wire
   format - a bug shared between the two would otherwise slip past this
   test. */
static int run_gen(const char *frames_path, const char *truth_path)
{
    FILE *ff = fopen(frames_path, "w");
    FILE *ft = fopen(truth_path, "w");
    uint8_t wire[ZONE_PARITY_MAX_WIRE];
    char hex[2u * ZONE_PARITY_MAX_WIRE + 1u];
    uint8_t x, y, z;
    uint32_t t;

    if(ff == NULL || ft == NULL)
    {
        fprintf(stderr, "zone_parity_test: cannot open output files\n");
        return 1;
    }

    if(sim_runner_init() != 0)
    {
        fprintf(stderr, "zone_parity_test: sim_runner_init failed\n");
        return 1;
    }
    sim_start_seed(ZONE_PARITY_SEED, FAULT_PROFILE_RANDOM);

    fprintf(ft, "DISTRICT,%u,%u,0\n", (unsigned)GRID_WIDTH, (unsigned)GRID_HEIGHT);
    for(y = 0u; y < MISSION_LINK_MAP_HEIGHT; y++)
    {
        MissionLinkDistrictRow_t row;
        uint32_t n;

        row.row = y;
        for(x = 0u; x < MISSION_LINK_MAP_WIDTH; x += 2u)
        {
            uint8_t d_lo = zone_id_for_location((int16_t)x, (int16_t)y);
            uint8_t d_hi = zone_id_for_location((int16_t)(x + 1u), (int16_t)y);

            row.cells[x / 2u] = mission_link_district_encode_byte(d_lo, d_hi);
            fprintf(ft, "D,%u,%u,%u\n", (unsigned)x, (unsigned)y, (unsigned)d_lo);
            fprintf(ft, "D,%u,%u,%u\n", (unsigned)(x + 1u), (unsigned)y, (unsigned)d_hi);
        }
        n = mission_link_pack_district_row(&row, wire);
        hex_encode(wire, n, hex);
        fprintf(ff, "DISTRICT %s\n", hex);
    }

    for(t = 1u; t <= ZONE_PARITY_TICKS; t++)
    {
        MissionLinkZoneTrust_t zt;
        uint32_t n;

        sim_tick();

        for(z = 0u; z < NUM_SEARCH_ZONES; z++)
        {
            zt.last_swept_tick[z] = g_search_zones[z].last_swept_tick;
            zt.aftershock_hits_since_swept[z] = (uint8_t)g_search_zones[z].aftershock_hits_since_swept;
            fprintf(ft, "T,%u,%u,%u\n", t, (unsigned)z, (unsigned)search_zone_trust(z, t));
        }
        n = mission_link_pack_zone_trust(&zt, wire);
        hex_encode(wire, n, hex);
        fprintf(ff, "TICK %u\n", t);
        fprintf(ff, "ZONETRUST %s\n", hex);
    }

    fclose(ff);
    fclose(ft);
    printf("zone_parity_test (gen): wrote %s and %s (%u ticks, seed %u)\n",
           frames_path, truth_path, ZONE_PARITY_TICKS, ZONE_PARITY_SEED);
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

/* This build never calls sim_tick()/generate_environment(), but still
   links every CORE_SOURCES file (same set the real board port links),
   and a few of them call these trace/debug hooks unconditionally
   behind g_simulation_trace_enabled (which this build leaves at 0).
   laptop/debug.c itself can't be linked here - it reads g_environment
   directly, which does not exist in this build - so these are no-op
   stubs, not a laptop/debug.c substitute. g_simulation_trace_enabled
   itself is core/simulation.c's own global (defaulted to 1 there) -
   not redefined here, just never consulted by these no-ops. */

void debug_trace(const char *fmt, ...)
{
    (void)fmt;
}

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

/* Ground truth, loaded once: per-cell district id and, per (tick, zone),
   the expected trust. Small enough (1296 + 10*ZONE_PARITY_TICKS rows) to
   just hold in memory rather than re-reading the file for every query. */
static uint8_t g_truth_district[GRID_WIDTH][GRID_HEIGHT];
static uint8_t g_truth_district_loaded[GRID_WIDTH][GRID_HEIGHT];

#define ZONE_PARITY_MAX_TRUST_ROWS (ZONE_PARITY_TICKS * NUM_SEARCH_ZONES + 16u)
static uint32_t g_truth_trust_tick[ZONE_PARITY_MAX_TRUST_ROWS];
static uint8_t g_truth_trust_zone[ZONE_PARITY_MAX_TRUST_ROWS];
static uint8_t g_truth_trust_value[ZONE_PARITY_MAX_TRUST_ROWS];
static uint32_t g_truth_trust_rows;

static int load_truth(const char *truth_path)
{
    FILE *ft = fopen(truth_path, "r");
    char line[128];

    if(ft == NULL)
    {
        fprintf(stderr, "zone_parity_test: cannot open %s\n", truth_path);
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
               g_truth_trust_rows < ZONE_PARITY_MAX_TRUST_ROWS)
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

static int run_check(const char *frames_path, const char *truth_path)
{
    FILE *ff;
    char line[256];
    uint32_t current_tick = 0u;
    uint32_t district_checked = 0u, district_mismatches = 0u;
    uint32_t trust_checked = 0u, trust_mismatches = 0u;
    int16_t x, y;

    if(load_truth(truth_path) != 0)
        return 1;

    ff = fopen(frames_path, "r");
    if(ff == NULL)
    {
        fprintf(stderr, "zone_parity_test: cannot open %s\n", frames_path);
        return 1;
    }

    while(fgets(line, sizeof(line), ff) != NULL)
    {
        if(strncmp(line, "DISTRICT ", 9) == 0)
        {
            uint8_t wire[ZONE_PARITY_MAX_WIRE];
            MissionLinkDistrictRow_t row;

            (void)hex_decode(line + 9, wire, sizeof(wire));
            if(mission_link_parse_district_row(wire, MISSION_LINK_DISTRICT_ROW_WIRE_SIZE, &row) == 1)
            {
                (void)mission_link_apply_district_row(&row);
            }
        }
        else if(strncmp(line, "TICK ", 5) == 0)
        {
            current_tick = (uint32_t)strtoul(line + 5, NULL, 10);
        }
        else if(strncmp(line, "ZONETRUST ", 10) == 0)
        {
            uint8_t wire[ZONE_PARITY_MAX_WIRE];
            MissionLinkZoneTrust_t zt;
            uint8_t z;

            (void)hex_decode(line + 10, wire, sizeof(wire));
            if(mission_link_parse_zone_trust(wire, MISSION_LINK_ZONE_TRUST_WIRE_SIZE, &zt) != 1)
                continue;
            mission_link_apply_zone_trust(&zt);

            for(z = 0u; z < NUM_SEARCH_ZONES; z++)
            {
                uint8_t expected;
                uint8_t got = search_zone_trust(z, current_tick);

                if(!lookup_truth_trust(current_tick, z, &expected))
                    continue;

                trust_checked++;
                if(got != expected)
                {
                    trust_mismatches++;
                    if(trust_mismatches <= 5u)
                        fprintf(stderr, "trust mismatch: tick=%u zone=%u board=%u laptop=%u\n",
                                current_tick, (unsigned)z, (unsigned)got, (unsigned)expected);
                }
            }
        }
    }
    fclose(ff);

    /* District check runs after every DISTRICT frame is applied - order
       within frames.txt doesn't matter for a fixed-after-world-gen map. */
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
                    fprintf(stderr, "district mismatch: x=%d y=%d board=%u laptop=%u\n",
                            x, y, (unsigned)got, (unsigned)expected);
            }
        }
    }

    printf("zone_parity_test (check): district %u/%u match, trust %u/%u match\n",
           district_checked - district_mismatches, district_checked,
           trust_checked - trust_mismatches, trust_checked);

    if(district_mismatches > 0u || trust_mismatches > 0u || district_checked == 0u || trust_checked == 0u)
    {
        printf("zone_parity_test: FAIL\n");
        return 1;
    }
    printf("zone_parity_test: PASS\n");
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
