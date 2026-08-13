#ifndef RANDOM_H
#define RANDOM_H

#include<stdint.h>

typedef enum{
    STREAM_SAFE_ZONES=1,
    STREAM_TEAMS,
    STREAM_INCIDENTS,
    STREAM_GRID,
    STREAM_EVACUATION_GATEWAYS,
    STREAM_EVACUATION_TEAMS,
    STREAM_EQUIPMENT,
    STREAM_WORK_RATE,
    STREAM_STAGE_BRANCH,
    STREAM_INCIDENT_SCHEDULE,
    STREAM_ENVIRONMENT,
    STREAM_CBBA_CONSENSUS,
    STREAM_FAULTS
} RandomStream_t;
#define SEED_MIX_CONSTANT 2654435761u
void random_seed(uint32_t seed);
void random_seed_stream(RandomStream_t stream, uint32_t seed);
void random_seed_all(uint32_t root_seed);
uint32_t random_u32(void);
uint32_t random_u32_stream(RandomStream_t stream);
uint32_t random_range(uint32_t min,uint32_t max);
uint32_t random_range_stream(RandomStream_t stream, uint32_t min, uint32_t max);
uint32_t derive_seed(uint32_t root_seed,RandomStream_t stream);

#endif
