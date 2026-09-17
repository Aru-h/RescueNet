#include "environment.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "random.h"
#include "search_zones.h"
#include "mission_state.h"   /* g_safezones / g_incidents, for post-quake reachability repair */

typedef struct {
    uint16_t district_id;
    uint8_t type;
    int16_t center_x;
    int16_t center_y;
    uint8_t population_density;
    uint8_t building_density;
    uint8_t road_density;
    uint8_t construction_quality;
    uint8_t soil_condition;
    uint8_t seismic_vulnerability;
    uint8_t critical_infrastructure_density;
} DistrictRecord_t;

typedef struct {
    int16_t x;
    int16_t y;
} GridPoint_t;

typedef struct {
    uint8_t valid;
    int16_t x;
    int16_t y;
} GatewaySite_t;

typedef struct {
    uint8_t open;
    uint16_t cost;
    uint16_t parent_x;
    uint16_t parent_y;
} PathNode_t;

enum {
    NAV_NODE_INTERSECTION = 1,
    NAV_NODE_BUILDING_ENTRANCE,
    NAV_NODE_SAFE_ZONE,
    NAV_NODE_GATEWAY,
    NAV_NODE_BRIDGE
};

#if ENVIRONMENT_HAS_WORLD_STORAGE
/* Promoted from stack to BSS to eliminate the ~11.7 KB stack frame in
 * carve_path_between(). Called only from stage_generate_roads() which
 * is single-threaded and non-reentrant, so a file-scope static is safe. */
static PathNode_t g_carve_nodes[GRID_WIDTH][GRID_HEIGHT];
static uint8_t    g_carve_visited[GRID_WIDTH][GRID_HEIGHT];

/* Same promotion as g_carve_nodes/g_carve_visited above, for the same
 * reason: road_reachable_map()'s BFS queue was a ~5.2 KB stack-local
 * array, called repeatedly from stage_generate_infrastructure(),
 * stage_validate_and_build_navigation(), and environment_validate_
 * hazard_reachability() - all single-threaded, non-reentrant callers
 * (confirmed: exactly one simulation task exists on either board
 * target), so a file-scope static is safe here too. */
static GridPoint_t g_road_reachable_queue[GRID_WIDTH * GRID_HEIGHT];
EnvironmentCell_t g_environment[GRID_WIDTH][GRID_HEIGHT];
EnvironmentBuilding_t g_environment_buildings[MAX_ENV_BUILDINGS];
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */
EnvironmentSummary_t g_environment_summary;
HazardPropagationConfig_t g_hazard_config =
{
    .fire_spread_enabled = 1,
    .gas_escalation_enabled = 1,
    .aftershock_enabled = 1,
    .road_degradation_enabled = 1,
    .fire_spread_interval_ticks = 25,
    .gas_escalation_interval_ticks = 40,
    .aftershock_interval_ticks = 80,
    .road_degradation_interval_ticks = 50,
    .fire_spread_probability = 18,
    .gas_explosion_probability = 12,
    .aftershock_collapse_probability = 10,
    .road_degradation_amount = 1
};

static EnvironmentHazardEvent_t g_pending_hazard_events[MAX_ENV_PENDING_HAZARDS];
static DistrictRecord_t g_districts[NUM_DISTRICT_TYPES];

uint8_t environment_get_district_center(uint8_t district_id, int16_t *x, int16_t *y)
{
    if(district_id >= NUM_DISTRICT_TYPES || x == NULL || y == NULL)
        return 0;

    *x = g_districts[district_id].center_x;
    *y = g_districts[district_id].center_y;
    return 1;
}
#if ENVIRONMENT_HAS_WORLD_STORAGE
static EnvironmentNavNode_t g_nav_nodes[MAX_ENV_NAV_NODES];
static EnvironmentNavEdge_t g_nav_edges[MAX_ENV_NAV_EDGES];
#endif
static GatewaySite_t g_gateway_sites[NUM_EVACUATION_GATEWAYS];

#if ENVIRONMENT_HAS_WORLD_STORAGE
static uint8_t g_coast_orientation = 0;
static int16_t g_river_center[GRID_WIDTH];
static uint8_t g_has_river = 0;

static uint8_t clamp_u8(int value, uint8_t min, uint8_t max)
{
    if(value < min)
        return min;
    if(value > max)
        return max;
    return (uint8_t)value;
}

static uint16_t clamp_u16(int value, uint16_t min, uint16_t max)
{
    if(value < (int)min)
        return min;
    if(value > (int)max)
        return max;
    return (uint16_t)value;
}

static int abs_i(int value)
{
    return value < 0 ? -value : value;
}

static uint32_t deterministic_hash(uint32_t tick, uint32_t a, uint32_t b, uint32_t salt)
{
    uint32_t value =
        g_environment_summary.seed ^
        (tick * 1664525u) ^
        (a * 1013904223u) ^
        (b * 2246822519u) ^
        (salt * 3266489917u);

    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    value ^= value >> 16;

    return value;
}

static uint8_t deterministic_percent_roll(uint32_t tick, uint32_t a, uint32_t b, uint32_t salt)
{
    return (uint8_t)(deterministic_hash(tick, a, b, salt) % 100u);
}

static uint32_t coord_hash(int x, int y, uint32_t salt)
{
    return deterministic_hash((uint32_t)(x + 4096), (uint32_t)(y + 4096), salt, 97u);
}

static float smoothstepf(float t)
{
    return t * t * (3.0f - (2.0f * t));
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float hash_to_unit(uint32_t value)
{
    return (float)(value & 0xFFFFu) / 65535.0f;
}

static float value_noise(float x, float y, uint32_t salt)
{
    int ix = (int)x;
    int iy = (int)y;
    float fx = x - (float)ix;
    float fy = y - (float)iy;
    float v00 = hash_to_unit(coord_hash(ix, iy, salt));
    float v10 = hash_to_unit(coord_hash(ix + 1, iy, salt));
    float v01 = hash_to_unit(coord_hash(ix, iy + 1, salt));
    float v11 = hash_to_unit(coord_hash(ix + 1, iy + 1, salt));
    float sx = smoothstepf(fx);
    float sy = smoothstepf(fy);
    float a = lerpf(v00, v10, sx);
    float b = lerpf(v01, v11, sx);
    return lerpf(a, b, sy);
}

static float fractal_noise(float x, float y, float scale, int octaves, float persistence, uint32_t salt)
{
    float amplitude = 1.0f;
    float frequency = scale;
    float total = 0.0f;
    float weight = 0.0f;

    for(int octave = 0; octave < octaves; octave++)
    {
        total += value_noise(x * frequency, y * frequency, salt + (uint32_t)(octave * 131u)) * amplitude;
        weight += amplitude;
        amplitude *= persistence;
        frequency *= 2.1f;
    }

    if(weight <= 0.0f)
        return 0.0f;

    return total / weight;
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */

static uint8_t in_bounds(int x, int y)
{
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

#if ENVIRONMENT_HAS_WORLD_STORAGE
static uint8_t is_water_cell(int x, int y)
{
    if(!in_bounds(x, y))
        return 0;

    /* flood=1 is the definitive physical-water marker, set once during
       terrain generation and never cleared. A safe zone or road placed
       on a water cell sets navigable=1 and infrastructure_type, which
       would make the old logic return 0 (not water) for a cell that is
       physically still underwater — causing wrong fire spread, path
       costs, and incident spawning. Check flood first. */
    if(g_environment[x][y].flood)
        return 1;

    return !g_environment[x][y].navigable &&
           g_environment[x][y].road_class == ROAD_NONE &&
           g_environment[x][y].infrastructure_type == INFRA_NONE;
}

static uint8_t has_road(int x, int y)
{
    return in_bounds(x, y) && g_environment[x][y].road_class != ROAD_NONE;
}

static uint8_t district_base_safety(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_RESCUE_BASE: return 96;
        case DISTRICT_MEDICAL:     return 92;
        case DISTRICT_GOVERNMENT:  return 88;
        case DISTRICT_EDUCATION:   return 86;
        case DISTRICT_PARK:        return 90;
        case DISTRICT_RESIDENTIAL: return 82;
        case DISTRICT_COMMERCIAL:  return 80;
        case DISTRICT_MIXED_USE:   return 78;
        case DISTRICT_WATERFRONT:  return 72;
        case DISTRICT_INDUSTRIAL:  return 66;
        default:                   return 80;
    }
}

static uint8_t district_risk(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_INDUSTRIAL: return 78;
        case DISTRICT_WATERFRONT: return 60;
        case DISTRICT_COMMERCIAL: return 54;
        case DISTRICT_MIXED_USE:  return 48;
        case DISTRICT_RESIDENTIAL:return 42;
        case DISTRICT_GOVERNMENT: return 34;
        case DISTRICT_EDUCATION:  return 30;
        case DISTRICT_MEDICAL:    return 24;
        case DISTRICT_RESCUE_BASE:return 18;
        case DISTRICT_PARK:       return 12;
        default:                  return 40;
    }
}

static uint8_t district_seismic_vulnerability(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_INDUSTRIAL: return 32;
        case DISTRICT_WATERFRONT: return 28;
        case DISTRICT_RESIDENTIAL:return 18;
        case DISTRICT_COMMERCIAL: return 16;
        case DISTRICT_MIXED_USE:  return 15;
        case DISTRICT_GOVERNMENT: return 12;
        case DISTRICT_EDUCATION:  return 11;
        case DISTRICT_MEDICAL:    return 8;
        case DISTRICT_RESCUE_BASE:return 5;
        case DISTRICT_PARK:       return 4;
        default:                  return 12;
    }
}

static uint8_t building_quality_for_district(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_RESCUE_BASE: return 92;
        case DISTRICT_MEDICAL:     return 86;
        case DISTRICT_GOVERNMENT:  return 80;
        case DISTRICT_EDUCATION:   return 74;
        case DISTRICT_PARK:        return 72;
        case DISTRICT_COMMERCIAL:  return 68;
        case DISTRICT_MIXED_USE:   return 64;
        case DISTRICT_RESIDENTIAL: return 60;
        case DISTRICT_INDUSTRIAL:  return 54;
        case DISTRICT_WATERFRONT:  return 50;
        default:                   return 60;
    }
}

static uint8_t district_road_density(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_RESIDENTIAL: return 78;
        case DISTRICT_MEDICAL:     return 84;
        case DISTRICT_GOVERNMENT:  return 74;
        case DISTRICT_EDUCATION:   return 66;
        case DISTRICT_COMMERCIAL:  return 72;
        case DISTRICT_MIXED_USE:   return 68;
        case DISTRICT_INDUSTRIAL:  return 56;
        case DISTRICT_WATERFRONT:  return 48;
        case DISTRICT_PARK:        return 38;
        case DISTRICT_RESCUE_BASE: return 70;
        default:                   return 60;
    }
}

static uint8_t district_building_density(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_RESIDENTIAL: return 82;
        case DISTRICT_COMMERCIAL:  return 76;
        case DISTRICT_MIXED_USE:   return 72;
        case DISTRICT_INDUSTRIAL:  return 58;
        case DISTRICT_GOVERNMENT:  return 54;
        case DISTRICT_EDUCATION:   return 50;
        case DISTRICT_MEDICAL:     return 44;
        case DISTRICT_PARK:        return 20;
        case DISTRICT_RESCUE_BASE: return 34;
        case DISTRICT_WATERFRONT:  return 48;
        default:                   return 50;
    }
}

static uint8_t district_population_density(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_RESIDENTIAL: return 84;
        case DISTRICT_MIXED_USE:   return 76;
        case DISTRICT_COMMERCIAL:  return 68;
        case DISTRICT_WATERFRONT:  return 60;
        case DISTRICT_EDUCATION:   return 56;
        case DISTRICT_MEDICAL:     return 52;
        case DISTRICT_GOVERNMENT:  return 42;
        case DISTRICT_INDUSTRIAL:  return 30;
        case DISTRICT_PARK:        return 12;
        case DISTRICT_RESCUE_BASE: return 8;
        default:                   return 50;
    }
}

static uint8_t district_infrastructure_density(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_MEDICAL:     return 90;
        case DISTRICT_RESCUE_BASE: return 88;
        case DISTRICT_GOVERNMENT:  return 72;
        case DISTRICT_EDUCATION:   return 70;
        case DISTRICT_COMMERCIAL:  return 62;
        case DISTRICT_INDUSTRIAL:  return 66;
        case DISTRICT_PARK:        return 54;
        case DISTRICT_MIXED_USE:   return 60;
        case DISTRICT_RESIDENTIAL: return 52;
        case DISTRICT_WATERFRONT:  return 58;
        default:                   return 50;
    }
}

static uint8_t road_travel_cost(uint8_t road_class)
{
    switch(road_class)
    {
        case ROAD_ARTERIAL:  return 1;
        case ROAD_COLLECTOR: return 2;
        case ROAD_LOCAL:     return 3;
        case ROAD_SERVICE:   return 4;
        default:             return 6;
    }
}

