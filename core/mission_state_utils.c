#include <stddef.h>

#include "mission_state.h"
#include "snapshot.h"

const char *team_status_to_string(TeamStatus_t status)
{
    switch(status)
    {
        case STATUS_AVAILABLE: return "AVAILABLE";
        case STATUS_EN_ROUTE:  return "EN_ROUTE";
        case STATUS_ON_SCENE:  return "ON_SCENE";
        case STATUS_RESTING:   return "RESTING";
        default:               return "UNKNOWN";
    }
}

const char *incident_status_to_string(IncidentStatus_t status)
{
    switch(status)
    {
        case INC_OPEN:        return "OPEN";
        case INC_ASSIGNED:    return "ASSIGNED";
        case INC_IN_PROGRESS: return "IN_PROGRESS";
        case INC_RESOLVED:    return "RESOLVED";
        default:              return "UNKNOWN";
    }
}

const char *specialization_to_string(TeamSpecialization_t spec)
{
    switch(spec)
    {
        case SPEC_SEARCH:      return "SEARCH";
        case SPEC_MEDICAL:     return "MEDICAL";
        case SPEC_EXTRACTION:  return "EXTRACTION";
        case SPEC_PERIMETER:   return "PERIMETER";
        case SPEC_SUPPORT:     return "SUPPORT";
        default:               return "UNKNOWN";
    }
}

const char *priority_to_string(Priority_t priority)
{
    switch(priority)
    {
        case PRIORITY_CRITICAL: return "CRITICAL";
        case PRIORITY_HIGH:     return "HIGH";
        case PRIORITY_MEDIUM:   return "MEDIUM";
        case PRIORITY_LOW:      return "LOW";
        default:                return "UNKNOWN";
    }
}

float team_recovery_rate(const TeamState_t *team)
{
    switch(team->fitness_tier)
    {
        case ARDUOUS:
            return ARDUOUS_RECOVERY_RATE;

        case MODERATE:
            return MODERATE_RECOVERY_RATE;

        case LIGHT:
            return LIGHT_RECOVERY_RATE;

        default:
            return LIGHT_RECOVERY_RATE;
    }
}

static float equipment_speed_multiplier(uint8_t equipment_flags)
{
    float mult = EQUIP_SPEED_BASE_MULT;

    if(equipment_flags & EQ_ROPE)         mult += EQUIP_SPEED_PENALTY_ROPE;
    if(equipment_flags & EQ_SPREADER)     mult += EQUIP_SPEED_PENALTY_SPREADER;
    if(equipment_flags & EQ_MEDICAL_KIT)  mult += EQUIP_SPEED_PENALTY_MEDICAL_KIT;
    if(equipment_flags & EQ_K9)           mult += EQUIP_SPEED_PENALTY_K9;
    if(equipment_flags & EQ_HAZMAT_SUIT)  mult += EQUIP_SPEED_PENALTY_HAZMAT_SUIT;
    if(equipment_flags & EQ_STRUCTURAL)   mult += EQUIP_SPEED_PENALTY_STRUCTURAL;
    if(equipment_flags & EQ_GENERATOR)    mult += EQUIP_SPEED_PENALTY_GENERATOR;

    return mult;
}

static float fitness_speed_multiplier(uint8_t fitness_tier)
{
    switch(fitness_tier)
    {
        case ARDUOUS:  return FITNESS_SPEED_MULT_ARDUOUS;
        case MODERATE: return FITNESS_SPEED_MULT_MODERATE;
        case LIGHT:    return FITNESS_SPEED_MULT_LIGHT;
        default:       return FITNESS_SPEED_MULT_MODERATE;
    }
}

float team_travel_stamina_cost(const TeamState_t *team)
{
    return STAMINA_TRAVEL_COST *
           equipment_speed_multiplier(team->equipment_flags) *
           fitness_speed_multiplier(team->fitness_tier);
}

void team_set_available(TeamState_t *team)
{
    if(team == NULL)
        return;

    if(team->status == STATUS_AVAILABLE)
        return;

    team->status = STATUS_AVAILABLE;
    snapshot_record(g_current_tick, EVENT_TEAM_AVAILABLE, team->team_id);
}
