/* city_generator_selftest: what a generated city must look like, for every world seed in [FIRST, LAST] (default 1-200).
 *
 * Each item below was a real defect of the generator (measured over seeds 1-100 before it was fixed, CHANGELOG):
 *
 *   footprints   no two buildings share a cell; every footprint cell carries its own building_id and no cell claims a
 *                building it is not inside. A building's footprint is its record's CENTRE (loc_x = x0 + w / 2, integer
 *                division) minus w / 2, h / 2 - the convention STATICENV's x / y uses too.
 *   access       no road cell inside a building (69% of buildings used to have a road carved through them: access was
 *                checked at the building's centre, inside it, and "repaired" from there).
 *   network      the roads are one network: no piece of 1-2 cells, and the largest piece holds at least 95% of road
 *                cells (there used to be a median of 34 pieces, 15 of them 1-2 cells).
 *   blobs        at most 20% of road cells in solid 2 x 2 blocks of road on any map, 12% in the median (was 48%).
 *   bridges      a map with a river has at least one crossing (4 rivers in 100 had none).
 *   districts    residential is the largest district on at least 90% of maps (was 6% of the city; medical was 20%).
 *   exits        the evacuation gateways are at least 10 cells apart on at least 95% of maps.
 *   reach        every exit can be reached from every safe zone (where teams start and rest), with the teams' own
 *                pathfinder, on the map as it stands after the earthquake. On the old cities 7 of 300 exits could
 *                not be reached from some starts (docs/CLAIMS.md C25).
 *   limits       building and navigation-node counts stay below their storage limits.
 *   seams        (first 30 seeds) sectors generated with the same seed at other world offsets (--world-offset) agree
 *                with this one: a sector shifted by half a map overlaps it, and the river must be identical, cell for
 *                cell, over the overlap (the course is a function of world position, not of the sector); and a road
 *                crosses the edge into the next sector on at least 90% of edges.
 *
 * Usage: city_generator_selftest [first_seed last_seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "environment.h"
#include "mission_state.h"
#include "path.h"
#include "simulation.h"

#define MAX_REPORTS 12

static unsigned g_reports, g_failures;

static void fail(unsigned seed, const char *what, int a, int b, int x, int y)
{
    g_failures++;
    if(g_reports++ < MAX_REPORTS)
        printf("FAIL seed %u: %s (%d, %d at %d,%d)\n", seed, what, a, b, x, y);
}

static int has_road_at(int x, int y)
{
    return x >= 0 && y >= 0 && x < GRID_WIDTH && y < GRID_HEIGHT && g_environment[x][y].road_class != 0;
}

static uint16_t g_owner[GRID_WIDTH][GRID_HEIGHT];
static uint8_t g_seen[GRID_WIDTH][GRID_HEIGHT];
static int g_queue[GRID_WIDTH * GRID_HEIGHT][2];

/* footprints and access */
static unsigned check_buildings(unsigned seed)
{
    uint16_t count = g_environment_summary.building_count;
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
            g_owner[x][y] = ENV_NO_BUILDING;

    for(uint16_t i = 0; i < count; i++)
    {
        const EnvironmentBuilding_t *b = &g_environment_buildings[i];
        int w = b->footprint_width, h = b->footprint_height, x0 = b->loc_x - w / 2, y0 = b->loc_y - h / 2;
        if(b->building_id != i)
            fail(seed, "building_id does not match its slot", i, b->building_id, b->loc_x, b->loc_y);
        if(w < 1 || h < 1 || b->footprint != (uint8_t)(w * h))
            fail(seed, "footprint != width * height", i, b->footprint, b->loc_x, b->loc_y);
        for(int x = x0; x < x0 + w; x++)
            for(int y = y0; y < y0 + h; y++)
            {
                if(x < 0 || y < 0 || x >= GRID_WIDTH || y >= GRID_HEIGHT)
                {
                    fail(seed, "footprint leaves the map", i, i, x, y);
                    continue;
                }
                if(g_owner[x][y] != ENV_NO_BUILDING)
                    fail(seed, "two footprints share a cell", i, g_owner[x][y], x, y);
                else
                    g_owner[x][y] = i;
                if(g_environment[x][y].building_id != i)
                    fail(seed, "footprint cell is owned by another id", i, g_environment[x][y].building_id, x, y);
                if(g_environment[x][y].road_class != 0)
                    fail(seed, "road cell inside a building", i, g_environment[x][y].road_class, x, y);
            }
    }
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            uint16_t id = g_environment[x][y].building_id;
            if(id != ENV_NO_BUILDING && (id >= count || g_owner[x][y] != id))
                fail(seed, "cell building_id outside that building's footprint", id, g_owner[x][y], x, y);
        }
    return count;
}