static uint8_t road_capacity(uint8_t road_class)
{
    switch(road_class)
    {
        case ROAD_ARTERIAL:  return 100;
        case ROAD_COLLECTOR: return 72;
        case ROAD_LOCAL:     return 44;
        case ROAD_SERVICE:   return 28;
        default:             return 0;
    }
}

static uint8_t road_quality(uint8_t road_class)
{
    switch(road_class)
    {
        case ROAD_ARTERIAL:  return 78;
        case ROAD_COLLECTOR: return 68;
        case ROAD_LOCAL:     return 58;
        case ROAD_SERVICE:   return 52;
        default:             return 50;
    }
}

static void clear_pending_hazard_events(void)
{
    for(uint8_t i = 0; i < MAX_ENV_PENDING_HAZARDS; i++)
        g_pending_hazard_events[i] = (EnvironmentHazardEvent_t){0};
}

static uint8_t queue_hazard_event(IncidentType_t type, int16_t x, int16_t y)
{
    for(uint8_t i = 0; i < MAX_ENV_PENDING_HAZARDS; i++)
    {
        if(g_pending_hazard_events[i].occupied)
            continue;

        g_pending_hazard_events[i].occupied = 1;
        g_pending_hazard_events[i].incident_type = (uint8_t)type;
        g_pending_hazard_events[i].loc_x = x;
        g_pending_hazard_events[i].loc_y = y;
        return 1;
    }

    return 0;
}

static void reset_generation_state(void)
{
    memset(&g_environment_summary, 0, sizeof(g_environment_summary));
    memset(g_environment, 0, sizeof(g_environment));
    memset(g_environment_buildings, 0, sizeof(g_environment_buildings));
    memset(g_districts, 0, sizeof(g_districts));
    memset(g_nav_nodes, 0, sizeof(g_nav_nodes));
    memset(g_nav_edges, 0, sizeof(g_nav_edges));
    memset(g_gateway_sites, 0, sizeof(g_gateway_sites));
    clear_pending_hazard_events();

    for(uint16_t i = 0; i < MAX_ENV_BUILDINGS; i++)
        g_environment_buildings[i].building_id = ENV_NO_BUILDING;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        g_river_center[x] = -1;
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            g_environment[x][y].building_id = ENV_NO_BUILDING;
            g_environment[x][y].road_id = ENV_NO_ROAD;
            g_environment[x][y].nav_node_id = ENV_NO_NAV_NODE;
            g_environment[x][y].district_id = 0;
            g_environment[x][y].navigable = 1;
            g_environment[x][y].travel_cost = 6;
            g_environment[x][y].base_safety = 80;
            g_environment[x][y].env_fault_ref_count = 0;
            g_environment[x][y].env_fault_base_cost = 0;
        }
    }
}

static uint8_t distance_to_coast(int x, int y)
{
    switch(g_coast_orientation)
    {
        case 0: return (uint8_t)(GRID_HEIGHT - 1 - y);
        case 1: return (uint8_t)y;
        case 2: return (uint8_t)(GRID_WIDTH - 1 - x);
        default:return (uint8_t)x;
    }
}

static void mark_water_cell(int x, int y)
{
    if(!in_bounds(x, y))
        return;

    EnvironmentCell_t *cell = &g_environment[x][y];
    cell->road_class = ROAD_NONE;
    cell->road_id = ENV_NO_ROAD;
    cell->road_capacity = 0;
    cell->building_id = ENV_NO_BUILDING;
    cell->navigable = 0;
    cell->flood = 1;
    cell->travel_cost = 25;
    cell->base_safety = clamp_u8((int)cell->base_safety - 20, 20, 90);
    cell->district = DISTRICT_WATERFRONT;
    cell->district_density = 24;
}

/* Stage 1: Terrain generation using layered noise, a coastal edge, and an optional
 * meandering river to create operationally relevant slopes and bottlenecks. */
static void stage_generate_terrain(void)
{
    g_coast_orientation = (uint8_t)random_range_stream(STREAM_ENVIRONMENT, 0, 3);
    g_has_river = (uint8_t)(random_range_stream(STREAM_ENVIRONMENT, 0, 99) < 60);

    int coast_depth = (int)random_range_stream(STREAM_ENVIRONMENT, 3, 5);
    int river_y = (int)random_range_stream(STREAM_ENVIRONMENT, GRID_HEIGHT / 4, (GRID_HEIGHT * 3) / 4);
    int river_drift = (int)random_range_stream(STREAM_ENVIRONMENT, 0, 1) ? 1 : -1;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        if(g_has_river)
        {
            if(x > 0)
            {
                int shift = (int)random_range_stream(STREAM_ENVIRONMENT, 0, 2) - 1;
                river_y = clamp_u16(river_y + shift + river_drift, GRID_HEIGHT / 6, (GRID_HEIGHT * 5) / 6);
                if(random_range_stream(STREAM_ENVIRONMENT, 0, 99) < 24)
                    river_drift = -river_drift;
            }
            g_river_center[x] = (int16_t)river_y;
        }

        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            float macro = fractal_noise((float)x, (float)y, 0.08f, 4, 0.55f, 101u);
            float micro = fractal_noise((float)x, (float)y, 0.22f, 3, 0.50f, 211u);
            int coast_dist = distance_to_coast(x, y);
            float coastal_ramp = (float)coast_dist / (float)(GRID_WIDTH > GRID_HEIGHT ? GRID_WIDTH : GRID_HEIGHT);
            int river_dist = g_has_river ? abs_i(y - g_river_center[x]) : GRID_HEIGHT;
            float river_pull = g_has_river ? (1.0f - ((float)river_dist / (float)(GRID_HEIGHT / 3))) : 0.0f;
            if(river_pull < 0.0f)
                river_pull = 0.0f;

            int elevation =
                12 +
                (int)(macro * 54.0f) +
                (int)(micro * 18.0f) +
                (int)(coastal_ramp * 24.0f) -
                (int)(river_pull * 12.0f);

            cell->terrain_elevation = clamp_u8(elevation, 5, 95);
            cell->soil_condition = clamp_u8(40 + (int)(micro * 34.0f) - (int)(river_pull * 18.0f), 18, 92);
            cell->district = DISTRICT_RESIDENTIAL;
            cell->base_safety = 80;
            cell->district_density = 50;

            if(coast_dist < coast_depth)
                mark_water_cell(x, y);
        }
    }

    if(g_has_river)
    {
        for(int x = 0; x < GRID_WIDTH; x++)
        {
            for(int w = -1; w <= 1; w++)
            {
                int ry = g_river_center[x] + w;
                if(in_bounds(x, ry) && distance_to_coast(x, ry) > 1)
                    mark_water_cell(x, ry);
            }
        }
    }

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            int max_diff = 0;
            static const int dx[4] = {1, -1, 0, 0};
            static const int dy[4] = {0, 0, 1, -1};

            for(int dir = 0; dir < 4; dir++)
            {
                int nx = x + dx[dir];
                int ny = y + dy[dir];
                if(!in_bounds(nx, ny))
                    continue;

                int diff = abs_i((int)g_environment[x][y].terrain_elevation - (int)g_environment[nx][ny].terrain_elevation);
                if(diff > max_diff)
                    max_diff = diff;
            }

            g_environment[x][y].terrain_slope = clamp_u8(max_diff * 8, 0, 100);
        }
    }
}

static void assign_anchor(uint8_t type, int16_t x, int16_t y)
{
    /* stage_generate_districts() below currently hardcodes exactly
       NUM_DISTRICT_TYPES calls to this function, which is the only
       reason g_districts[NUM_DISTRICT_TYPES] never overflows here - if
       that assumption ever breaks (a new district type added without
       updating the call site to match), this guard drops the extra
       anchor silently instead of corrupting whatever static memory
       follows g_districts, and this comment is the trail back to why. */
    if(g_environment_summary.district_total >= NUM_DISTRICT_TYPES)
        return;

    DistrictRecord_t *district = &g_districts[g_environment_summary.district_total];
    district->district_id = g_environment_summary.district_total;
    district->type = type;
    district->center_x = x;
    district->center_y = y;
    district->population_density = district_population_density(type);
    district->building_density = district_building_density(type);
    district->road_density = district_road_density(type);
    district->construction_quality = building_quality_for_district(type);
    district->soil_condition = g_environment[x][y].soil_condition;
    district->seismic_vulnerability = district_seismic_vulnerability(type);
    district->critical_infrastructure_density = district_infrastructure_density(type);
    g_environment_summary.district_total++;
}

static GridPoint_t find_best_land_cell(int min_x, int max_x, int min_y, int max_y, uint8_t prefer_high, uint8_t near_water)
{
    GridPoint_t best = {(int16_t)clamp_u16((min_x + max_x) / 2, 0, GRID_WIDTH - 1),
                        (int16_t)clamp_u16((min_y + max_y) / 2, 0, GRID_HEIGHT - 1)};
    int best_score = INT_MIN;

    for(int x = min_x; x <= max_x; x++)
    {
        for(int y = min_y; y <= max_y; y++)
        {
            if(!in_bounds(x, y) || is_water_cell(x, y))
                continue;

            int coast = distance_to_coast(x, y);
            int river_dist = g_has_river ? abs_i(y - g_river_center[x]) : GRID_HEIGHT;
            int score = prefer_high ? g_environment[x][y].terrain_elevation : (100 - g_environment[x][y].terrain_elevation);
            score += prefer_high ? (100 - g_environment[x][y].terrain_slope) : 0;
            score += near_water ? ((coast <= 6 || river_dist <= 4) ? 40 : -10) : (coast > 4 ? 10 : 0);
            score += g_environment[x][y].soil_condition / 3;

            if(score > best_score)
            {
                best_score = score;
                best.x = (int16_t)x;
                best.y = (int16_t)y;
            }
        }
    }

    return best;
}

unsigned long g_diag_duplicate_anchor_fallback = 0;

static GridPoint_t ensure_unique_anchor(GridPoint_t candidate, const GridPoint_t *anchors, uint16_t count)
{
    for(int radius = 0; radius < GRID_WIDTH + GRID_HEIGHT; radius++)
    {
        for(int dx = -radius; dx <= radius; dx++)
        {
            for(int dy = -radius; dy <= radius; dy++)
            {
                GridPoint_t probe = {(int16_t)(candidate.x + dx), (int16_t)(candidate.y + dy)};
                uint8_t unique = 1;

                if(!in_bounds(probe.x, probe.y) || is_water_cell(probe.x, probe.y))
                    continue;

                for(uint16_t i = 0; i < count; i++)
                {
                    if(abs_i(anchors[i].x - probe.x) + abs_i(anchors[i].y - probe.y) < 5)
                    {
                        unique = 0;
                        break;
                    }
                }

                if(unique)
                    return probe;
            }
        }
    }

    /* Bug fix: no probe within radius [0, GRID_WIDTH+GRID_HEIGHT) was ever
       unique - silently returning the original candidate unmodified used
       to permit a duplicate/overlapping anchor with no signal at all.
       Diagnostic-only fix (matching the same posture as this file's other
       g_diag_* counters for rare/degenerate world-gen outcomes): still
       returns the original candidate (there is no better fallback to
       offer - every reachable cell was already tried), but now a
       degenerate world is at least visible via this counter instead of
       silently producing overlapping districts. */
    g_diag_duplicate_anchor_fallback++;
    return candidate;
}

/* Stage 2: District generation assigns urban intent from seeded anchors rather than
 * hard-coded map slices, creating plausible land-use clusters. */
