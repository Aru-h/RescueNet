#include "road_knowledge.h"
#include "path.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "environment.h"

static uint8_t path_in_bounds(int x, int y)
{
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

/* 8-direction movement. A* runs in tenths of a cost unit so a diagonal
 * step can cost 1.4x the entered cell's travel cost exactly (14 vs 10)
 * without integer rounding making a cost-1 diagonal as cheap as a
 * straight step. Every cost this file returns is rounded back to whole
 * units, so callers keep the units they always had. A team still moves
 * exactly one cell per tick, diagonal or not. */
#define PATH_COST_SCALE 10u
#define PATH_STRAIGHT_WEIGHT 10u
#define PATH_DIAGONAL_WEIGHT 14u
#define PATH_G_UNREACHED UINT32_MAX

/* --- Intra-call path cost cache ---------------------------------------
 * Direct-mapped, fixed-size table. A collision just means a cache miss
 * (the incoming key overwrites whatever was there) -- never a wrong
 * answer, since every lookup verifies the full key before trusting a
 * cached entry. Only active between path_cache_begin()/path_cache_end(),
 * so any call made outside that span always computes fresh, exactly as
 * before this change. */
#define PATH_CACHE_SIZE 1024u

typedef struct
{
    uint8_t occupied;
    int16_t start_x, start_y, dest_x, dest_y;
    uint8_t equipment_flags;
    int32_t cost;           /* tenths (PATH_COST_SCALE); -1 = no route */
    int16_t next_x, next_y;
    uint16_t steps;         /* cells moved along the route (see g_path_last_steps) */
} PathCacheEntry_t;

static PathCacheEntry_t g_path_cache[PATH_CACHE_SIZE];
static uint8_t g_path_cache_active = 0;
static const uint8_t *g_path_extra_cost = NULL;

void path_set_extra_cost(const uint8_t *grid)
{
    g_path_extra_cost = grid;
}

/* Number of moves (one per tick, diagonal or not) on the route the most
 * recent path_search() call found - set on a fresh search and restored on
 * a cache hit, so find_path_steps() is right either way. Like g_path_nodes
 * this is shared state: same single-task caveat. */
static uint16_t g_path_last_steps;

void path_cache_begin(void)
{
    memset(g_path_cache, 0, sizeof(g_path_cache));
    g_path_cache_active = 1;
}

void path_cache_end(void)
{
    g_path_cache_active = 0;
}

static uint32_t path_cache_hash(int sx, int sy, int dx, int dy, uint8_t eq)
{
    uint32_t h = (uint32_t)sx;
    h = h * 131u + (uint32_t)sy;
    h = h * 131u + (uint32_t)dx;
    h = h * 131u + (uint32_t)dy;
    h = h * 131u + (uint32_t)eq;
    return h % PATH_CACHE_SIZE;
}

static uint8_t path_cache_lookup(
    int sx, int sy, int dx, int dy, uint8_t eq,
    int *out_cost, int *out_next_x, int *out_next_y)
{
    if(!g_path_cache_active || g_path_extra_cost != NULL)
        return 0;

    uint32_t slot = path_cache_hash(sx, sy, dx, dy, eq);
    PathCacheEntry_t *e = &g_path_cache[slot];

    if(!e->occupied ||
       e->start_x != sx || e->start_y != sy ||
       e->dest_x != dx || e->dest_y != dy ||
       e->equipment_flags != eq)
        return 0;

    *out_cost = e->cost;
    *out_next_x = e->next_x;
    *out_next_y = e->next_y;
    g_path_last_steps = e->steps;
    return 1;
}

static void path_cache_store(
    int sx, int sy, int dx, int dy, uint8_t eq,
    int cost, int next_x, int next_y)
{
    if(!g_path_cache_active || g_path_extra_cost != NULL)
        return;

    uint32_t slot = path_cache_hash(sx, sy, dx, dy, eq);
    PathCacheEntry_t *e = &g_path_cache[slot];

    e->occupied = 1;
    e->steps = g_path_last_steps;
    e->start_x = (int16_t)sx;
    e->start_y = (int16_t)sy;
    e->dest_x = (int16_t)dx;
    e->dest_y = (int16_t)dy;
    e->equipment_flags = eq;
    e->cost = cost;
    e->next_x = (int16_t)next_x;
    e->next_y = (int16_t)next_y;
}

typedef struct //path
{
    uint32_t g_cost;        /* tenths (PATH_COST_SCALE) */
    uint8_t parent_x;
    uint8_t parent_y;
    uint8_t state;
} PathNode_t;

/* Promoted from function-local static to file-scope so that
 * find_next_path_step_with_equipment is safe to call from multiple
 * RTOS tasks without a shared static corrupting concurrent searches.
 * The array is fully re-initialised at the top of every call, so
 * promoting it here has no behavioural effect in single-threaded use. */
static PathNode_t g_path_nodes[GRID_WIDTH][GRID_HEIGHT];

static int get_movement_cost(int x, int y, uint8_t equipment_flags)
{
    (void)equipment_flags;

    if(!path_in_bounds(x, y))
        return -1;

    {
        /* Step 5 R1-R3: the fleet's belief about causal road damage; inactive (mode 0) by default. */
        int rk_nav, rk_cost;

        if(road_knowledge_planning_view(x, y, &rk_nav, &rk_cost))
        {
            if(!rk_nav)
                return -1;
            if(g_path_extra_cost != NULL)
                return rk_cost + (int)g_path_extra_cost[x * GRID_HEIGHT + y];
            return rk_cost;
        }
    }

    if(!environment_cell_is_navigable(x, y))
        return -1;

    if(g_path_extra_cost != NULL)
        return (int)environment_cell_travel_cost(x, y) + (int)g_path_extra_cost[x * GRID_HEIGHT + y];

    return environment_cell_travel_cost(x, y);
}

static uint8_t can_traverse_cell(int x, int y, uint8_t equipment_flags)
{
    (void)equipment_flags;

    if(!path_in_bounds(x, y))
        return 0;

    {
        int rk_nav, rk_cost;

        if(road_knowledge_planning_view(x, y, &rk_nav, &rk_cost))
            return rk_nav ? 1 : 0;
    }

    return environment_cell_is_navigable(x, y);
}

/* Octile distance in tenths. Admissible: every navigable cell costs at
 * least 1 (see get_movement_cost()), so no route to dest can be cheaper. */
static uint32_t get_heuristic(int x,int y,int dest_x,int dest_y) //path
{
    int ax = abs(x - dest_x);
    int ay = abs(y - dest_y);
    int diag = ax < ay ? ax : ay;
    int straight = (ax > ay ? ax : ay) - diag;

    return (uint32_t)straight * PATH_STRAIGHT_WEIGHT +
           (uint32_t)diag * PATH_DIAGONAL_WEIGHT;
}

/* Cost in tenths of stepping from (from_x, from_y) into the adjacent
 * cell (to_x, to_y), or -1 if that step is not allowed. A diagonal step
 * must not cut a corner: both cells it squeezes between have to be
 * navigable, otherwise a team could slip diagonally past the end of a
 * wall, a blocked road or a collapsed building. */
static int32_t path_step_cost(int from_x, int from_y, int to_x, int to_y,
                              uint8_t equipment_flags)
{
    int cell_cost = get_movement_cost(to_x, to_y, equipment_flags);

    if(cell_cost < 0)
        return -1;

    /* A navigable cell always costs at least 1 - the octile heuristic
     * above relies on it. */
    if(cell_cost < 1)
        cell_cost = 1;

    if(from_x != to_x && from_y != to_y)
    {
        if(get_movement_cost(to_x, from_y, equipment_flags) < 0 ||
           get_movement_cost(from_x, to_y, equipment_flags) < 0)
            return -1;

        return (int32_t)cell_cost * (int32_t)PATH_DIAGONAL_WEIGHT;
    }

    return (int32_t)cell_cost * (int32_t)PATH_STRAIGHT_WEIGHT;
}

/* Tenths -> whole cost units, rounded, kept clear of the UINT16_MAX
 * "unreachable" value find_path_cost() returns. */
static int path_cost_to_units(int32_t tenths)
{
    int32_t units = (tenths + (int32_t)(PATH_COST_SCALE / 2u)) / (int32_t)PATH_COST_SCALE;

    if(units > (int32_t)UINT16_MAX - 1)
        units = (int32_t)UINT16_MAX - 1;

    return (int)units;
}

/*
 * ---------------------------------------------------------------
 * Binary min-heap open-set, ordered by f_cost = g_cost + heuristic
 * ---------------------------------------------------------------
 * Replaces the flat open_list_x[]/open_list_y[] arrays' O(n) linear
 * scan for pop-min with O(log n) via a proper heap. Unlike a lazy-
 * deletion heap, this uses TRUE decrease-key: a position-index array
 * (g_heap_pos) tracks each cell's current slot in the heap, so a
 * g_cost improvement re-sifts the existing entry in place rather than
 * pushing a duplicate. Net effect: capped at exactly GRID_WIDTH*
 * GRID_HEIGHT entries (same bound as the array it replaces, no
 * oversized safety-margin array needed for stale duplicates) while
 * still getting the O(log n) pop-min win. Costs are never cached in
 * the heap itself - f_cost is always computed live from nodes[][].g_cost
 * at comparison time, so a decrease-key is just "re-sift this cell's
 * existing slot," nothing more.
 */

#define PATH_HEAP_NONE 0xFFFFu

static uint16_t g_heap[GRID_WIDTH * GRID_HEIGHT];
static uint16_t g_heap_pos[GRID_WIDTH][GRID_HEIGHT];
static int g_heap_size;

static uint16_t path_cell_index(int x, int y)
{
    return (uint16_t)(x * GRID_HEIGHT + y);
}

static void path_heap_reset(void)
{
    g_heap_size = 0;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            g_heap_pos[x][y] = PATH_HEAP_NONE;
        }
    }
}