/* the road network: piece sizes; returns the share of road cells in solid 2 x 2 blocks (per cent) */
static int check_roads(unsigned seed)
{
    int total = 0, biggest = 0;
    memset(g_seen, 0, sizeof(g_seen));
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            if(!has_road_at(x, y) || g_seen[x][y])
                continue;
            int head = 0, tail = 0, size = 0;
            g_queue[tail][0] = x; g_queue[tail++][1] = y; g_seen[x][y] = 1;
            while(head < tail)
            {
                int cx = g_queue[head][0], cy = g_queue[head++][1];
                static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                size++;
                for(int k = 0; k < 4; k++)
                {
                    int nx = cx + d[k][0], ny = cy + d[k][1];
                    if(has_road_at(nx, ny) && !g_seen[nx][ny])
                    {
                        g_seen[nx][ny] = 1;
                        g_queue[tail][0] = nx; g_queue[tail++][1] = ny;
                    }
                }
            }
            total += size;
            if(size > biggest)
                biggest = size;
            if(size <= 2)
                fail(seed, "loose road piece of 1-2 cells", size, 0, x, y);
        }
    if(total && biggest * 100 < total * 95)
        fail(seed, "largest road piece under 95% of road cells", biggest, total, -1, -1);

    memset(g_seen, 0, sizeof(g_seen));
    int in_blocks = 0;
    for(int x = 0; x + 1 < GRID_WIDTH; x++)
        for(int y = 0; y + 1 < GRID_HEIGHT; y++)
            if(has_road_at(x, y) && has_road_at(x + 1, y) && has_road_at(x, y + 1) && has_road_at(x + 1, y + 1))
                for(int i = 0; i < 2; i++)
                    for(int j = 0; j < 2; j++)
                        if(!g_seen[x + i][y + j]) { g_seen[x + i][y + j] = 1; in_blocks++; }
    int pct = total ? in_blocks * 100 / total : 0;
    if(pct > 20)
        fail(seed, "over 20% of road cells in solid 2 x 2 blocks", pct, total, -1, -1);
    return pct;
}

/* a river has a crossing: returns 1 if the map has a river */
static int check_river(unsigned seed)
{
    int water = 0, crossings = 0;
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            water += g_environment[x][y].flood != 0;
            crossings += g_environment[x][y].flood && g_environment[x][y].road_class != 0;
        }
    if(water >= 40 && crossings == 0)
        fail(seed, "a river with no crossing", water, 0, -1, -1);
    return water >= 40;
}

static int residential_is_largest(void)
{
    int count[NUM_DISTRICT_TYPES] = {0};
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
            if(!g_environment[x][y].flood && g_environment[x][y].district < NUM_DISTRICT_TYPES)
                count[g_environment[x][y].district]++;
    for(int d = 0; d < NUM_DISTRICT_TYPES; d++)
        if(d != DISTRICT_RESIDENTIAL && count[d] >= count[DISTRICT_RESIDENTIAL])
            return 0;
    return 1;
}

static int exits_spaced(void)
{
    int16_t gx[NUM_EVACUATION_GATEWAYS], gy[NUM_EVACUATION_GATEWAYS];
    for(uint8_t i = 0; i < NUM_EVACUATION_GATEWAYS; i++)
        if(!environment_find_gateway_site(i, &gx[i], &gy[i]))
            return 0;
    for(int a = 0; a < NUM_EVACUATION_GATEWAYS; a++)
        for(int b = a + 1; b < NUM_EVACUATION_GATEWAYS; b++)
            if(abs(gx[a] - gx[b]) + abs(gy[a] - gy[b]) < 10)
                return 0;
    return 1;
}

/* reach: route from every safe zone to every exit; returns the number of (zone, exit) pairs with no route */
static unsigned check_reach(unsigned seed, unsigned *pairs)
{
    unsigned missing = 0;
    for(uint8_t z = 0; z < g_num_safe_zones; z++)
        for(uint8_t g = 0; g < NUM_EVACUATION_GATEWAYS; g++)
        {
            (*pairs)++;
            int zx = g_safezones[z].loc_x, zy = g_safezones[z].loc_y;
            int gx = g_evacuation_gateways[g].loc_x, gy = g_evacuation_gateways[g].loc_y;
            if(find_path_steps(zx, zy, gx, gy) == UINT16_MAX)
            {
                missing++;
                fail(seed, "an exit cannot be reached from a safe zone (zone, exit)", z, g, gx, gy);
            }
        }
    return missing;
}