static void stage_generate_districts(void)
{
    GridPoint_t rescue = find_best_land_cell(1, GRID_WIDTH / 3, 1, GRID_HEIGHT / 3, 1, 0);
    GridPoint_t medical = find_best_land_cell(GRID_WIDTH / 4, (GRID_WIDTH * 3) / 4, GRID_HEIGHT / 4, (GRID_HEIGHT * 3) / 4, 1, 0);
    GridPoint_t industrial = find_best_land_cell((GRID_WIDTH * 2) / 3, GRID_WIDTH - 2, GRID_HEIGHT / 6, (GRID_HEIGHT * 3) / 4, 0, 1);
    GridPoint_t government = find_best_land_cell(GRID_WIDTH / 3, (GRID_WIDTH * 2) / 3, GRID_HEIGHT / 3, (GRID_HEIGHT * 2) / 3, 1, 0);
    GridPoint_t education = find_best_land_cell(2, GRID_WIDTH - 3, GRID_HEIGHT / 3, GRID_HEIGHT - 4, 1, 0);
    GridPoint_t commercial = find_best_land_cell(GRID_WIDTH / 3, GRID_WIDTH - 4, GRID_HEIGHT / 5, (GRID_HEIGHT * 2) / 3, 0, 0);
    GridPoint_t park = find_best_land_cell(2, GRID_WIDTH - 3, 2, GRID_HEIGHT - 3, 1, 1);
    GridPoint_t mixed = find_best_land_cell(GRID_WIDTH / 4, (GRID_WIDTH * 3) / 4, GRID_HEIGHT / 4, (GRID_HEIGHT * 3) / 4, 0, 0);
    GridPoint_t waterfront = find_best_land_cell(1, GRID_WIDTH - 2, 1, GRID_HEIGHT - 2, 0, 1);
    GridPoint_t residential = find_best_land_cell(2, GRID_WIDTH - 3, 2, GRID_HEIGHT - 3, 0, 0);
    GridPoint_t anchors[NUM_DISTRICT_TYPES];

    anchors[0] = rescue;
    anchors[1] = ensure_unique_anchor(residential, anchors, 1);
    anchors[2] = ensure_unique_anchor(commercial, anchors, 2);
    anchors[3] = ensure_unique_anchor(industrial, anchors, 3);
    anchors[4] = ensure_unique_anchor(medical, anchors, 4);
    anchors[5] = ensure_unique_anchor(government, anchors, 5);
    anchors[6] = ensure_unique_anchor(education, anchors, 6);
    anchors[7] = ensure_unique_anchor(mixed, anchors, 7);
    anchors[8] = ensure_unique_anchor(park, anchors, 8);
    anchors[9] = ensure_unique_anchor(waterfront, anchors, 9);

    assign_anchor(DISTRICT_RESCUE_BASE, anchors[0].x, anchors[0].y);
    assign_anchor(DISTRICT_RESIDENTIAL, anchors[1].x, anchors[1].y);
    assign_anchor(DISTRICT_COMMERCIAL, anchors[2].x, anchors[2].y);
    assign_anchor(DISTRICT_INDUSTRIAL, anchors[3].x, anchors[3].y);
    assign_anchor(DISTRICT_MEDICAL, anchors[4].x, anchors[4].y);
    assign_anchor(DISTRICT_GOVERNMENT, anchors[5].x, anchors[5].y);
    assign_anchor(DISTRICT_EDUCATION, anchors[6].x, anchors[6].y);
    assign_anchor(DISTRICT_MIXED_USE, anchors[7].x, anchors[7].y);
    assign_anchor(DISTRICT_PARK, anchors[8].x, anchors[8].y);
    assign_anchor(DISTRICT_WATERFRONT, anchors[9].x, anchors[9].y);

    memset(g_environment_summary.district_count, 0, sizeof(g_environment_summary.district_count));

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            int best_score = INT_MIN;
            uint16_t best_district = 0;

            if(is_water_cell(x, y))
            {
                cell->district = DISTRICT_WATERFRONT;
                cell->district_id = 9;
                continue;
            }

            for(uint16_t i = 0; i < g_environment_summary.district_total; i++)
            {
                const DistrictRecord_t *district = &g_districts[i];
                int dist = abs_i(x - district->center_x) + abs_i(y - district->center_y);
                int score = 140 - (dist * 5);
                score += district->road_density / 3;
                score += district->construction_quality / 6;
                score -= g_environment[x][y].terrain_slope / 5;

                if(district->type == DISTRICT_INDUSTRIAL)
                {
                    score += (distance_to_coast(x, y) <= 6 ? 28 : -18);
                    score += (cell->soil_condition < 48 ? 12 : -6);
                }
                else if(district->type == DISTRICT_WATERFRONT)
                {
                    score += (distance_to_coast(x, y) <= 5 ? 36 : -24);
                }
                else if(district->type == DISTRICT_PARK)
                {
                    score += (cell->terrain_slope < 24 ? 20 : -10);
                    score += (distance_to_coast(x, y) <= 7 ? 10 : 0);
                }
                else if(district->type == DISTRICT_MEDICAL || district->type == DISTRICT_RESCUE_BASE)
                {
                    score += (cell->terrain_slope < 22 ? 18 : -16);
                    score += (cell->soil_condition > 46 ? 14 : -12);
                }
                else if(district->type == DISTRICT_RESIDENTIAL || district->type == DISTRICT_MIXED_USE)
                {
                    score += (cell->terrain_slope < 28 ? 14 : -10);
                }

                if(dist <= 4)
                    score += 35;

                if(score > best_score)
                {
                    best_score = score;
                    best_district = i;
                }
            }

            cell->district = g_districts[best_district].type;
            cell->district_id = best_district;
            cell->district_density = g_districts[best_district].building_density;
            cell->base_safety = district_base_safety(cell->district);
            g_environment_summary.district_count[cell->district]++;
        }
    }

    for(uint16_t i = 0; i < g_environment_summary.district_total; i++)
    {
        for(int dx = -2; dx <= 2; dx++)
        {
            for(int dy = -2; dy <= 2; dy++)
            {
                int x = g_districts[i].center_x + dx;
                int y = g_districts[i].center_y + dy;
                EnvironmentCell_t *cell;

                if(!in_bounds(x, y) || is_water_cell(x, y))
                    continue;

                cell = &g_environment[x][y];
                g_environment_summary.district_count[cell->district]--;
                cell->district = g_districts[i].type;
                cell->district_id = i;
                cell->district_density = g_districts[i].building_density;
                cell->base_safety = district_base_safety(cell->district);
                g_environment_summary.district_count[cell->district]++;
            }
        }
    }

    for(int dx = -3; dx <= 3; dx++)
    {
        for(int dy = -3; dy <= 3; dy++)
        {
            int x = g_districts[DISTRICT_MIXED_USE].center_x + dx;
            int y = g_districts[DISTRICT_MIXED_USE].center_y + dy;
            EnvironmentCell_t *cell;

            if(!in_bounds(x, y) || is_water_cell(x, y))
                continue;

            cell = &g_environment[x][y];
            g_environment_summary.district_count[cell->district]--;
            cell->district = DISTRICT_MIXED_USE;
            cell->district_id = DISTRICT_MIXED_USE;
            cell->district_density = g_districts[DISTRICT_MIXED_USE].building_density;
            cell->base_safety = district_base_safety(cell->district);
            g_environment_summary.district_count[cell->district]++;
        }
    }

    g_environment_summary.connected_districts = (uint8_t)g_environment_summary.district_total;
}

static void paint_road_cell(int x, int y, uint8_t road_class, uint16_t road_id, uint8_t bridge)
{
    if(!in_bounds(x, y))
        return;

    EnvironmentCell_t *cell = &g_environment[x][y];

    if(road_class < cell->road_class)
        return;

    if(is_water_cell(x, y) && !bridge)
        return;

    cell->road_class = road_class;
    cell->road_id = road_id;
    cell->navigable = 1;
    cell->travel_cost = road_travel_cost(road_class);
    cell->road_capacity = road_capacity(road_class);
    cell->base_safety = clamp_u8((int)cell->base_safety + (road_class == ROAD_ARTERIAL ? 6 : 2), 20, 100);

    if(bridge)
    {
        cell->bridge = 1;
        cell->infrastructure_type = INFRA_BRIDGE;
    }
}

static void paint_road_width(int x, int y, uint8_t road_class, uint16_t road_id, int width, uint8_t vertical)
{
    for(int offset = -width; offset <= width; offset++)
    {
        int px = vertical ? x + offset : x;
        int py = vertical ? y : y + offset;
        uint8_t bridge = is_water_cell(px, py);
        paint_road_cell(px, py, road_class, road_id, bridge);
    }
}

static uint16_t next_road_id(void)
{
    static uint16_t road_id = 0;
    return ++road_id;
}

static uint16_t connection_cost(int x, int y, int tx, int ty, uint8_t road_class, uint8_t allow_bridge)
{
    if(!in_bounds(x, y))
        return UINT16_MAX;

    const EnvironmentCell_t *cell = &g_environment[x][y];
    uint16_t cost = 8u + cell->terrain_slope + (uint16_t)(100 - cell->soil_condition) / 3u;
    cost += (uint16_t)(abs_i(x - tx) + abs_i(y - ty));

    if(is_water_cell(x, y))
    {
        if(!allow_bridge)
            return UINT16_MAX;
        cost += 80;
    }

    if(cell->road_class != ROAD_NONE)
        cost /= 2u;

    if(road_class == ROAD_ARTERIAL && cell->district == DISTRICT_PARK)
        cost += 20;

    return cost;
}

static uint8_t carve_path_between(GridPoint_t start, GridPoint_t end, uint8_t road_class, int width, uint8_t allow_bridge)
{
    PathNode_t (* const nodes)[GRID_HEIGHT] = g_carve_nodes;
    uint8_t    (* const visited)[GRID_HEIGHT] = g_carve_visited;
    memset(g_carve_visited, 0, sizeof(g_carve_visited));
    uint16_t road_id = next_road_id();

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            nodes[x][y].open = 0;
            nodes[x][y].cost = UINT16_MAX;
            nodes[x][y].parent_x = (uint16_t)x;
            nodes[x][y].parent_y = (uint16_t)y;
        }
    }

    nodes[start.x][start.y].cost = 0;
    nodes[start.x][start.y].open = 1;

    while(1)
    {
        int best_x = -1;
        int best_y = -1;
        uint16_t best_cost = UINT16_MAX;

        for(int x = 0; x < GRID_WIDTH; x++)
        {
            for(int y = 0; y < GRID_HEIGHT; y++)
            {
                if(!nodes[x][y].open || visited[x][y])
                    continue;

                uint16_t score = nodes[x][y].cost + (uint16_t)(abs_i(x - end.x) + abs_i(y - end.y));
                if(score < best_cost)
                {
                    best_cost = score;
                    best_x = x;
                    best_y = y;
                }
            }
        }

        if(best_x < 0)
            return 0;

        if(best_x == end.x && best_y == end.y)
            break;

        visited[best_x][best_y] = 1;

        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};
        for(int dir = 0; dir < 4; dir++)
        {
            int nx = best_x + dx[dir];
            int ny = best_y + dy[dir];
            if(!in_bounds(nx, ny) || visited[nx][ny])
                continue;

            uint16_t step = connection_cost(nx, ny, end.x, end.y, road_class, allow_bridge);
            if(step == UINT16_MAX)
                continue;

            uint16_t next_cost = nodes[best_x][best_y].cost + step;
            if(!nodes[nx][ny].open || next_cost < nodes[nx][ny].cost)
            {
                nodes[nx][ny].open = 1;
                nodes[nx][ny].cost = next_cost;
                nodes[nx][ny].parent_x = (uint16_t)best_x;
                nodes[nx][ny].parent_y = (uint16_t)best_y;
            }
        }
    }

    int cx = end.x;
    int cy = end.y;
    /* Bug fix: no iteration cap. Safe today only because parent pointers
       are only ever set by the forward search above, which always forms
       a tree rooted at `start` - so this can't currently cycle - but
       that's an invariant of the forward search, not something this
       backtrace verifies for itself. GRID_WIDTH*GRID_HEIGHT is the most
       cells a path could possibly visit, so bounding the loop at that
       many steps costs nothing in the working case and turns a future
       forward-search bug that lets a node get re-parented into a
       diagnostic instead of a hang. */
    for(int steps = 0; steps < GRID_WIDTH * GRID_HEIGHT && !(cx == start.x && cy == start.y); steps++)
    {
        int px = nodes[cx][cy].parent_x;
        int py = nodes[cx][cy].parent_y;
        uint8_t vertical = px != cx;
        paint_road_width(cx, cy, road_class, road_id, width, vertical);
        cx = px;
        cy = py;
    }

    paint_road_width(start.x, start.y, road_class, road_id, width, 1);
    return 1;
}

/* Stage 3 and 4: Arterial and secondary roads are laid as a connected graph that
 * follows terrain and creates district access plus natural bridge bottlenecks. */
static void stage_generate_roads(void)
{
    GridPoint_t anchors[NUM_DISTRICT_TYPES];
    for(uint16_t i = 0; i < g_environment_summary.district_total; i++)
    {
        anchors[i].x = g_districts[i].center_x;
        anchors[i].y = g_districts[i].center_y;
    }

    carve_path_between(anchors[0], anchors[4], ROAD_ARTERIAL, 1, 1);
    carve_path_between(anchors[4], anchors[5], ROAD_ARTERIAL, 1, 1);
    carve_path_between(anchors[5], anchors[2], ROAD_ARTERIAL, 1, 1);
    carve_path_between(anchors[2], anchors[9], ROAD_ARTERIAL, 1, 1);
    carve_path_between(anchors[0], anchors[1], ROAD_ARTERIAL, 1, 1);
    carve_path_between(anchors[1], anchors[7], ROAD_COLLECTOR, 0, 1);
    carve_path_between(anchors[7], anchors[6], ROAD_COLLECTOR, 0, 1);
    carve_path_between(anchors[6], anchors[8], ROAD_COLLECTOR, 0, 1);
    carve_path_between(anchors[8], anchors[9], ROAD_COLLECTOR, 0, 1);

    for(int x = 1; x < GRID_WIDTH - 1; x++)
    {
        for(int y = 1; y < GRID_HEIGHT - 1; y++)
        {
            if(is_water_cell(x, y) || g_environment[x][y].road_class != ROAD_NONE)
                continue;

            uint8_t district = g_environment[x][y].district;
            int spacing =
                district == DISTRICT_RESIDENTIAL ? 4 :
                district == DISTRICT_INDUSTRIAL ? 6 :
                district == DISTRICT_MEDICAL ? 4 :
                district == DISTRICT_PARK ? 7 : 5;

            int jitter = (int)(coord_hash(x, y, 411u) % 3u);
            int hline = ((y + jitter) % spacing) == 0;
            int vline = ((x + jitter + (district % 3)) % spacing) == 0;

            if(hline || vline)
            {
                uint8_t road_class =
                    district == DISTRICT_INDUSTRIAL ? ROAD_COLLECTOR :
                    district == DISTRICT_PARK ? ROAD_SERVICE :
                    district == DISTRICT_MEDICAL ? ROAD_COLLECTOR : ROAD_LOCAL;

                if(g_environment[x][y].terrain_slope < 48)
                    paint_road_cell(x, y, road_class, next_road_id(), 0);
            }
        }
    }

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            if(g_environment[x][y].road_class == ROAD_NONE)
                continue;
            g_environment_summary.road_cell_count++;
        }
    }
}