static uint32_t path_heap_f_cost(PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT],
                                  int dest_x, int dest_y, uint16_t cell)
{
    int x = cell / GRID_HEIGHT;
    int y = cell % GRID_HEIGHT;

    return nodes[x][y].g_cost + get_heuristic(x, y, dest_x, dest_y);
}

static void path_heap_swap(int i, int j)
{
    uint16_t ci = g_heap[i];
    uint16_t cj = g_heap[j];
    int cix = ci / GRID_HEIGHT, ciy = ci % GRID_HEIGHT;
    int cjx = cj / GRID_HEIGHT, cjy = cj % GRID_HEIGHT;

    g_heap[i] = cj;
    g_heap[j] = ci;
    g_heap_pos[cix][ciy] = (uint16_t)j;
    g_heap_pos[cjx][cjy] = (uint16_t)i;
}

static void path_heap_sift_up(PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT],
                               int dest_x, int dest_y, int i)
{
    while(i > 0)
    {
        int parent = (i - 1) / 2;

        if(path_heap_f_cost(nodes, dest_x, dest_y, g_heap[i]) >=
           path_heap_f_cost(nodes, dest_x, dest_y, g_heap[parent]))
        {
            break;
        }

        path_heap_swap(i, parent);
        i = parent;
    }
}

