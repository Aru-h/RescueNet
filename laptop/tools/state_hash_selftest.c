/* state_hash_selftest: the incident hash depends on WHICH incidents exist,
 * not on which slot each one sits in (a board fills slots in link-arrival
 * order). Also checks the hash still sees real changes. */
#include <stdio.h>
#include <string.h>

#include "mission_state.h"
#include "state_hash.h"

static int g_fail;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while(0)

static void put(uint32_t slot, uint32_t id, int16_t x)
{
    memset(&g_incidents[slot], 0, sizeof(g_incidents[slot]));
    g_incidents[slot].slot_occupied = 1u;
    g_incidents[slot].incident_id = id;
    g_incidents[slot].loc_x = x;
    g_incidents[slot].victim_count = (uint8_t)(id & 0x7u);
}

int main(void)
{
    StateHash_t a, b, c, d;

    memset(g_incidents, 0, sizeof(g_incidents));
    put(0, 0x00010001u, 3); put(1, 0x00010002u, 9); put(2, 0x00020001u, 5); put(3, 0x00030001u, 7);
    state_hash_compute(&a);

    /* Same four incidents, different slots and gaps. */
    memset(g_incidents, 0, sizeof(g_incidents));
    put(17, 0x00030001u, 7); put(4, 0x00010002u, 9); put(9, 0x00020001u, 5); put(0, 0x00010001u, 3);
    state_hash_compute(&b);
    CHECK(a.incidents == b.incidents);
    CHECK(a.all == b.all);

    /* A changed field changes it. */
    put(9, 0x00020001u, 6);
    state_hash_compute(&c);
    CHECK(c.incidents != a.incidents);

    /* A missing incident changes it. */
    memset(&g_incidents[9], 0, sizeof(g_incidents[9]));
    state_hash_compute(&d);
    CHECK(d.incidents != a.incidents && d.incidents != c.incidents);

    if(g_fail)
        return 1;
    printf("PASS: incident hash is slot-order independent and still sensitive to content\n");
    return 0;
}