static int road_degree(int x, int y)
{
    static const int dx[4] = {1, -1, 0, 0};
    static const int dy[4] = {0, 0, 1, -1};
    int degree = 0;

    for(int dir = 0; dir < 4; dir++)
    {
        if(has_road(x + dx[dir], y + dy[dir]))
            degree++;
    }

    return degree;
}

static uint8_t lot_adjacent_to_road(int x, int y, uint8_t *best_class, uint8_t *access_count)
{
    static const int dx[4] = {1, -1, 0, 0};
    static const int dy[4] = {0, 0, 1, -1};
    uint8_t found = 0;
    uint8_t strongest = ROAD_NONE;
    uint8_t count = 0;

    for(int dir = 0; dir < 4; dir++)
    {
        int nx = x + dx[dir];
        int ny = y + dy[dir];
        if(!has_road(nx, ny))
            continue;
        count++;
        found = 1;
        if(g_environment[nx][ny].road_class > strongest)
            strongest = g_environment[nx][ny].road_class;
    }

    if(best_class != NULL)
        *best_class = strongest;
    if(access_count != NULL)
        *access_count = count;
    return found;
}

static uint8_t can_place_building_rect(int x, int y, int w, int h, uint16_t district_id)
{
    if(x <= 0 || y <= 0 || x + w >= GRID_WIDTH || y + h >= GRID_HEIGHT)
        return 0;

    for(int cx = x; cx < x + w; cx++)
    {
        for(int cy = y; cy < y + h; cy++)
        {
            EnvironmentCell_t *cell = &g_environment[cx][cy];
            if(is_water_cell(cx, cy) ||
               cell->road_class != ROAD_NONE ||
               cell->bridge ||
               cell->building_id != ENV_NO_BUILDING ||
               cell->district_id != district_id)
            {
                return 0;
            }
        }
    }

    return lot_adjacent_to_road(x, y, NULL, NULL) ||
           lot_adjacent_to_road(x + w - 1, y, NULL, NULL) ||
           lot_adjacent_to_road(x, y + h - 1, NULL, NULL) ||
           lot_adjacent_to_road(x + w - 1, y + h - 1, NULL, NULL);
}

static uint8_t pick_building_type(uint8_t district, int area)
{
    switch(district)
    {
        case DISTRICT_RESCUE_BASE: return BUILDING_UTILITY;
        case DISTRICT_MEDICAL:     return BUILDING_MEDICAL;
        case DISTRICT_GOVERNMENT:  return BUILDING_CIVIC;
        case DISTRICT_EDUCATION:   return BUILDING_EDUCATION;
        case DISTRICT_INDUSTRIAL:  return area >= 9 ? BUILDING_INDUSTRIAL : BUILDING_UTILITY;
        case DISTRICT_COMMERCIAL:  return area >= 8 ? BUILDING_OFFICE : BUILDING_COMMERCIAL;
        case DISTRICT_MIXED_USE:   return area >= 6 ? BUILDING_APARTMENT : BUILDING_COMMERCIAL;
        case DISTRICT_WATERFRONT:  return area >= 6 ? BUILDING_COMMERCIAL : BUILDING_RESIDENTIAL;
        case DISTRICT_PARK:        return BUILDING_PARKING;
        default:                   return area >= 6 ? BUILDING_APARTMENT : BUILDING_RESIDENTIAL;
    }
}

static void place_building_rect(int x, int y, int w, int h)
{
    if(g_environment_summary.building_count >= MAX_ENV_BUILDINGS)
        return;

    EnvironmentCell_t *root = &g_environment[x][y];
    uint16_t id = g_environment_summary.building_count;
    uint8_t access_class = ROAD_NONE;
    uint8_t access_count = 0;
    EnvironmentBuilding_t *building = &g_environment_buildings[id];
    int area = w * h;

    lot_adjacent_to_road(x, y, &access_class, &access_count);
    if(access_count == 0)
    {
        lot_adjacent_to_road(x + w - 1, y + h - 1, &access_class, &access_count);
        if(access_count == 0)
            access_count = 1;
    }

    *building = (EnvironmentBuilding_t){0};
    building->building_id = id;
    building->district = root->district;
    building->district_id = root->district_id;
    building->building_type = pick_building_type(root->district, area);
    building->loc_x = (int16_t)(x + (w / 2));
    building->loc_y = (int16_t)(y + (h / 2));
    building->footprint = (uint8_t)area;
    building->footprint_width = (uint8_t)w;
    building->footprint_height = (uint8_t)h;
    building->floors = (uint8_t)(
        root->district == DISTRICT_COMMERCIAL ? 5 + (area / 4) :
        root->district == DISTRICT_GOVERNMENT ? 4 + (area / 5) :
        root->district == DISTRICT_MEDICAL ? 3 + (area / 6) :
        root->district == DISTRICT_INDUSTRIAL ? 2 + (area / 7) :
        root->district == DISTRICT_RESIDENTIAL ? 1 + (area / 5) :
        root->district == DISTRICT_MIXED_USE ? 3 + (area / 5) :
        root->district == DISTRICT_EDUCATION ? 3 + (area / 6) : 2);
    building->height_m = clamp_u8(8 + (building->floors * 4), 8, 90);
    building->occupancy = clamp_u8(
        area * (root->district == DISTRICT_RESIDENTIAL ? 8 :
                root->district == DISTRICT_COMMERCIAL ? 12 :
                root->district == DISTRICT_MEDICAL ? 10 :
                root->district == DISTRICT_EDUCATION ? 14 :
                root->district == DISTRICT_INDUSTRIAL ? 6 : 9),
        4,
        120);
    building->expected_civilians = clamp_u8(
        (building->occupancy * (district_population_density(root->district) + 20)) / 100,
        0,
        120);
    building->risk_level = district_risk(root->district);
    building->construction_quality = clamp_u8(
        building_quality_for_district(root->district) +
        (int)(coord_hash(x, y, 631u) % 15u) - 7,
        24,
        96);
    building->collapse_resistance = clamp_u8(
        building->construction_quality - (building->height_m / 5) + (100 - root->soil_condition) / 10,
        18,
        95);
    building->seismic_vulnerability = clamp_u8(
        district_seismic_vulnerability(root->district) + (building->height_m / 6) + ((100 - building->construction_quality) / 6),
        5,
        95);
    building->fire_susceptibility = clamp_u8(
        building->risk_level + (building->building_type == BUILDING_INDUSTRIAL ? 15 : 0) + (building->building_type == BUILDING_MEDICAL ? -8 : 0),
        8,
        95);
    building->gas_infrastructure = (uint8_t)(
        building->building_type == BUILDING_INDUSTRIAL ||
        building->building_type == BUILDING_COMMERCIAL ||
        building->building_type == BUILDING_OFFICE ||
        building->building_type == BUILDING_MEDICAL);
    building->electrical_infrastructure = (uint8_t)(
        building->building_type != BUILDING_PARKING);
    building->access_road_class = access_class;
    building->access_count = access_count;

    for(int cx = x; cx < x + w; cx++)
    {
        for(int cy = y; cy < y + h; cy++)
        {
            EnvironmentCell_t *cell = &g_environment[cx][cy];
            cell->building_id = id;
            cell->travel_cost = clamp_u8((int)cell->travel_cost + 3, 1, 12);
            cell->base_safety = clamp_u8((int)cell->base_safety - (building->risk_level / 12), 30, 100);
        }
    }

    g_environment_summary.building_count++;
}

/* Stage 5 and 6: Road-defined parcels are filled with multi-cell buildings whose
 * metadata serves mission planning rather than rendering. */
static void stage_generate_buildings(void)
{
    for(int x = 1; x < GRID_WIDTH - 2; x++)
    {
        for(int y = 1; y < GRID_HEIGHT - 2; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            int density_roll;
            int max_w;
            int max_h;

            if(g_environment_summary.building_count >= MAX_ENV_BUILDINGS)
                return;

            if(is_water_cell(x, y) ||
               cell->road_class != ROAD_NONE ||
               cell->building_id != ENV_NO_BUILDING ||
               cell->district == DISTRICT_PARK)
            {
                continue;
            }

            density_roll = (int)(coord_hash(x, y, 701u) % 100u);
            if(density_roll > district_building_density(cell->district))
                continue;

            max_w =
                cell->district == DISTRICT_INDUSTRIAL ? 4 :
                cell->district == DISTRICT_COMMERCIAL ? 3 :
                cell->district == DISTRICT_RESIDENTIAL ? 2 :
                cell->district == DISTRICT_MEDICAL ? 4 :
                cell->district == DISTRICT_EDUCATION ? 4 : 3;
            max_h =
                cell->district == DISTRICT_INDUSTRIAL ? 4 :
                cell->district == DISTRICT_COMMERCIAL ? 3 :
                cell->district == DISTRICT_RESIDENTIAL ? 2 :
                cell->district == DISTRICT_MEDICAL ? 3 :
                cell->district == DISTRICT_EDUCATION ? 3 : 3;

            for(int w = max_w; w >= 1; w--)
            {
                for(int h = max_h; h >= 1; h--)
                {
                    if(can_place_building_rect(x, y, w, h, cell->district_id))
                    {
                        place_building_rect(x, y, w, h);
                        w = 0;
                        break;
                    }
                }
            }
        }
    }
}

static void mark_safe_zone_candidate(int x, int y)
{
    if(!in_bounds(x, y))
        return;

    if(!g_environment[x][y].safe_zone_candidate)
        g_environment_summary.safe_zone_candidate_count++;
    g_environment[x][y].safe_zone_candidate = 1;
}

/* Diagnostic: per-seed count of worlds where the 3 evacuation gateways did
 * NOT resolve to 3 distinct valid cells (the pre-fix bug collapsed all 3
 * onto the single best-scoring border cell). Process-cumulative; the P1
 * sweep captures before/after deltas. Expected to stay 0. */
unsigned long g_diag_gateway_degenerate_worlds = 0;
/* Cumulative count of evacuation-gateway candidates that needed
 * carve_repair_to_nearest_road() (not already road-connected when chosen).
 * Process-cumulative; the P1 sweep captures before/after deltas. */
unsigned long g_diag_gateway_repaired = 0;

/* Forward declarations: stage_generate_infrastructure() needs the road-
 * reachability trio (defined further down this file, where stage_validate_
 * and_build_navigation() already uses them) to repair a gateway candidate
 * that isn't road-connected yet. */
static uint8_t road_reachable_map(uint8_t reachable[GRID_WIDTH][GRID_HEIGHT]);
static uint8_t lot_reachable(const uint8_t reachable[GRID_WIDTH][GRID_HEIGHT], int x, int y);
static void carve_repair_to_nearest_road(int x, int y);

/* Stage 7: Infrastructure highlights bridges, intersections, and open-space nodes
 * that later become safe-zone or gateway candidates. */