static void path_heap_sift_down(PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT],
                                 int dest_x, int dest_y, int i)
{
    while(1)
    {
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        int smallest = i;

        if(left < g_heap_size &&
           path_heap_f_cost(nodes, dest_x, dest_y, g_heap[left]) <
           path_heap_f_cost(nodes, dest_x, dest_y, g_heap[smallest]))
        {
            smallest = left;
        }

        if(right < g_heap_size &&
           path_heap_f_cost(nodes, dest_x, dest_y, g_heap[right]) <
           path_heap_f_cost(nodes, dest_x, dest_y, g_heap[smallest]))
        {
            smallest = right;
        }

        if(smallest == i)
        {
            break;
        }

        path_heap_swap(i, smallest);
        i = smallest;
    }
}

static void path_heap_push(PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT],
                            int dest_x, int dest_y, int x, int y)
{
    int i = g_heap_size;

    g_heap[i] = path_cell_index(x, y);
    g_heap_pos[x][y] = (uint16_t)i;
    g_heap_size++;

    path_heap_sift_up(nodes, dest_x, dest_y, i);
}

static void path_heap_decrease_key(PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT],
                                    int dest_x, int dest_y, int x, int y)
{
    /* Caller already wrote the improved g_cost into nodes[][] before
       calling this - just re-establish the heap invariant from this
       cell's existing slot. */
    path_heap_sift_up(nodes, dest_x, dest_y, g_heap_pos[x][y]);
}

static int path_heap_pop_min(PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT],
                              int dest_x, int dest_y,
                              int *out_x, int *out_y)
{
    if(g_heap_size == 0)
    {
        return 0;
    }

    uint16_t root = g_heap[0];
    int rx = root / GRID_HEIGHT, ry = root % GRID_HEIGHT;

    g_heap_size--;

    if(g_heap_size > 0)
    {
        g_heap[0] = g_heap[g_heap_size];

        int lx = g_heap[0] / GRID_HEIGHT, ly = g_heap[0] % GRID_HEIGHT;
        g_heap_pos[lx][ly] = 0;

        path_heap_sift_down(nodes, dest_x, dest_y, 0);
    }

    g_heap_pos[rx][ry] = PATH_HEAP_NONE;

    *out_x = rx;
    *out_y = ry;
    return 1;
}

/* The A* search. Returns the route cost in tenths (0 when start == dest)
 * and the first step, or -1 when there is no route. */