/* seams: the edge of this sector against the next one over, same seed */
static uint8_t g_edge_water[2][GRID_WIDTH > GRID_HEIGHT ? GRID_WIDTH : GRID_HEIGHT];
static uint8_t g_edge_road[2][GRID_WIDTH > GRID_HEIGHT ? GRID_WIDTH : GRID_HEIGHT];
static void read_edge(int which, int east)
{
    int n = east ? GRID_HEIGHT : GRID_WIDTH;
    for(int i = 0; i < n; i++)
    {
        int x = east ? (which ? 0 : GRID_WIDTH - 1) : i, y = east ? i : (which ? 0 : GRID_HEIGHT - 1);
        g_edge_water[which][i] = g_environment[x][y].flood != 0;
        g_edge_road[which][i] = g_environment[x][y].road_class != 0;
    }
}
static uint8_t g_flood_home[GRID_WIDTH][GRID_HEIGHT];
static void check_seam(unsigned seed, int east, unsigned *edges, unsigned *edges_with_road)
{
    int n = east ? GRID_HEIGHT : GRID_WIDTH, roads = 0, miss = 0;
    int hx = east ? GRID_WIDTH / 2 : 0, hy = east ? 0 : GRID_HEIGHT / 2;   /* the half-map shift */

    environment_set_world_offset(0, 0);
    generate_environment(seed);
    read_edge(0, east);
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
            g_flood_home[x][y] = g_environment[x][y].flood != 0;

    environment_set_world_offset(hx, hy);   /* overlaps the home sector's far half */
    generate_environment(seed);
    for(int x = hx; x < GRID_WIDTH; x++)
        for(int y = hy; y < GRID_HEIGHT; y++)
            miss += g_flood_home[x][y] != (g_environment[x - hx][y - hy].flood != 0);
    if(miss)
        fail(seed, east ? "river differs where sectors overlap (east)" : "river differs where sectors overlap (south)", miss, 0, -1, -1);

    environment_set_world_offset(east ? GRID_WIDTH : 0, east ? 0 : GRID_HEIGHT);   /* the next sector over */
    generate_environment(seed);
    read_edge(1, east);
    environment_set_world_offset(0, 0);
    for(int i = 0; i < n; i++)
        roads += g_edge_road[0][i] && g_edge_road[1][i];
    (*edges)++;
    if(roads)
        (*edges_with_road)++;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv)
{
    unsigned first = 1, last = 200;
    if(argc == 3)
    {
        first = (unsigned)strtoul(argv[1], NULL, 10);
        last = (unsigned)strtoul(argv[2], NULL, 10);
    }
    else if(argc != 1)
    {
        fprintf(stderr, "usage: %s [first_seed last_seed]\n", argv[0]);
        return 2;
    }
    if(last < first || last - first > 9999)
        return 2;

    unsigned reach_pairs = 0, reach_missing = 0, reach_maps_bad = 0;
    unsigned maps = 0, rivers = 0, residential = 0, spaced = 0, edges = 0, edges_road = 0;
    unsigned long buildings = 0;
    int *blob = calloc(last - first + 1, sizeof(int));
    g_simulation_trace_enabled = 0;
    for(unsigned seed = first; seed <= last; seed++)
    {
        environment_set_world_offset(0, 0);
        generate_world(seed);
        buildings += check_buildings(seed);
        blob[maps] = check_roads(seed);
        rivers += (unsigned)check_river(seed);
        residential += (unsigned)residential_is_largest();
        spaced += (unsigned)exits_spaced();
        {
            unsigned m = check_reach(seed, &reach_pairs);
            reach_missing += m;
            reach_maps_bad += m != 0;
        }
        if(g_environment_summary.building_count >= MAX_ENV_BUILDINGS)
            fail(seed, "building storage full", g_environment_summary.building_count, MAX_ENV_BUILDINGS, -1, -1);
        if(g_environment_summary.nav_node_count >= MAX_ENV_NAV_NODES)
            fail(seed, "navigation node storage full", g_environment_summary.nav_node_count, MAX_ENV_NAV_NODES, -1, -1);
        maps++;
    }
    for(unsigned seed = first; seed <= last && seed < first + 30; seed++)
    {
        check_seam(seed, 1, &edges, &edges_road);
        check_seam(seed, 0, &edges, &edges_road);
    }
    qsort(blob, maps, sizeof(int), cmp_int);
    int blob_median = blob[maps / 2];
    free(blob);

    if(blob_median > 12)
        fail(0, "median share of road cells in solid 2 x 2 blocks over 12%", blob_median, 12, -1, -1);
    if(residential * 100 < maps * 90)
        fail(0, "residential is the largest district on under 90% of maps", (int)residential, (int)maps, -1, -1);
    if(spaced * 100 < maps * 95)
        fail(0, "exits at least 10 cells apart on under 95% of maps", (int)spaced, (int)maps, -1, -1);
    if(edges && edges_road * 100 < edges * 90)
        fail(0, "a road crosses under 90% of sector edges", (int)edges_road, (int)edges, -1, -1);

    printf("seeds %u-%u: %u maps, %lu buildings, %u with a river; road cells in 2 x 2 blocks: median %d%%; "
           "residential largest on %u; exits spaced on %u; sector edges checked %u, crossed by a road %u\n",
           first, last, maps, buildings, rivers, blob_median, residential, spaced, edges, edges_road);
    printf("reach: %u safe zone to exit routes checked, %u with no route, on %u maps\n",
           reach_pairs, reach_missing, reach_maps_bad);
    if(g_failures)
    {
        printf("FAIL: %u problems\n", g_failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