static void stage_generate_infrastructure(void)
{
    for(int x = 1; x < GRID_WIDTH - 1; x++)
    {
        for(int y = 1; y < GRID_HEIGHT - 1; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            int degree = road_degree(x, y);

            if(cell->bridge)
                g_environment_summary.bridge_count++;

            if(cell->road_class != ROAD_NONE && degree >= 3 && cell->infrastructure_type == INFRA_NONE)
                cell->infrastructure_type = INFRA_INTERSECTION;

            if(cell->district == DISTRICT_PARK && cell->road_class == ROAD_NONE && cell->building_id == ENV_NO_BUILDING)
            {
                cell->infrastructure_type = INFRA_OPEN_SPACE;
                if((coord_hash(x, y, 811u) % 100u) < 35u)
                    mark_safe_zone_candidate(x, y);
            }

            if(cell->bridge)
                mark_safe_zone_candidate(x, y);
        }
    }

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        if(building->building_type == BUILDING_MEDICAL ||
           building->building_type == BUILDING_EDUCATION ||
           building->building_type == BUILDING_UTILITY)
        {
            mark_safe_zone_candidate(building->loc_x, building->loc_y);
        }
    }

    /* Reachability snapshot for gateway repair below. Built once, up front -
     * rebuilt only when a repair actually carves a new road, same pattern
     * as environment_validate_hazard_reachability(). */
    uint8_t gw_reachable[GRID_WIDTH][GRID_HEIGHT];
    road_reachable_map(gw_reachable);

    for(uint8_t i = 0; i < NUM_EVACUATION_GATEWAYS; i++)
    {
        GatewaySite_t *gateway = &g_gateway_sites[i];
        int best_score = INT_MIN;
        gateway->valid = 0;

        for(int x = 0; x < GRID_WIDTH; x++)
        {
            for(int y = 0; y < GRID_HEIGHT; y++)
            {
                /* No has_road() requirement here - most worlds have 0-1
                 * border cells that already sit on a road (measured
                 * directly), so requiring one left this loop with nothing
                 * to pick in the large majority of worlds. Any non-water
                 * border cell is now a candidate; road_class still feeds
                 * the score below, so an existing road cell wins over a
                 * bare one when both are available, and the repair pass
                 * after this loop connects whichever cell is actually
                 * picked. */
                if(is_water_cell(x, y))
                    continue;
                if(x != 0 && y != 0 && x != GRID_WIDTH - 1 && y != GRID_HEIGHT - 1)
                    continue;

                /* Skip cells already claimed by an earlier gateway */
                int already_used = 0;
                for(uint8_t k = 0; k < i; k++)
                {
                    if(g_gateway_sites[k].valid &&
                       g_gateway_sites[k].x == (int16_t)x &&
                       g_gateway_sites[k].y == (int16_t)y)
                    {
                        already_used = 1;
                        break;
                    }
                }
                if(already_used)
                    continue;

                int score = (int)g_environment[x][y].road_class * 25
                            - g_environment[x][y].terrain_slope;
                if(score > best_score)
                {
                    best_score = score;
                    gateway->x = (int16_t)x;
                    gateway->y = (int16_t)y;
                    gateway->valid = 1;
                }
            }
        }

        /* Repair access if the chosen candidate isn't already road-connected.
         * "Connected" covers both shapes a candidate can take now that
         * has_road() is no longer required above: the cell itself is a
         * reachable road, or it's a bare lot adjacent to one (lot_reachable(),
         * same check environment_validate_hazard_reachability() uses for
         * safe zones/incidents). carve_repair_to_nearest_road() paints a
         * road onto the cell itself as part of carving to it, so this
         * handles "no road at all" (the dominant case) the same way it
         * handles "has a road but it's an isolated fragment". */
        if(gateway->valid)
        {
            uint8_t already_connected =
                (has_road(gateway->x, gateway->y) && gw_reachable[gateway->x][gateway->y]) ||
                lot_reachable(gw_reachable, gateway->x, gateway->y);

            if(!already_connected)
            {
                g_diag_gateway_repaired++;
                carve_repair_to_nearest_road(gateway->x, gateway->y);
                road_reachable_map(gw_reachable);
            }
        }
    }

    /* Verify the "skip cells already claimed by an earlier gateway" scan
     * above actually produced 3 distinct valid cells - the pre-fix code
     * had no such exclusion and collapsed all NUM_EVACUATION_GATEWAYS
     * onto one coordinate every run. */
    for(uint8_t a = 0; a < NUM_EVACUATION_GATEWAYS; a++)
    {
        if(!g_gateway_sites[a].valid)
        {
            g_diag_gateway_degenerate_worlds++;
            break;
        }
        uint8_t dup = 0;
        for(uint8_t b = a + 1; b < NUM_EVACUATION_GATEWAYS; b++)
        {
            if(g_gateway_sites[b].valid &&
               g_gateway_sites[a].x == g_gateway_sites[b].x &&
               g_gateway_sites[a].y == g_gateway_sites[b].y)
            {
                dup = 1;
                break;
            }
        }
        if(dup)
        {
            g_diag_gateway_degenerate_worlds++;
            break;
        }
    }
    /* Verified via g_diag_gateway_degenerate_worlds (--sweep-priority1):
     * 0 degenerate worlds across 550 seeds in 3 disjoint ranges, 0 carve-
     * repair failures. Dropping the has_road() requirement above (any
     * non-water border cell is a candidate, not just one already on a
     * road) means this loop virtually always finds NUM_EVACUATION_GATEWAYS
     * distinct valid cells, and the repair pass just above connects
     * whichever cell is actually picked - so initialize_evacuation_
     * gateways()'s (incident.c) random un-road-checked fallback is a
     * true last-resort, not the common case a stale prior version of
     * this comment claimed. */
}

static uint8_t road_reachable_map(uint8_t reachable[GRID_WIDTH][GRID_HEIGHT])
{
    GridPoint_t *const queue = g_road_reachable_queue;
    int head = 0;
    int tail = 0;
    uint8_t found_start = 0;

    memset(reachable, 0, sizeof(uint8_t) * GRID_WIDTH * GRID_HEIGHT);

    /* Seed from anchors[0] (DISTRICT_RESCUE_BASE, g_districts[0]) instead of
     * an arbitrary row-major-first road cell. stage_generate_roads() carves
     * an explicit, always-succeeding (allow_bridge=1, so connection_cost()
     * never fails in-bounds) 9-anchor chain starting at anchors[0], and
     * carve_path_between() always paints a road onto its own start cell -
     * so anchors[0] is provably part of the map's one guaranteed-connected
     * component. A row-major scan has no such guarantee: measured directly,
     * it lands on a disconnected 1-2 cell fragment from the probabilistic
     * secondary road fill (stage_generate_roads()'s hline/vline pass) in
     * roughly 3 of 5 sampled worlds, making every "is X reachable" check
     * built on that map meaningless for that call - including every prior
     * use of this function (building/district validation, and Priority-1's
     * post-earthquake safe-zone/incident pass). Falls back to the old
     * row-major scan only if anchors[0]'s own cell isn't usable (e.g.
     * earthquake damage road-blocked it) - still better than refusing to
     * build a map at all. */
    if(g_environment_summary.district_total > 0)
    {
        int sx = g_districts[0].center_x;
        int sy = g_districts[0].center_y;
        if(in_bounds(sx, sy) && has_road(sx, sy) && !g_environment[sx][sy].road_blocked)
        {
            queue[tail++] = (GridPoint_t){(int16_t)sx, (int16_t)sy};
            reachable[sx][sy] = 1;
            found_start = 1;
        }
    }

    if(!found_start)
    {
        for(int x = 0; x < GRID_WIDTH && !found_start; x++)
        {
            for(int y = 0; y < GRID_HEIGHT; y++)
            {
                if(has_road(x, y) && !g_environment[x][y].road_blocked)
                {
                    queue[tail++] = (GridPoint_t){(int16_t)x, (int16_t)y};
                    reachable[x][y] = 1;
                    found_start = 1;
                    break;
                }
            }
        }
    }

    if(!found_start)
        return 0;

    while(head < tail)
    {
        GridPoint_t current = queue[head++];
        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};

        for(int dir = 0; dir < 4; dir++)
        {
            int nx = current.x + dx[dir];
            int ny = current.y + dy[dir];
            if(!has_road(nx, ny) || g_environment[nx][ny].road_blocked || reachable[nx][ny])
                continue;

            reachable[nx][ny] = 1;
            queue[tail++] = (GridPoint_t){(int16_t)nx, (int16_t)ny};
        }
    }

    return 1;
}

/*
 * Note on a -Wpedantic warning at this function's two call sites in
 * stage_validate_and_build_navigation(): "invalid use of pointers to
 * arrays with different qualifiers in ISO C before C2X". The caller's
 * `reachable` is a plain (non-const) uint8_t[GRID_WIDTH][GRID_HEIGHT];
 * this function takes it as const for the pointer/read-only version.
 * That's a benign qualifier mismatch, not a wrong-array-decay bug:
 * both decay to the same address with the same row stride regardless
 * of the const qualifier on the element type (const is compile-time
 * only here, not part of the runtime representation), and GCC's own
 * wording ("before C2X") confirms this exact conversion was simply
 * missing from ISO C's implicit-conversion rules prior to C23, which
 * added it - see C23's changes to 6.5.16.1/6.3.2.3 for qualified
 * array-pointer conversions. Left as-is rather than dropping the
 * `const` here (which would silence the warning but lose the
 * documentation that this function only reads the reachability map)
 * or duplicating a non-const overload just for this.
 */
static uint8_t lot_reachable(const uint8_t reachable[GRID_WIDTH][GRID_HEIGHT], int x, int y)
{
    static const int dx[4] = {1, -1, 0, 0};
    static const int dy[4] = {0, 0, 1, -1};

    for(int dir = 0; dir < 4; dir++)
    {
        int nx = x + dx[dir];
        int ny = y + dy[dir];
        if(in_bounds(nx, ny) && reachable[nx][ny])
            return 1;
    }

    return 0;
}

/* Diagnostic: how many times carve_repair_to_nearest_road() actually fired
 * (i.e. a lot was flagged unreachable and a repair path was carved).
 * Process-cumulative; the priority-1 sweep captures before/after deltas. */
unsigned long g_diag_carve_repair_count = 0;
unsigned long g_diag_carve_repair_failed = 0;   /* carve_path_between() returned 0 */

static void carve_repair_to_nearest_road(int x, int y)
{
    GridPoint_t start = {(int16_t)x, (int16_t)y};
    GridPoint_t best = start;
    int best_dist = INT_MAX;

    g_diag_carve_repair_count++;

    for(int cx = 0; cx < GRID_WIDTH; cx++)
    {
        for(int cy = 0; cy < GRID_HEIGHT; cy++)
        {
            if(!has_road(cx, cy))
                continue;
            int dist = abs_i(cx - x) + abs_i(cy - y);
            if(dist < best_dist)
            {
                best_dist = dist;
                best.x = (int16_t)cx;
                best.y = (int16_t)cy;
            }
        }
    }

    if(!carve_path_between(start, best, ROAD_LOCAL, 0, 1))
        g_diag_carve_repair_failed++;
}

static void ensure_hospital_redundancy(void)
{
    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        if(building->building_type != BUILDING_MEDICAL || building->access_count >= 2)
            continue;

        int x = building->loc_x;
        int y = building->loc_y;
        /* Bug fix: access_count used to be hardcoded to 2 here
           regardless of whether either paint_road_cell() call below
           actually ran - a hospital near the grid's far edge (both
           guards false) got credited with redundant access it never
           received, silently misleading any consumer that trusts
           access_count >= 2 to mean "reachable via 2 independent
           routes." Only count the roads actually painted. */
        if(x + 1 < GRID_WIDTH - 1)
        {
            paint_road_cell(x + 1, y, ROAD_COLLECTOR, next_road_id(), 0);
            building->access_count++;
        }
        if(y + 1 < GRID_HEIGHT - 1)
        {
            paint_road_cell(x, y + 1, ROAD_COLLECTOR, next_road_id(), 0);
            building->access_count++;
        }
    }
}

/* Stage 10 and validation: Build a lightweight navigation graph from intersections
 * and access points, then repair any broken access before handing the world off. */
static void stage_validate_and_build_navigation(void)
{
    uint8_t reachable[GRID_WIDTH][GRID_HEIGHT];

    road_reachable_map(reachable);

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        if(!lot_reachable(reachable, building->loc_x, building->loc_y))
        {
            carve_repair_to_nearest_road(building->loc_x, building->loc_y);
            road_reachable_map(reachable);
        }
    }

    for(uint16_t i = 0; i < g_environment_summary.district_total; i++)
    {
        if(!lot_reachable(reachable, g_districts[i].center_x, g_districts[i].center_y))
        {
            carve_repair_to_nearest_road(g_districts[i].center_x, g_districts[i].center_y);
            road_reachable_map(reachable);
        }
    }

    ensure_hospital_redundancy();
    road_reachable_map(reachable);

    g_environment_summary.nav_node_count = 0;
    g_environment_summary.nav_edge_count = 0;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            uint8_t add_node = 0;
            uint8_t kind = 0;

            if(cell->road_class != ROAD_NONE)
            {
                int degree = road_degree(x, y);
                if(degree != 2 || cell->bridge)
                {
                    add_node = 1;
                    kind = cell->bridge ? NAV_NODE_BRIDGE : NAV_NODE_INTERSECTION;
                }
            }

            if(cell->safe_zone_candidate)
            {
                add_node = 1;
                kind = NAV_NODE_SAFE_ZONE;
            }

            if(cell->infrastructure_type == INFRA_GATEWAY)
            {
                add_node = 1;
                kind = NAV_NODE_GATEWAY;
            }

            if(cell->building_id != ENV_NO_BUILDING && lot_adjacent_to_road(x, y, NULL, NULL))
            {
                add_node = 1;
                kind = NAV_NODE_BUILDING_ENTRANCE;
            }

            if(add_node && g_environment_summary.nav_node_count < MAX_ENV_NAV_NODES)
            {
                uint16_t node_id = g_environment_summary.nav_node_count++;
                cell->nav_node_id = node_id;
                g_nav_nodes[node_id] = (EnvironmentNavNode_t){
                    .node_id = node_id,
                    .x = (int16_t)x,
                    .y = (int16_t)y,
                    .kind = kind
                };
            }
        }
    }

    for(uint16_t i = 0; i < g_environment_summary.nav_node_count; i++)
    {
        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};
        EnvironmentNavNode_t *node = &g_nav_nodes[i];

        for(int dir = 0; dir < 4; dir++)
        {
            int x = node->x + dx[dir];
            int y = node->y + dy[dir];
            uint16_t cost = 0;

            while(in_bounds(x, y) && g_environment[x][y].road_class != ROAD_NONE)
            {
                cost += g_environment[x][y].travel_cost;
                if(g_environment[x][y].nav_node_id != ENV_NO_NAV_NODE &&
                   g_environment[x][y].nav_node_id != node->node_id)
                {
                    if(g_environment_summary.nav_edge_count < MAX_ENV_NAV_EDGES)
                    {
                        g_nav_edges[g_environment_summary.nav_edge_count++] = (EnvironmentNavEdge_t){
                            .from_node = node->node_id,
                            .to_node = g_environment[x][y].nav_node_id,
                            .cost = cost,
                            .road_class = g_environment[x][y].road_class,
                            .blocked = g_environment[x][y].road_blocked
                        };
                    }
                    break;
                }

                x += dx[dir];
                y += dy[dir];
            }
        }
    }

    g_environment_summary.validation_passed = 1;
}