static int32_t path_search(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    uint8_t equipment_flags,
    int *next_x,
    int *next_y) //path
{
    /* Bug fix: start_x/start_y were used raw below (nodes[start_x]
     * [start_y], path_heap_push()'s path_cell_index()) with no bounds
     * check at all, unlike dest_x/dest_y just below. A corrupted
     * caller-supplied location (e.g. a team's loc_x/loc_y written
     * straight from wire data with no validation - see mission_link.c's
     * mission_link_apply_roster()) would be an out-of-bounds write into
     * the file-scope g_path_nodes/g_heap_pos statics. Deliberately just
     * an in-bounds check, NOT can_traverse_cell() like the destination
     * gets below: a team must always be able to attempt to leave its
     * current cell even if that cell just became non-navigable (see the
     * destination check's own comment for why), so start only needs to
     * be a valid array index, not a valid place to stand. */
    if(!path_in_bounds(start_x, start_y))
    {
        return -1;
    }

    /* Only the destination needs to be traversable here - a team must
     * always be able to attempt to leave its current cell, even if that
     * cell just became hazardous/blocked (e.g. road/infrastructure damage
     * landing on a team's current position). Without this, a team standing
     * on a cell that turns non-navigable gets permanently stuck: this
     * function would refuse to plan any route at all, forever, since the
     * origin check failed before pathfinding ever started. Movement INTO
     * any other blocked cell along the way is still correctly refused by
     * get_movement_cost() in the A* expansion below. */
    if(!can_traverse_cell(dest_x, dest_y, equipment_flags))
    {
        return -1;
    }

    if(start_x == dest_x && start_y == dest_y)
    {
        g_path_last_steps = 0;
        if(next_x != NULL)
            *next_x = start_x;

        if(next_y != NULL)
            *next_y = start_y;

        return 0;
    }

    {
        int cached_cost, cached_next_x, cached_next_y;
        if(path_cache_lookup(start_x, start_y, dest_x, dest_y, equipment_flags,
                              &cached_cost, &cached_next_x, &cached_next_y))
        {
            if(cached_cost >= 0)
            {
                if(next_x != NULL)
                    *next_x = cached_next_x;
                if(next_y != NULL)
                    *next_y = cached_next_y;
            }
            return cached_cost;
        }
    }

    PathNode_t (* const nodes)[GRID_HEIGHT] = g_path_nodes;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            nodes[x][y].g_cost = PATH_G_UNREACHED;
            nodes[x][y].parent_x = (uint8_t)x;
            nodes[x][y].parent_y = (uint8_t)y;
            nodes[x][y].state = PATH_UNVISITED;
        }
    }
    /* g_cost is uint32_t in tenths: even the worst case (every cell of the
     * grid at cost 255, all diagonal) is far below UINT32_MAX. The whole-
     * unit value handed back to callers is capped below UINT16_MAX by
     * path_cost_to_units(). */
    nodes[start_x][start_y].g_cost = 0;
    nodes[start_x][start_y].parent_x = (uint8_t)start_x;
    nodes[start_x][start_y].parent_y = (uint8_t)start_y;
    nodes[start_x][start_y].state = PATH_OPEN;

    path_heap_reset();
    path_heap_push(nodes, dest_x, dest_y, start_x, start_y);

    while(1)
    {
        int current_x, current_y;

        if(!path_heap_pop_min(nodes, dest_x, dest_y, &current_x, &current_y))
        {
            path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
            return -1;
        }

        if(current_x == dest_x && current_y == dest_y)
        {
            break;
        }

        nodes[current_x][current_y].state = PATH_CLOSED;

        /* Straight neighbours first, so on an exact cost tie the
         * earlier-found straight step is kept. */
        static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        static const int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

        for(int i = 0; i < 8; i++)
        {
            int nx = current_x + dx[i];
            int ny = current_y + dy[i];

            if(!path_in_bounds(nx, ny))
                continue;

            if(nodes[nx][ny].state == PATH_CLOSED)
                continue;

            int32_t movement_cost = path_step_cost(current_x, current_y, nx, ny, equipment_flags);

            if(movement_cost < 0)
                continue;

            uint32_t new_g =
                nodes[current_x][current_y].g_cost +
                (uint32_t)movement_cost;

            if(new_g < nodes[nx][ny].g_cost)
            {
                nodes[nx][ny].g_cost = new_g;
                nodes[nx][ny].parent_x = current_x;
                nodes[nx][ny].parent_y = current_y;

                if(nodes[nx][ny].state == PATH_OPEN)
                {
                    /* Already sitting in the heap - re-sift its existing
                       slot now that g_cost improved, don't push again. */
                    path_heap_decrease_key(nodes, dest_x, dest_y, nx, ny);
                }
                else
                {
                    path_heap_push(nodes, dest_x, dest_y, nx, ny);
                }

                nodes[nx][ny].state = PATH_OPEN;
            }
        }
    }

    if(!path_in_bounds(dest_x, dest_y) ||
       nodes[dest_x][dest_y].g_cost == PATH_G_UNREACHED)
    {
        path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
        return -1;
    }

    int path_x = dest_x;
    int path_y = dest_y;
    uint16_t route_steps = 1;   /* the dest cell itself; +1 per hop back toward start */

    while(path_in_bounds(path_x, path_y) &&
          (nodes[path_x][path_y].parent_x != start_x ||
           nodes[path_x][path_y].parent_y != start_y))
    {
        int parent_x = nodes[path_x][path_y].parent_x;
        int parent_y = nodes[path_x][path_y].parent_y;

        if(parent_x == path_x && parent_y == path_y)
        {
            path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
            return -1;
        }

        if(!path_in_bounds(parent_x, parent_y))
        {
            path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
            return -1;
        }

        path_x = parent_x;
        path_y = parent_y;
        route_steps++;
    }

    if(!path_in_bounds(path_x, path_y))
    {
        path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
        return -1;
    }

    if(next_x != NULL)
        *next_x = path_x;

    if(next_y != NULL)
        *next_y = path_y;

    g_path_last_steps = route_steps;
    path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags,
                      (int32_t)nodes[dest_x][dest_y].g_cost, path_x, path_y);

    return (int32_t)nodes[dest_x][dest_y].g_cost;
}

