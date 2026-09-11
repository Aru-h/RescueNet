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

            if(!environment_cell_is_navigable(x, y) && attempts < 40)
            {
                attempts++;
                continue;
            }

            if(!environment_cell_is_navigable(x, y))
            {
                x = 0;
                y = 0;
            }

            /* Hard exit — never loop forever on a degenerate map */
            if(attempts >= MAX_ATTEMPTS)
            {
                g_diag_safezone_maxattempts_fallback++;
                g_safezones[i].loc_x = (int16_t)x;
                g_safezones[i].loc_y = (int16_t)y;
                g_safezones[i].safezone_id = i;
                g_safezones[i].capacity = random_range_stream(
                    STREAM_SAFE_ZONES, MIN_SAFE_ZONE_CAPACITY, MAX_SAFE_ZONE_CAPACITY);
                g_safezones[i].evacuee_count = 0;
                environment_register_safe_zone((uint8_t)i,
                                               g_safezones[i].loc_x,
                                               g_safezones[i].loc_y);
                break;
            }

            if(attempts > 80)
                flag_distance = 1;

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