void generate_environment(uint32_t seed)
{
    random_seed_stream(STREAM_ENVIRONMENT, derive_seed(seed, STREAM_ENVIRONMENT));
    reset_generation_state();
    g_environment_summary.seed = seed;

    stage_generate_terrain();
    stage_generate_districts();
    stage_generate_roads();
    stage_generate_buildings();
    stage_generate_infrastructure();
    stage_validate_and_build_navigation();

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            if(g_environment[x][y].navigable)
                g_environment_summary.navigable_cell_count++;
        }
    }

    g_environment_summary.generated = 1;
}

static uint8_t earthquake_damage_at(int x, int y, uint8_t district, uint8_t quality, uint8_t vulnerability, uint8_t height_factor)
{
    int distance = abs_i(x - g_environment_summary.earthquake_epicenter_x) + abs_i(y - g_environment_summary.earthquake_epicenter_y);
    int terrain_factor = (100 - g_environment[x][y].soil_condition) / 2 + g_environment[x][y].terrain_slope / 3;
    int attenuation = distance * 3;
    int raw_damage =
        (int)g_environment_summary.earthquake_intensity +
        (int)district_seismic_vulnerability(district) +
        terrain_factor +
        vulnerability +
        height_factor -
        (int)(quality / 2) -
        attenuation;

    raw_damage += (int)(coord_hash(x, y, 991u) % 13u) - 6;
    return clamp_u8(raw_damage, ENV_DAMAGE_NONE, ENV_DAMAGE_DESTROYED);
}

/* Stage 8 and 9: Earthquake and secondary-hazard states derive from terrain,
 * soil, structure quality, and network fragility rather than random collapse. */
static void apply_building_damage(void)
{
    g_environment_summary.damaged_building_count = 0;

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        uint8_t damage = earthquake_damage_at(
            building->loc_x,
            building->loc_y,
            building->district,
            building->construction_quality,
            building->seismic_vulnerability,
            building->height_m / 3);

        building->damage_state = damage;

        if(damage >= ENV_DAMAGE_MINOR)
            g_environment_summary.damaged_building_count++;

        if(damage >= ENV_DAMAGE_MODERATE)
            building->hazard_flags |= ENV_HAZARD_COLLAPSE_RISK;

        if(damage >= ENV_DAMAGE_MODERATE && building->gas_infrastructure)
            building->hazard_flags |= ENV_HAZARD_GAS;

        if(damage >= ENV_DAMAGE_SEVERE && building->fire_susceptibility > 60)
            building->hazard_flags |= ENV_HAZARD_FIRE;

        for(int x = building->loc_x - (building->footprint_width / 2); x <= building->loc_x + (building->footprint_width / 2); x++)
        {
            for(int y = building->loc_y - (building->footprint_height / 2); y <= building->loc_y + (building->footprint_height / 2); y++)
            {
                if(!in_bounds(x, y) || g_environment[x][y].building_id != building->building_id)
                    continue;

                g_environment[x][y].base_safety = clamp_u8((int)g_environment[x][y].base_safety - (damage / 4), 0, 100);
                g_environment[x][y].travel_cost = clamp_u8((int)g_environment[x][y].travel_cost + (damage / 25), 1, 20);
                g_environment[x][y].hazard_flags |= building->hazard_flags;
            }
        }
    }
}

static void apply_road_damage(void)
{
    g_environment_summary.blocked_road_count = 0;
    g_environment_summary.navigable_cell_count = 0;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];

            if(cell->road_class == ROAD_NONE)
            {
                if(cell->navigable)
                    g_environment_summary.navigable_cell_count++;
                continue;
            }

            cell->road_damage = earthquake_damage_at(
                x,
                y,
                cell->district,
                road_quality(cell->road_class),
                cell->bridge ? 18 : 0,
                0);

            if(cell->road_damage >= ENV_DAMAGE_SEVERE &&
               !cell->safe_zone_candidate &&
               cell->infrastructure_type != INFRA_GATEWAY)
            {
                cell->road_blocked = 1;
                cell->travel_cost = 18;
                g_environment_summary.blocked_road_count++;
            }
            else if(cell->road_damage >= ENV_DAMAGE_MODERATE)
            {
                cell->travel_cost = clamp_u8((int)cell->travel_cost + 4, 1, 15);
            }

            cell->base_safety = clamp_u8((int)cell->base_safety - (cell->road_damage / 6), 0, 100);

            if(cell->navigable)
                g_environment_summary.navigable_cell_count++;
        }
    }
}

static void apply_infrastructure_damage(void)
{
    g_environment_summary.damaged_infrastructure_count = 0;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            uint8_t quality;

            if(cell->infrastructure_type == INFRA_NONE)
                continue;

            quality =
                cell->infrastructure_type == INFRA_SAFE_ZONE ? 90 :
                cell->infrastructure_type == INFRA_GATEWAY ? 74 :
                cell->infrastructure_type == INFRA_BRIDGE ? 64 :
                cell->infrastructure_type == INFRA_INTERSECTION ? 70 : 58;

            cell->infrastructure_damage = earthquake_damage_at(
                x,
                y,
                cell->district,
                quality,
                cell->bridge ? 22 : 0,
                0);

            if(cell->infrastructure_damage >= ENV_DAMAGE_MINOR)
                g_environment_summary.damaged_infrastructure_count++;

            if(cell->infrastructure_damage >= ENV_DAMAGE_MODERATE)
            {
                cell->base_safety = clamp_u8((int)cell->base_safety - (cell->infrastructure_damage / 4), 0, 100);
                cell->travel_cost = clamp_u8((int)cell->travel_cost + (cell->infrastructure_damage / 30), 1, 18);
            }
        }
    }
}

void environment_apply_earthquake(uint32_t seed)
{
    random_seed_stream(STREAM_ENVIRONMENT, derive_seed(seed, STREAM_ENVIRONMENT));

    g_environment_summary.earthquake_epicenter_x =
        (int16_t)random_range_stream(STREAM_ENVIRONMENT, GRID_WIDTH / 5, (GRID_WIDTH * 4) / 5);
    g_environment_summary.earthquake_epicenter_y =
        (int16_t)random_range_stream(STREAM_ENVIRONMENT, GRID_HEIGHT / 5, (GRID_HEIGHT * 4) / 5);
    g_environment_summary.earthquake_intensity =
        (uint8_t)random_range_stream(STREAM_ENVIRONMENT, 64, 90);

    apply_building_damage();
    apply_road_damage();
    apply_infrastructure_damage();
    g_environment_summary.earthquake_applied = 1;
}

/* Post-earthquake reachability repair for hazard placements.
 * Safe zones and incidents are placed in generate_world() BEFORE
 * environment_apply_earthquake() runs (an intentional ordering - see
 * simulation.c / TRON_FIX_PLAN), so quake road/building damage can strand
 * one on a lot with no adjacent reachable road. Mirror the building/
 * district pass in stage_validate_and_build_navigation(): build the
 * reachable map, and for any stranded location carve a repair path to the
 * nearest road and rebuild the map.
 *
 * World-gen only. Extending this to mid-run incident spawns was tried and
 * reverted: road_reachable_map()'s road-only connectivity model doesn't
 * match the pathfinder's navigable-cell model, and carve_repair_to_
 * nearest_road() targets the nearest road without checking it's reachable,
 * so on a post-quake fragmented network it "repairs" incidents onto
 * stranded road fragments and inflates re-bid/abandon churn. */
void environment_validate_hazard_reachability(void)
{
    uint8_t reachable[GRID_WIDTH][GRID_HEIGHT];

    road_reachable_map(reachable);

    for(int i = 0; i < g_num_safe_zones; i++)
    {
        SafeZone_t *zone = &g_safezones[i];
        if(!lot_reachable(reachable, zone->loc_x, zone->loc_y))
        {
            carve_repair_to_nearest_road(zone->loc_x, zone->loc_y);
            road_reachable_map(reachable);
        }
    }

    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *incident = &g_incidents[i];
        if(!incident->slot_occupied || incident->status == INC_RESOLVED)
            continue;
        if(!lot_reachable(reachable, incident->loc_x, incident->loc_y))
        {
            carve_repair_to_nearest_road(incident->loc_x, incident->loc_y);
            road_reachable_map(reachable);
        }
    }
}

#else /* !ENVIRONMENT_HAS_WORLD_STORAGE */

/* A board never builds a world (see environment.h); these exist only
 * because generate_world() is compiled into the board image too. Leaving
 * g_environment_summary.generated at 0 is exactly the board's state. */
void generate_environment(uint32_t seed)
{
    (void)seed;
}

void environment_apply_earthquake(uint32_t seed)
{
    (void)seed;
}

