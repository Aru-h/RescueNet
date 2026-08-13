#include "random.h"

#define NUM_RANDOM_STREAM_STATES (STREAM_FAULTS + 1)

static uint32_t g_random_state = 1;
static uint32_t g_stream_states[NUM_RANDOM_STREAM_STATES];

static uint32_t normalize_seed(uint32_t seed)
{
    return seed == 0 ? 1u : seed;
}

static uint32_t xorshift32(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;

    return *state;
}

void random_seed(uint32_t seed)
{
    g_random_state = normalize_seed(seed);

}

void random_seed_stream(RandomStream_t stream, uint32_t seed)
{
    if(stream <= 0 || stream >= NUM_RANDOM_STREAM_STATES)
    {
        return;
    }

    g_stream_states[stream] = normalize_seed(seed);
}

void random_seed_all(uint32_t root_seed)
{
    random_seed(root_seed);

    for(int stream = STREAM_SAFE_ZONES; stream < NUM_RANDOM_STREAM_STATES; stream++)
    {
        random_seed_stream((RandomStream_t)stream, derive_seed(root_seed, (RandomStream_t)stream));
    }
}

uint32_t random_u32(void)
{
    return xorshift32(&g_random_state);
}

uint32_t random_u32_stream(RandomStream_t stream)
{
    if(stream <= 0 || stream >= NUM_RANDOM_STREAM_STATES)
    {
        return random_u32();
    }

    if(g_stream_states[stream] == 0)
    {
        random_seed_stream(stream, derive_seed(1u, stream));
    }

    return xorshift32(&g_stream_states[stream]);
}

uint32_t random_range(uint32_t min,uint32_t max)
{
    if(min>max)
    {
        uint32_t temp=max;
        max=min;
        min=temp;
    }
    return min + (random_u32()%(max-min+1));
}

uint32_t random_range_stream(RandomStream_t stream, uint32_t min, uint32_t max)
{
    if(min>max)
    {
        uint32_t temp=max;
        max=min;
        min=temp;
    }
    return min + random_u32_stream(stream)%(max-min+1);
}

uint32_t derive_seed(uint32_t root_seed,RandomStream_t stream)
{
    return root_seed^(stream*SEED_MIX_CONSTANT);
}
