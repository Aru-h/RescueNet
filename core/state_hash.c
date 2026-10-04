#include "state_hash.h"

#include <string.h>

#include "mission_state.h"
#include "recovery_manager.h"

#define FNV64_OFFSET 0xcbf29ce484222325ull
#define FNV64_PRIME  0x00000100000001b3ull

static void fnv_bytes(uint64_t *h, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    for(uint32_t i = 0; i < len; i++)
    {
        *h ^= p[i];
        *h *= FNV64_PRIME;
    }
}

/* Fixed-width little-endian feed, independent of the host's endianness or
   the declared width of the source field. */
static void fnv_u32(uint64_t *h, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

    fnv_bytes(h, b, 4u);
}

static void fnv_f32(uint64_t *h, float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    fnv_u32(h, bits);
}

static void hash_team_faults(uint64_t *h, const TeamFaultState_t *f)
{
    fnv_u32(h, f->active_mask);
    for(uint32_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
    {
        fnv_u32(h, f->active[c]);
        fnv_u32(h, f->start_tick[c]);
        fnv_u32(h, f->duration_ticks[c]);
        fnv_u32(h, f->recovery_tick[c]);
        fnv_u32(h, f->severity[c]);
        fnv_u32(h, f->target_team[c]);
        fnv_u32(h, (uint32_t)(uint16_t)f->target_x[c]);
        fnv_u32(h, (uint32_t)(uint16_t)f->target_y[c]);
        fnv_u32(h, f->target_incident_id[c]);
        fnv_u32(h, f->original_travel_cost[c]);
        fnv_u32(h, f->original_road_blocked[c]);
        fnv_u32(h, f->original_navigable[c]);
        fnv_u32(h, f->original_priority[c]);
        fnv_u32(h, f->original_victim_count[c]);
        fnv_u32(h, f->pending_repair[c]);
        fnv_u32(h, f->repair_confirm_tick[c]);
        fnv_u32(h, f->immune_until_tick[c]);
    }
    fnv_u32(h, f->last_fault_class);
    fnv_u32(h, f->last_fault_start_tick);
    fnv_u32(h, f->last_fault_duration_ticks);
    fnv_u32(h, f->last_fault_recovery_tick);
    fnv_u32(h, f->last_fault_mask);
}

static void hash_team(uint64_t *h, const TeamState_t *t)
{
    fnv_u32(h, t->team_id);
    fnv_u32(h, (uint32_t)(uint16_t)t->loc_x);
    fnv_u32(h, (uint32_t)(uint16_t)t->loc_y);
    fnv_f32(h, t->stamina);
    fnv_u32(h, t->specialization);
    fnv_u32(h, t->status);
    fnv_u32(h, t->assigned_task_pool);
    fnv_u32(h, t->assigned_task_slot);
    for(uint32_t i = 0; i < CBBA_MAX_BUNDLE_SIZE; i++)
    {
        fnv_u32(h, t->incident_queue_slot[i]);
        fnv_u32(h, t->incident_queue_id[i]);
    }
    fnv_u32(h, t->incident_queue_size);
    fnv_u32(h, t->incident_queue_index);
    fnv_u32(h, t->hours_worked);
    fnv_u32(h, t->ticks_worked_this_period);
    fnv_u32(h, t->equipment_flags);
    fnv_u32(h, t->last_heartbeat_tick);
    fnv_u32(h, t->fitness_tier);
    fnv_u32(h, t->resting_zone_id);
    fnv_u32(h, t->heading_to_rest);
    fnv_u32(h, t->equipment_level);
    fnv_u32(h, t->cached_remaining_path_cost);
    fnv_u32(h, t->cached_remaining_path_cost_tick);
    fnv_u32(h, t->partial_rest_cycles);
    fnv_u32(h, t->degraded);
}

static void hash_incident(uint64_t *h, const IncidentState_t *n)
{
    fnv_u32(h, n->incident_id);
    fnv_u32(h, n->slot_occupied);
    fnv_u32(h, (uint32_t)(uint16_t)n->loc_x);
    fnv_u32(h, (uint32_t)(uint16_t)n->loc_y);
    fnv_u32(h, (uint32_t)n->priority);
    fnv_u32(h, (uint32_t)n->incident_type);
    fnv_u32(h, (uint32_t)n->req_capability);
    fnv_u32(h, n->status);
    fnv_u32(h, n->victim_count);
    fnv_u32(h, n->hazard_flags);
    fnv_u32(h, n->reported_at_tick);
    fnv_u32(h, n->structure_type);
    fnv_u32(h, n->assigned_team);
    fnv_u32(h, n->passive_effect);
    fnv_u32(h, n->passive_timer);
    fnv_u32(h, n->spawned_by);
    fnv_u32(h, n->support_team);
    fnv_u32(h, n->work_remaining);
    fnv_u32(h, n->building_id);
    fnv_u32(h, n->floor);
    fnv_u32(h, n->stage_work);
    fnv_u32(h, n->discovered);
    fnv_u32(h, n->mission_fault_active_count);
    fnv_u32(h, n->unreachable_streak);
    fnv_u32(h, n->mission_fault_original_priority);
    fnv_u32(h, n->mission_fault_original_victim_count);
    fnv_u32(h, n->evac_safezone_id);
    fnv_u32(h, n->evac_gateway_id);
    fnv_u32(h, n->evac_capacity);
}

void state_hash_compute(StateHash_t *out)
{
    uint64_t ht = FNV64_OFFSET;
    uint64_t hi = FNV64_OFFSET;
    uint64_t hf = FNV64_OFFSET;
    uint64_t hr = FNV64_OFFSET;
    uint64_t ha = FNV64_OFFSET;

    for(uint32_t t = 0; t < NUM_TEAMS; t++)
    {
        hash_team(&ht, &g_teams[t]);
        hash_team_faults(&hf, &g_teams[t].fault_state);
    }

    for(uint32_t i = 0; i < NUM_INCIDENTS; i++)
        hash_incident(&hi, &g_incidents[i]);
    for(uint32_t i = 0; i < NUM_SYSTEM_TASKS; i++)
        hash_incident(&hi, &g_system_tasks[i]);

    /* Recovery: the fused verdict, every evaluator's own verdict (this
       includes the AI predictor's confidence, so it is what makes an
       AI-path change visible), and the action/timing counters. */
    for(uint32_t t = 0; t < NUM_TEAMS; t++)
    {
        RecoveryStatus_t s = recovery_manager_get_status((uint8_t)t);

        fnv_u32(&hr, (uint32_t)s.recommendation);
        fnv_u32(&hr, (uint32_t)s.reason);
        fnv_f32(&hr, s.confidence);
        fnv_u32(&hr, s.escalation_level);
        for(uint32_t e = 0; e < RECOVERY_MANAGER_EVAL_SLOT_COUNT; e++)
        {
            RecoveryEvaluatorVerdict_t v =
                recovery_manager_get_evaluator_verdict((uint8_t)t, (uint8_t)e);

            fnv_u32(&hr, (uint32_t)v.reason);
            fnv_f32(&hr, v.confidence);
        }
    }
    {
        RecoveryMetrics_t m = recovery_manager_get_metrics();

        for(uint32_t a = 0; a < RECOVERY_ACTION_COUNT; a++)
        {
            fnv_u32(&hr, m.dispatched[a]);
            fnv_u32(&hr, m.resolved[a]);
            fnv_u32(&hr, m.resolution_ticks_sum[a]);
            fnv_u32(&hr, m.timed_out[a]);
        }
        fnv_u32(&hr, m.recommendation_reversals);
        fnv_u32(&hr, m.evaluation_ticks);
    }

    fnv_u32(&ha, g_current_tick);
    fnv_bytes(&ha, &ht, sizeof(ht));
    fnv_bytes(&ha, &hi, sizeof(hi));
    fnv_bytes(&ha, &hf, sizeof(hf));
    fnv_bytes(&ha, &hr, sizeof(hr));

    out->teams = ht;
    out->incidents = hi;
    out->faults = hf;
    out->recovery = hr;
    out->all = ha;
}