void environment_validate_hazard_reachability(void)
{
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */

uint8_t environment_cell_is_navigable(int x, int y)
{
    uint8_t map_cell;

    if(!in_bounds(x, y))
        return 0;
    if(!g_environment_summary.generated)
    {
        /* A board: no environment of its own, routes on the laptop's
         * map once that row has arrived (mission_link.h section 5c). */
        if(mission_link_map_cell(x, y, &map_cell))
            return map_cell != MISSION_LINK_MAP_BLOCKED;
        return 1;
    }
#if ENVIRONMENT_HAS_WORLD_STORAGE
    return g_environment[x][y].navigable;
#else
    return 1;   /* not reached: never generated on a board */
#endif
}

uint8_t environment_cell_is_flood(int x, int y)
{
    if(!in_bounds(x, y))
        return 0;
    if(!g_environment_summary.generated)
        return 0;
#if ENVIRONMENT_HAS_WORLD_STORAGE
    return g_environment[x][y].flood;
#else
    return 0;   /* not reached: never generated on a board */
#endif
}

uint8_t environment_cell_travel_cost(int x, int y)
{
    uint8_t map_cell;

    if(!in_bounds(x, y))
        return 0;
    if(!g_environment_summary.generated)
    {
        /* See environment_cell_is_navigable(). A blocked cell's cost is
         * never used for routing; 1 keeps the old default for it. */
        if(mission_link_map_cell(x, y, &map_cell) && map_cell != MISSION_LINK_MAP_BLOCKED)
            return map_cell;
        return 1;
    }
#if ENVIRONMENT_HAS_WORLD_STORAGE
    return g_environment[x][y].travel_cost;
#else
    return 1;   /* not reached: never generated on a board */
#endif
}

uint8_t environment_cell_base_safety(int x, int y)
{
    if(!in_bounds(x, y))
        return 100;
    if(!g_environment_summary.generated)
        return 100;
#if ENVIRONMENT_HAS_WORLD_STORAGE
    return g_environment[x][y].base_safety;
#else
    return 100; /* not reached: never generated on a board */
#endif
}

/* Non-atomic read-modify-write on g_environment_summary's counters, no
 * lock. Confirmed safe today, not just assumed: every caller
 * (core/fault_injector.c's inject_environment_fault()/recover_
 * environment_fault()) runs from fault_injector_step(), called from
 * sim_tick() in the same single simulation task both board targets
 * create - never an ISR, never a second task. Revisit if this is ever
 * called from a real async fault-injection callback instead. */
void environment_set_cell_operational_state(int x, int y, uint8_t travel_cost, uint8_t navigable, uint8_t road_blocked)
{
    if(!in_bounds(x, y))
        return;

#if !ENVIRONMENT_HAS_WORLD_STORAGE
    /* Only fault_injector_step() calls this, and it never runs on a
     * board (sim_tick()); board routing reads the laptop's map. */
    (void)travel_cost;
    (void)navigable;
    (void)road_blocked;
#else
    EnvironmentCell_t *cell = &g_environment[x][y];

    if(g_environment_summary.generated)
    {
        if(cell->road_blocked && !road_blocked && g_environment_summary.blocked_road_count > 0)
            g_environment_summary.blocked_road_count--;
        else if(!cell->road_blocked && road_blocked)
            g_environment_summary.blocked_road_count++;

        if(cell->navigable && !navigable && g_environment_summary.navigable_cell_count > 0)
            g_environment_summary.navigable_cell_count--;
        else if(!cell->navigable && navigable)
            g_environment_summary.navigable_cell_count++;
    }

    cell->travel_cost = travel_cost;
    cell->navigable = navigable;
    cell->road_blocked = road_blocked;
#endif
}

void environment_register_safe_zone(uint8_t safezone_id, int16_t x, int16_t y)
{
    (void)safezone_id;
    if(!in_bounds(x, y))
        return;

#if ENVIRONMENT_HAS_WORLD_STORAGE
    g_environment[x][y].infrastructure_type = INFRA_SAFE_ZONE;
    g_environment[x][y].safe_zone_candidate = 1;
    g_environment[x][y].road_blocked = 0;
    g_environment[x][y].navigable = 1;
#endif
}

void environment_register_gateway(uint8_t gateway_id, int16_t x, int16_t y)
{
    if(!in_bounds(x, y))
        return;

    g_gateway_sites[gateway_id % NUM_EVACUATION_GATEWAYS] = (GatewaySite_t){
        .valid = 1,
        .x = x,
        .y = y
    };
#if ENVIRONMENT_HAS_WORLD_STORAGE
    g_environment[x][y].infrastructure_type = INFRA_GATEWAY;
    g_environment[x][y].road_blocked = 0;
    g_environment[x][y].navigable = 1;
#endif
}

uint8_t environment_find_safe_zone_site(uint8_t index, uint8_t total, int16_t *x, int16_t *y)
{
    uint16_t seen = 0;
    uint16_t target;

    if(x == NULL || y == NULL || total == 0 || g_environment_summary.safe_zone_candidate_count == 0)
        return 0;

    target = (uint16_t)(((uint32_t)index * g_environment_summary.safe_zone_candidate_count) / total);

#if ENVIRONMENT_HAS_WORLD_STORAGE
    for(int cx = 0; cx < GRID_WIDTH; cx++)
    {
        for(int cy = 0; cy < GRID_HEIGHT; cy++)
        {
            if(!g_environment[cx][cy].safe_zone_candidate)
                continue;
            if(seen++ == target)
            {
                *x = (int16_t)cx;
                *y = (int16_t)cy;
                return 1;
            }
        }
    }
#else
    (void)seen;
    (void)target;   /* no candidates: count is 0, returned above */
#endif

    return 0;
}

uint8_t environment_find_gateway_site(uint8_t index, int16_t *x, int16_t *y)
{
    if(x == NULL || y == NULL)
        return 0;

    if(g_gateway_sites[index % NUM_EVACUATION_GATEWAYS].valid)
    {
        *x = g_gateway_sites[index % NUM_EVACUATION_GATEWAYS].x;
        *y = g_gateway_sites[index % NUM_EVACUATION_GATEWAYS].y;
        return 1;
    }

    return 0;
}

#if ENVIRONMENT_HAS_WORLD_STORAGE
static uint8_t building_can_spawn_incident(const EnvironmentBuilding_t *building, IncidentType_t type)
{
    if(building == NULL || building->building_id == ENV_NO_BUILDING)
        return 0;
    if(building->spawned_incident_mask & ENV_INCIDENT_BIT(type))
        return 0;

    switch(type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
            return building->damage_state >= ENV_DAMAGE_SEVERE;

        case INCIDENT_CONFINED_SPACE:
            return building->damage_state >= ENV_DAMAGE_MODERATE &&
                   building->damage_state < ENV_DAMAGE_DESTROYED;

        case INCIDENT_CHEMICAL_EXPOSURE:
        case INCIDENT_GAS_LEAK:
            return building->damage_state >= ENV_DAMAGE_MODERATE &&
                   (building->building_type == BUILDING_INDUSTRIAL ||
                    building->gas_infrastructure);

        case INCIDENT_MASS_CASUALTY:
            return building->damage_state >= ENV_DAMAGE_MODERATE &&
                   (building->building_type == BUILDING_RESIDENTIAL ||
                    building->building_type == BUILDING_APARTMENT ||
                    building->building_type == BUILDING_MEDICAL ||
                    building->building_type == BUILDING_CIVIC ||
                    building->building_type == BUILDING_EDUCATION);

        case INCIDENT_ACTIVE_FIRE:
            return building->damage_state >= ENV_DAMAGE_MODERATE &&
                   building->building_type != BUILDING_MEDICAL;

        case INCIDENT_UNSEARCHED_SECTOR:
        case INCIDENT_SURVIVOR_SIGNAL:
            return building->damage_state >= ENV_DAMAGE_MINOR &&
                   building->district != DISTRICT_RESCUE_BASE;

        default:
            return 0;
    }
}

static uint8_t cell_can_spawn_incident(const EnvironmentCell_t *cell, IncidentType_t type)
{
    if(cell == NULL)
        return 0;
    if(cell->spawned_incident_mask & ENV_INCIDENT_BIT(type))
        return 0;

    switch(type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
            return cell->road_blocked || cell->infrastructure_damage >= ENV_DAMAGE_SEVERE;

        case INCIDENT_GAS_LEAK:
        case INCIDENT_CHEMICAL_EXPOSURE:
            return cell->infrastructure_damage >= ENV_DAMAGE_MODERATE &&
                   cell->district == DISTRICT_INDUSTRIAL;

        case INCIDENT_MASS_CASUALTY:
            return cell->infrastructure_damage >= ENV_DAMAGE_MODERATE &&
                   (cell->infrastructure_type == INFRA_SAFE_ZONE ||
                    cell->district == DISTRICT_GOVERNMENT ||
                    cell->district == DISTRICT_MEDICAL ||
                    cell->district == DISTRICT_EDUCATION);

        case INCIDENT_ACTIVE_FIRE:
            return cell->infrastructure_damage >= ENV_DAMAGE_SEVERE ||
                   (cell->road_damage >= ENV_DAMAGE_SEVERE && cell->district == DISTRICT_INDUSTRIAL);

        case INCIDENT_UNSEARCHED_SECTOR:
        case INCIDENT_SURVIVOR_SIGNAL:
            return cell->road_blocked && cell->district != DISTRICT_RESCUE_BASE;

        default:
            return 0;
    }
}

static uint16_t building_incident_score(const EnvironmentBuilding_t *building)
{
    return (uint16_t)building->damage_state +
           (uint16_t)building->risk_level +
           (uint16_t)(100 - building->construction_quality) +
           (uint16_t)building->expected_civilians;
}

static uint16_t cell_incident_score(const EnvironmentCell_t *cell)
{
    uint16_t score =
        (uint16_t)cell->road_damage +
        (uint16_t)cell->infrastructure_damage +
        (uint16_t)(100 - cell->base_safety);

    if(cell->road_blocked)
        score += 40;

    if(cell->bridge)
        score += 20;

    return score;
}

static void mark_building_incident_hazard(EnvironmentBuilding_t *building, IncidentType_t type)
{
    if(building == NULL)
        return;

    switch(type)
    {
        case INCIDENT_ACTIVE_FIRE:
            building->hazard_flags |= ENV_HAZARD_FIRE;
            break;
        case INCIDENT_GAS_LEAK:
        case INCIDENT_CHEMICAL_EXPOSURE:
            building->hazard_flags |= ENV_HAZARD_GAS;
            break;
        case INCIDENT_STRUCTURAL_COLLAPSE:
        case INCIDENT_CONFINED_SPACE:
            building->hazard_flags &= (uint8_t)~ENV_HAZARD_COLLAPSE_RISK;
            break;
        default:
            break;
    }
}

static void mark_cell_incident_hazard(EnvironmentCell_t *cell, IncidentType_t type)
{
    if(cell == NULL)
        return;

    switch(type)
    {
        case INCIDENT_ACTIVE_FIRE:
            cell->hazard_flags |= ENV_HAZARD_FIRE;
            break;
        case INCIDENT_GAS_LEAK:
        case INCIDENT_CHEMICAL_EXPOSURE:
            cell->hazard_flags |= ENV_HAZARD_GAS;
            break;
        case INCIDENT_STRUCTURAL_COLLAPSE:
        case INCIDENT_CONFINED_SPACE:
            cell->hazard_flags &= (uint8_t)~ENV_HAZARD_COLLAPSE_RISK;
            break;
        default:
            break;
    }
}

static uint8_t select_damaged_building_source(IncidentType_t type, int16_t *x, int16_t *y)
{
    uint8_t found = 0;
    uint16_t best_id = ENV_NO_BUILDING;
    uint16_t best_score = 0;

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        uint16_t score;

        if(!building_can_spawn_incident(building, type))
            continue;

        score = building_incident_score(building);
        if(!found || score > best_score ||
           (score == best_score && random_range_stream(STREAM_INCIDENTS, 0, 1) == 1))
        {
            found = 1;
            best_score = score;
            best_id = i;
        }
    }

    if(!found || best_id == ENV_NO_BUILDING)
        return 0;

    g_environment_buildings[best_id].spawned_incident_mask |= ENV_INCIDENT_BIT(type);
    mark_building_incident_hazard(&g_environment_buildings[best_id], type);
    g_environment[g_environment_buildings[best_id].loc_x][g_environment_buildings[best_id].loc_y].spawned_incident_mask |= ENV_INCIDENT_BIT(type);
    *x = g_environment_buildings[best_id].loc_x;
    *y = g_environment_buildings[best_id].loc_y;
    return 1;
}

static uint8_t select_damaged_cell_source(IncidentType_t type, int16_t *x, int16_t *y)
{
    uint8_t found = 0;
    int best_x = -1;
    int best_y = -1;
    uint16_t best_score = 0;

    for(int cx = 0; cx < GRID_WIDTH; cx++)
    {
        for(int cy = 0; cy < GRID_HEIGHT; cy++)
        {
            EnvironmentCell_t *cell = &g_environment[cx][cy];
            uint16_t score;

            if(!cell_can_spawn_incident(cell, type))
                continue;

            score = cell_incident_score(cell);
            if(!found || score > best_score ||
               (score == best_score && random_range_stream(STREAM_INCIDENTS, 0, 1) == 1))
            {
                found = 1;
                best_score = score;
                best_x = cx;
                best_y = cy;
            }
        }
    }

    if(!found)
        return 0;

    g_environment[best_x][best_y].spawned_incident_mask |= ENV_INCIDENT_BIT(type);
    mark_cell_incident_hazard(&g_environment[best_x][best_y], type);
    *x = (int16_t)best_x;
    *y = (int16_t)best_y;
    return 1;
}

static uint8_t select_damaged_building_source_in_zone(IncidentType_t type, uint8_t zone_id, int16_t *x, int16_t *y)
{
    uint8_t found = 0;
    uint16_t best_id = ENV_NO_BUILDING;
    uint16_t best_score = 0;

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        uint16_t score;

        if(building->district_id != zone_id)
            continue;

        if(!building_can_spawn_incident(building, type))
            continue;

        score = building_incident_score(building);
        if(!found || score > best_score ||
           (score == best_score && random_range_stream(STREAM_INCIDENTS, 0, 1) == 1))
        {
            found = 1;
            best_score = score;
            best_id = i;
        }
    }

    if(!found || best_id == ENV_NO_BUILDING)
        return 0;

    g_environment_buildings[best_id].spawned_incident_mask |= ENV_INCIDENT_BIT(type);
    mark_building_incident_hazard(&g_environment_buildings[best_id], type);
    g_environment[g_environment_buildings[best_id].loc_x][g_environment_buildings[best_id].loc_y].spawned_incident_mask |= ENV_INCIDENT_BIT(type);
    *x = g_environment_buildings[best_id].loc_x;
    *y = g_environment_buildings[best_id].loc_y;
    return 1;
}

static uint8_t select_damaged_cell_source_in_zone(IncidentType_t type, uint8_t zone_id, int16_t *x, int16_t *y)
{
    uint8_t found = 0;
    int best_x = -1;
    int best_y = -1;
    uint16_t best_score = 0;

    for(int cx = 0; cx < GRID_WIDTH; cx++)
    {
        for(int cy = 0; cy < GRID_HEIGHT; cy++)
        {
            EnvironmentCell_t *cell = &g_environment[cx][cy];
            uint16_t score;

            if(cell->district_id != zone_id)
                continue;

            if(!cell_can_spawn_incident(cell, type))
                continue;

            score = cell_incident_score(cell);
            if(!found || score > best_score ||
               (score == best_score && random_range_stream(STREAM_INCIDENTS, 0, 1) == 1))
            {
                found = 1;
                best_score = score;
                best_x = cx;
                best_y = cy;
            }
        }
    }

    if(!found)
        return 0;

    g_environment[best_x][best_y].spawned_incident_mask |= ENV_INCIDENT_BIT(type);
    mark_cell_incident_hazard(&g_environment[best_x][best_y], type);
    *x = (int16_t)best_x;
    *y = (int16_t)best_y;
    return 1;
}

