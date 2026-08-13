#include "path.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "environment.h"

static uint8_t path_in_bounds(int x, int y)
{
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

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
    int32_t cost;
    int16_t next_x, next_y;
} PathCacheEntry_t;

static PathCacheEntry_t g_path_cache[PATH_CACHE_SIZE];
static uint8_t g_path_cache_active = 0;

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
    if(!g_path_cache_active)
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
    return 1;
}

static void path_cache_store(
    int sx, int sy, int dx, int dy, uint8_t eq,
    int cost, int next_x, int next_y)
{
    if(!g_path_cache_active)
        return;

    uint32_t slot = path_cache_hash(sx, sy, dx, dy, eq);
    PathCacheEntry_t *e = &g_path_cache[slot];

    e->occupied = 1;
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
    uint16_t g_cost;
    uint8_t parent_x;
    uint8_t parent_y;
    uint8_t state;
} PathNode_t;


static int get_movement_cost(int x, int y, uint8_t equipment_flags)
{
    (void)equipment_flags;

    if(!path_in_bounds(x, y))
        return -1;

    if(!environment_cell_is_navigable(x, y))
        return -1;

    return environment_cell_travel_cost(x, y);
}

static uint8_t can_traverse_cell(int x, int y, uint8_t equipment_flags)
{
    (void)equipment_flags;

    if(!path_in_bounds(x, y))
        return 0;

    return environment_cell_is_navigable(x, y);
}

static uint16_t get_heuristic(int x,int y,int dest_x,int dest_y) //path
{
    return abs(x-dest_x)+abs(y-dest_y);
}

int find_next_path_step_with_equipment(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    uint8_t equipment_flags,
    int *next_x,
    int *next_y) //path
{
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
        if(next_x != NULL)
            *next_x = start_x;

        if(next_y != NULL)
            *next_y = start_y;

        return 1;
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

    PathNode_t nodes[GRID_WIDTH][GRID_HEIGHT];

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            nodes[x][y].g_cost = UINT16_MAX;
            nodes[x][y].parent_x = (uint8_t)x;
            nodes[x][y].parent_y = (uint8_t)y;
            nodes[x][y].state = PATH_UNVISITED;
        }
    }

    nodes[start_x][start_y].g_cost = 0;
    nodes[start_x][start_y].parent_x = (uint8_t)start_x;
    nodes[start_x][start_y].parent_y = (uint8_t)start_y;
    nodes[start_x][start_y].state = PATH_OPEN;

    int open_list_x[GRID_WIDTH * GRID_HEIGHT];
    int open_list_y[GRID_WIDTH * GRID_HEIGHT];
    int open_count = 0;

    open_list_x[open_count] = start_x;
    open_list_y[open_count] = start_y;
    open_count++;

    while(1)
    {
        int current_x = -1;
        int current_y = -1;
        int current_slot = -1;
        uint32_t lowest_f = UINT32_MAX;

        for(int i = 0; i < open_count; i++)
        {
            int x = open_list_x[i];
            int y = open_list_y[i];

            if(nodes[x][y].state != PATH_OPEN)
                continue;

            uint32_t f_cost =
                nodes[x][y].g_cost +
                get_heuristic(x, y, dest_x, dest_y);

            if(f_cost < lowest_f)
            {
                lowest_f = f_cost;
                current_x = x;
                current_y = y;
                current_slot = i;
            }
        }

        if(current_x == -1)
        {
            path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
            return -1;
        }

        open_list_x[current_slot] = open_list_x[open_count - 1];
        open_list_y[current_slot] = open_list_y[open_count - 1];
        open_count--;

        if(current_x == dest_x && current_y == dest_y)
        {
            break;
        }

        nodes[current_x][current_y].state = PATH_CLOSED;

        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};

        for(int i = 0; i < 4; i++)
        {
            int nx = current_x + dx[i];
            int ny = current_y + dy[i];

            if(!path_in_bounds(nx, ny))
                continue;

            int movement_cost = get_movement_cost(nx, ny, equipment_flags);

            if(movement_cost < 0)
                continue;

            if(nodes[nx][ny].state == PATH_CLOSED)
                continue;

            uint32_t new_g =
                nodes[current_x][current_y].g_cost +
                movement_cost;

            if(new_g < nodes[nx][ny].g_cost)
            {
                nodes[nx][ny].g_cost = new_g;
                nodes[nx][ny].parent_x = current_x;
                nodes[nx][ny].parent_y = current_y;

                if(nodes[nx][ny].state != PATH_OPEN)
                {
                    open_list_x[open_count] = nx;
                    open_list_y[open_count] = ny;
                    open_count++;
                }

                nodes[nx][ny].state = PATH_OPEN;
            }
        }
    }

    if(!path_in_bounds(dest_x, dest_y) ||
       nodes[dest_x][dest_y].g_cost == UINT16_MAX)
    {
        path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags, -1, 0, 0);
        return -1;
    }

    int path_x = dest_x;
    int path_y = dest_y;

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

    path_cache_store(start_x, start_y, dest_x, dest_y, equipment_flags,
                      (int)nodes[dest_x][dest_y].g_cost, path_x, path_y);

    return (int)nodes[dest_x][dest_y].g_cost;
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