/* Whole-unit wrapper. Keeps the old contract: 1 when start == dest, the
 * rounded route cost (always >= 1) otherwise, -1 for no route. */
int find_next_path_step_with_equipment(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    uint8_t equipment_flags,
    int *next_x,
    int *next_y) //path
{
    int32_t tenths = path_search(start_x, start_y, dest_x, dest_y,
                                 equipment_flags, next_x, next_y);

    if(tenths < 0)
        return -1;

    if(start_x == dest_x && start_y == dest_y)
        return 1;

    return path_cost_to_units(tenths);
}

int find_next_path_step_remaining(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    int *next_x,
    int *next_y,
    uint16_t *remaining_after_step)
{
    int step_x = start_x;
    int step_y = start_y;
    int32_t tenths = path_search(start_x, start_y, dest_x, dest_y, 0u,
                                 &step_x, &step_y);

    if(next_x != NULL)
        *next_x = step_x;
    if(next_y != NULL)
        *next_y = step_y;

    if(tenths < 0)
        return -1;

    if(start_x == dest_x && start_y == dest_y)
    {
        if(remaining_after_step != NULL)
            *remaining_after_step = 0u;
        return 1;
    }

    if(remaining_after_step != NULL)
    {
        /* The first step lies on the optimal route, so the route left
         * from it costs exactly total minus that step. */
        int32_t step = path_step_cost(start_x, start_y, step_x, step_y, 0u);
        int32_t rest = (step < 0 || step >= tenths) ? 0 : tenths - step;

        *remaining_after_step = (uint16_t)path_cost_to_units(rest);
    }

    return path_cost_to_units(tenths);
}

int find_next_path_step(int start_x, int start_y,int dest_x, int dest_y,int cost,int *next_x, int *next_y) //path
{
    (void)cost;
    return find_next_path_step_with_equipment(
        start_x,
        start_y,
        dest_x,
        dest_y,
        0u,
        next_x,
        next_y);
}

uint16_t find_path_steps(int start_x, int start_y, int dest_x, int dest_y)
{
    g_path_last_steps = UINT16_MAX;

    if(path_search(start_x, start_y, dest_x, dest_y, 0u, NULL, NULL) < 0)
        return UINT16_MAX;

    return g_path_last_steps;
}

uint16_t find_path_cost_with_equipment(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    uint8_t equipment_flags)
{
    int path_cost = find_next_path_step_with_equipment(
        start_x,
        start_y,
        dest_x,
        dest_y,
        equipment_flags,
        NULL,
        NULL);

    if(path_cost < 0)
        return UINT16_MAX;

    return (uint16_t)path_cost;
}

uint16_t find_path_cost(int start_x, int start_y,int dest_x, int dest_y)
{
    return find_path_cost_with_equipment(start_x, start_y, dest_x, dest_y, 0u);
}
