#include "random.h"
#include "safezone.h"
#include "environment.h"
#include <stdlib.h>

/* Diagnostic: how many safe zones were placed via the MAX_ATTEMPTS hard-exit
 * fallback (relaxation logic exhausted - degenerate map). Process-cumulative;
 * the P1 sweep captures before/after deltas. Expected to stay 0 on normal
 * seeds; a non-zero value flags maps where safe-zone placement is degrading
 * to the forced fallback rather than finding a real spot. */
unsigned long g_diag_safezone_maxattempts_fallback = 0;

/* Deterministic last resort for the MAX_ATTEMPTS hard-exit fallback below:
 * scans the whole grid for the first navigable cell not already claimed
 * by an earlier safe zone (index < zone_index), same shape as core/
 * incident.c's find_free_perimeter_cell() added for the analogous
 * evacuation-gateway fallback. Guaranteed to succeed as long as any
 * unclaimed navigable cell exists at all - with MAX_SAFE_ZONES (4)
 * claiming at most 4 cells out of a 36x36 grid, that's always true in
 * practice. */
static uint8_t find_free_navigable_cell(int zone_index, int16_t *out_x, int16_t *out_y)
{
    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            if(!environment_cell_is_navigable(x, y))
                continue;

            uint8_t claimed = 0;

            for(int j = 0; j < zone_index; j++)
            {
                if(g_safezones[j].loc_x == x && g_safezones[j].loc_y == y)
                {
                    claimed = 1;
                    break;
                }
            }

            if(!claimed)
            {
                *out_x = (int16_t)x;
                *out_y = (int16_t)y;
                return 1;
            }
        }
    }

    return 0;
}

void generate_safe_zones(uint32_t seed)
{
    random_seed_stream(STREAM_SAFE_ZONES, derive_seed(seed, STREAM_SAFE_ZONES));
    g_num_safe_zones = random_range_stream(STREAM_SAFE_ZONES, MIN_SAFE_ZONES, MAX_SAFE_ZONES);

    for(int i = 0; i < g_num_safe_zones; i++)
    {
        int flag = 1;
        int x, y;
        uint16_t attempts = 0;           /* was uint8_t — wrapped at 255 */
        const uint16_t MAX_ATTEMPTS = 300;

        while(flag)
        {
            int flag_distance = 1, flag_pos = 1;

            if(attempts == 0 &&
               environment_find_safe_zone_site((uint8_t)i, g_num_safe_zones,
                                               &g_safezones[i].loc_x,
                                               &g_safezones[i].loc_y))
            {
                x = g_safezones[i].loc_x;
                y = g_safezones[i].loc_y;
            }
            else
            {
                x = random_range_stream(STREAM_SAFE_ZONES, 0, GRID_WIDTH - 1);
                y = random_range_stream(STREAM_SAFE_ZONES, 0, GRID_HEIGHT - 1);
            }

            /* Never accept a non-navigable candidate: keep drawing until
               MAX_ATTEMPTS, where the hard-exit path below scans for a
               free navigable cell. (This used to swap in (0,0) after 40
               tries without checking (0,0) was navigable either.) */
            if(!environment_cell_is_navigable(x, y) && attempts < MAX_ATTEMPTS)
            {
                attempts++;
                continue;
            }

            /* Hard exit — never loop forever on a degenerate map */
            if(attempts >= MAX_ATTEMPTS)
            {
                int16_t fallback_x = (int16_t)x;
                int16_t fallback_y = (int16_t)y;

                g_diag_safezone_maxattempts_fallback++;

                /* Bug fix: this used to commit (x,y) directly - which
                   could be a non-navigable last candidate, or any
                   other candidate - without re-running the
                   duplicate-location check the normal path below
                   performs, so more than one zone hitting this branch
                   on the same degenerate map could register at the
                   identical location with no de-duplication at all.
                   Scan for a genuinely free cell instead; only fall
                   back to the raw (possibly-duplicate) candidate if
                   even that scan is somehow exhausted. */
                if(!find_free_navigable_cell(i, &fallback_x, &fallback_y))
                {
                    fallback_x = (int16_t)x;
                    fallback_y = (int16_t)y;
                }

                g_safezones[i].loc_x = fallback_x;
                g_safezones[i].loc_y = fallback_y;
                g_safezones[i].safezone_id = i;
                g_safezones[i].capacity = random_range_stream(
                    STREAM_SAFE_ZONES, MIN_SAFE_ZONE_CAPACITY, MAX_SAFE_ZONE_CAPACITY);
                g_safezones[i].evacuee_count = 0;
                environment_register_safe_zone((uint8_t)i,
                                               g_safezones[i].loc_x,
                                               g_safezones[i].loc_y);
                break;
            }

            attempts++;

            for(int j = 0; j < i; j++)
            {
                if(g_safezones[j].loc_x == x && g_safezones[j].loc_y == y)
                {
                    flag_pos = 0;
                    break;
                }
                int dx = abs((int)g_safezones[j].loc_x - (int)x);
                int dy = abs((int)g_safezones[j].loc_y - (int)y);
                if(dx + dy < 4 && attempts <= 80)
                {
                    flag_distance = 0;
                    break;
                }
            }

            flag = !(flag_distance && flag_pos);
            if(!flag)
            {
                g_safezones[i].loc_x = x;
                g_safezones[i].loc_y = y;
                g_safezones[i].safezone_id = i;
                g_safezones[i].capacity = random_range_stream(
                    STREAM_SAFE_ZONES, MIN_SAFE_ZONE_CAPACITY, MAX_SAFE_ZONE_CAPACITY);
                g_safezones[i].evacuee_count = 0;
                environment_register_safe_zone((uint8_t)i,
                                               g_safezones[i].loc_x,
                                               g_safezones[i].loc_y);
            }
        }
    }
}