static EnvironmentBuilding_t *building_at(int x, int y)
{
    uint16_t building_id;

    if(!in_bounds(x, y))
        return NULL;

    building_id = g_environment[x][y].building_id;
    if(building_id == ENV_NO_BUILDING || building_id >= g_environment_summary.building_count)
        return NULL;
    return &g_environment_buildings[building_id];
}

static void propagate_fire(uint32_t tick)
{
    uint8_t ignite[MAX_ENV_BUILDINGS] = {0};
    static const int dx[4] = {1, -1, 0, 0};
    static const int dy[4] = {0, 0, 1, -1};

    if(!g_hazard_config.fire_spread_enabled ||
       g_hazard_config.fire_spread_interval_ticks == 0 ||
       tick % g_hazard_config.fire_spread_interval_ticks != 0)
    {
        return;
    }

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *source = &g_environment_buildings[i];
        if(!(source->hazard_flags & ENV_HAZARD_FIRE))
            continue;

        for(int dir = 0; dir < 4; dir++)
        {
            EnvironmentBuilding_t *target = building_at(source->loc_x + dx[dir], source->loc_y + dy[dir]);
            uint8_t probability;

            if(target == NULL ||
               target->building_id == source->building_id ||
               (target->hazard_flags & ENV_HAZARD_FIRE) ||
               (target->spawned_incident_mask & ENV_INCIDENT_BIT(INCIDENT_ACTIVE_FIRE)))
            {
                continue;
            }

            probability =
                g_hazard_config.fire_spread_probability +
                (target->damage_state / 10) +
                (target->risk_level / 20) +
                (target->fire_susceptibility / 10);

            if(source->district == DISTRICT_INDUSTRIAL)
                probability += 8;
            if(probability > 95)
                probability = 95;

            if(deterministic_percent_roll(tick, source->building_id, target->building_id, 11) < probability)
            {
                if(target->building_id < MAX_ENV_BUILDINGS)
                    ignite[target->building_id] = 1;
            }
        }
    }

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        if(i >= MAX_ENV_BUILDINGS || !ignite[i])
            continue;

        g_environment_buildings[i].hazard_flags |= ENV_HAZARD_FIRE;
        g_environment_buildings[i].spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_ACTIVE_FIRE);
        g_environment[g_environment_buildings[i].loc_x][g_environment_buildings[i].loc_y].spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_ACTIVE_FIRE);

        if(queue_hazard_event(INCIDENT_ACTIVE_FIRE, g_environment_buildings[i].loc_x, g_environment_buildings[i].loc_y))
            g_environment_summary.fire_spread_count++;
    }
}

static void escalate_gas_leaks(uint32_t tick)
{
    if(!g_hazard_config.gas_escalation_enabled ||
       g_hazard_config.gas_escalation_interval_ticks == 0 ||
       tick % g_hazard_config.gas_escalation_interval_ticks != 0)
    {
        return;
    }

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        EnvironmentCell_t *cell;
        uint8_t probability;

        if(!(building->hazard_flags & ENV_HAZARD_GAS) ||
           (building->spawned_incident_mask & ENV_INCIDENT_BIT(INCIDENT_STRUCTURAL_COLLAPSE)))
        {
            continue;
        }

        probability =
            g_hazard_config.gas_explosion_probability +
            (building->damage_state / 12) +
            (building->risk_level / 25) +
            (building->gas_infrastructure ? 10 : 0);

        if(building->building_type == BUILDING_INDUSTRIAL)
            probability += 10;
        if(probability > 90)
            probability = 90;

        if(deterministic_percent_roll(tick, building->building_id, building->loc_x, 23) >= probability)
            continue;

        building->hazard_flags &= (uint8_t)~ENV_HAZARD_GAS;
        building->hazard_flags |= ENV_HAZARD_FIRE;
        building->damage_state = ENV_DAMAGE_DESTROYED;
        building->spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_STRUCTURAL_COLLAPSE);
        building->spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_ACTIVE_FIRE);

        cell = &g_environment[building->loc_x][building->loc_y];
        cell->base_safety = clamp_u8((int)cell->base_safety - 35, 0, 100);
        cell->travel_cost = clamp_u8((int)cell->travel_cost + 6, 1, 20);
        cell->spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_STRUCTURAL_COLLAPSE);
        cell->spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_ACTIVE_FIRE);

        if(queue_hazard_event(INCIDENT_STRUCTURAL_COLLAPSE, building->loc_x, building->loc_y))
            g_environment_summary.gas_explosion_count++;
        queue_hazard_event(INCIDENT_ACTIVE_FIRE, building->loc_x, building->loc_y);
    }
}

static void apply_aftershocks(uint32_t tick)
{
    if(!g_hazard_config.aftershock_enabled ||
       g_hazard_config.aftershock_interval_ticks == 0 ||
       tick % g_hazard_config.aftershock_interval_ticks != 0)
    {
        return;
    }

    for(uint16_t i = 0; i < g_environment_summary.building_count; i++)
    {
        EnvironmentBuilding_t *building = &g_environment_buildings[i];
        EnvironmentCell_t *cell;
        uint8_t probability;

        if(building->damage_state < ENV_DAMAGE_MODERATE ||
           (building->spawned_incident_mask & ENV_INCIDENT_BIT(INCIDENT_STRUCTURAL_COLLAPSE)))
        {
            continue;
        }

        probability =
            g_hazard_config.aftershock_collapse_probability +
            (building->damage_state / 8) +
            ((100 - building->construction_quality) / 10) +
            (building->seismic_vulnerability / 8);

        if(probability > 85)
            probability = 85;

        if(deterministic_percent_roll(tick, building->building_id, building->damage_state, 37) >= probability)
            continue;

        building->damage_state = ENV_DAMAGE_DESTROYED;
        building->hazard_flags &= (uint8_t)~ENV_HAZARD_COLLAPSE_RISK;
        building->spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_STRUCTURAL_COLLAPSE);

        cell = &g_environment[building->loc_x][building->loc_y];
        cell->base_safety = clamp_u8((int)cell->base_safety - 28, 0, 100);
        cell->travel_cost = clamp_u8((int)cell->travel_cost + 5, 1, 20);
        cell->spawned_incident_mask |= ENV_INCIDENT_BIT(INCIDENT_STRUCTURAL_COLLAPSE);

        if(queue_hazard_event(INCIDENT_STRUCTURAL_COLLAPSE, building->loc_x, building->loc_y))
        {
            g_environment_summary.aftershock_collapse_count++;

            /* Cross-module increment on search_zones.c-owned state, with
               no lock - confirmed safe today, not just assumed: both
               board targets create exactly one simulation task
               (tk_cre_tsk() in main_board.c) and nothing else ever
               touches g_search_zones concurrently - the same single-
               task model documented in comms_transport_inbox_push()'s
               concurrency-invariant comment. Revisit if a second task
               is ever added that also touches search-zone state. */
            if(building->district_id < NUM_SEARCH_ZONES)
                g_search_zones[building->district_id].aftershock_hits_since_swept++;
        }
    }
}

static void degrade_blocked_roads(uint32_t tick)
{
    if(!g_hazard_config.road_degradation_enabled ||
       g_hazard_config.road_degradation_interval_ticks == 0 ||
       tick % g_hazard_config.road_degradation_interval_ticks != 0)
    {
        return;
    }

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            uint8_t old_cost;

            if(!cell->road_blocked)
                continue;

            old_cost = cell->travel_cost;
            cell->travel_cost = clamp_u8((int)cell->travel_cost + g_hazard_config.road_degradation_amount, 1, 25);
            if(cell->travel_cost != old_cost)
                g_environment_summary.road_degradation_count++;
        }
    }
}

void environment_update_hazard_propagation(uint32_t tick)
{
    if(!g_environment_summary.generated || !g_environment_summary.earthquake_applied)
        return;

    propagate_fire(tick);
    escalate_gas_leaks(tick);
    apply_aftershocks(tick);
    degrade_blocked_roads(tick);
}
#else /* !ENVIRONMENT_HAS_WORLD_STORAGE */

void environment_update_hazard_propagation(uint32_t tick)
{
    (void)tick;   /* nothing generated, nothing to spread */
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */

uint8_t environment_pop_pending_hazard_event(IncidentType_t *type, int16_t *x, int16_t *y)
{
    if(type == NULL || x == NULL || y == NULL)
        return 0;

    for(uint8_t i = 0; i < MAX_ENV_PENDING_HAZARDS; i++)
    {
        if(!g_pending_hazard_events[i].occupied)
            continue;

        *type = (IncidentType_t)g_pending_hazard_events[i].incident_type;
        *x = g_pending_hazard_events[i].loc_x;
        *y = g_pending_hazard_events[i].loc_y;
        g_pending_hazard_events[i] = (EnvironmentHazardEvent_t){0};
        return 1;
    }

    return 0;
}

#if ENVIRONMENT_HAS_WORLD_STORAGE
uint8_t environment_find_incident_site(IncidentType_t type, int16_t *x, int16_t *y)
{
    if(x == NULL || y == NULL)
        return 0;

    switch(type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
            if(select_damaged_building_source(type, x, y))
                return 1;
            return select_damaged_cell_source(type, x, y);

        case INCIDENT_GAS_LEAK:
        case INCIDENT_CHEMICAL_EXPOSURE:
        case INCIDENT_MASS_CASUALTY:
        case INCIDENT_ACTIVE_FIRE:
        case INCIDENT_UNSEARCHED_SECTOR:
        case INCIDENT_SURVIVOR_SIGNAL:
        case INCIDENT_CONFINED_SPACE:
            if(select_damaged_building_source(type, x, y))
                return 1;
            return select_damaged_cell_source(type, x, y);

        default:
            return 0;
    }
}

/* Same site-selection rules as environment_find_incident_site(), but
 * scoped to a single search zone - used when a Search team's sweep of
 * that zone "discovers" an incident, rather than the periodic
 * whole-map generator picking anywhere on the grid. */
uint8_t environment_find_incident_site_in_zone(IncidentType_t type, uint8_t zone_id, int16_t *x, int16_t *y)
{
    if(x == NULL || y == NULL)
        return 0;

    switch(type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
        case INCIDENT_GAS_LEAK:
        case INCIDENT_CHEMICAL_EXPOSURE:
        case INCIDENT_MASS_CASUALTY:
        case INCIDENT_ACTIVE_FIRE:
        case INCIDENT_UNSEARCHED_SECTOR:
        case INCIDENT_SURVIVOR_SIGNAL:
        case INCIDENT_CONFINED_SPACE:
            if(select_damaged_building_source_in_zone(type, zone_id, x, y))
                return 1;
            return select_damaged_cell_source_in_zone(type, zone_id, x, y);

        default:
            return 0;
    }
}
#else /* !ENVIRONMENT_HAS_WORLD_STORAGE */

/* Incidents are placed by the laptop and reach a board as
 * MissionLinkIncident_t frames; a board has no sites to offer. */
uint8_t environment_find_incident_site(IncidentType_t type, int16_t *x, int16_t *y)
{
    (void)type;
    (void)x;
    (void)y;
    return 0;
}

uint8_t environment_find_incident_site_in_zone(IncidentType_t type, uint8_t zone_id, int16_t *x, int16_t *y)
{
    (void)type;
    (void)zone_id;
    (void)x;
    (void)y;
    return 0;
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */

const char *environment_district_name(uint8_t district)
{
    switch(district)
    {
        case DISTRICT_RESCUE_BASE: return "RESCUE_BASE";
        case DISTRICT_RESIDENTIAL: return "RESIDENTIAL";
        case DISTRICT_COMMERCIAL:  return "COMMERCIAL";
        case DISTRICT_INDUSTRIAL:  return "INDUSTRIAL";
        case DISTRICT_MEDICAL:     return "MEDICAL";
        case DISTRICT_GOVERNMENT:  return "GOVERNMENT";
        case DISTRICT_EDUCATION:   return "EDUCATION";
        case DISTRICT_MIXED_USE:   return "MIXED_USE";
        case DISTRICT_PARK:        return "PARK";
        case DISTRICT_WATERFRONT:  return "WATERFRONT";
        default:                   return "UNKNOWN";
    }
}

const char *environment_road_name(uint8_t road_class)
{
    switch(road_class)
    {
        case ROAD_SERVICE:   return "SERVICE";
        case ROAD_LOCAL:     return "LOCAL";
        case ROAD_COLLECTOR: return "COLLECTOR";
        case ROAD_ARTERIAL:  return "ARTERIAL";
        default:             return "NONE";
    }
}
