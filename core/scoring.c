#include <math.h>
#include "scoring.h"
#include "path.h"
#include <stdint.h>
#include <stdlib.h>
static const float MAX_ROUTE_DISTANCE = ((GRID_WIDTH - 1) + (GRID_HEIGHT - 1)) * 4.0f;

#define MIN_EQUIPMENT_FACTOR 0.25f

static const ScoringWeights_t g_default_scoring_weights = {
    0.30f,
    0.15f,
    0.20f,
    0.25f,
    0.10f
};

static ScoringWeights_t g_scoring_weights = {
    0.30f,
    0.15f,
    0.20f,
    0.25f,
    0.10f
};

typedef struct
{
    uint8_t minimum;
    uint8_t optimal;
} EquipmentRequirement_t;

static EquipmentRequirement_t equipment_requirements(
    const IncidentState_t *incident)
{
    EquipmentRequirement_t req = {0, 0};

    switch(incident->incident_type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
            if(incident->req_capability == SPEC_PERIMETER)
            {
                req.minimum = EQ_STRUCTURAL;
                req.optimal = EQ_ROPE;
            }
            else if(incident->req_capability == SPEC_MEDICAL)
            {
                req.minimum = EQ_MEDICAL_KIT;
            }
            else if(incident->req_capability == SPEC_EXTRACTION)
            {
                req.minimum = EQ_SPREADER;
                req.optimal = EQ_ROPE | EQ_STRUCTURAL;
            }
            break;

        case INCIDENT_CONFINED_SPACE:
            if(incident->req_capability == SPEC_PERIMETER)
            {
                req.minimum = EQ_ROPE;
                req.optimal = EQ_STRUCTURAL;
            }
            else if(incident->req_capability == SPEC_MEDICAL)
            {
                req.minimum = EQ_MEDICAL_KIT;
            }
            else if(incident->req_capability == SPEC_EXTRACTION)
            {
                req.minimum = EQ_ROPE;
                req.optimal = EQ_SPREADER;
            }
            break;

        case INCIDENT_MASS_CASUALTY:
            if(incident->req_capability == SPEC_MEDICAL)
            {
                req.minimum = EQ_MEDICAL_KIT;
            }
            break;

        case INCIDENT_CHEMICAL_EXPOSURE:
            if(incident->req_capability == SPEC_PERIMETER)
            {
                req.minimum = EQ_HAZMAT_SUIT;
                req.optimal = EQ_STRUCTURAL;
            }
            else if(incident->req_capability == SPEC_MEDICAL)
            {
                req.minimum = EQ_MEDICAL_KIT;
            }
            break;

        case INCIDENT_UNSEARCHED_SECTOR:
            if(incident->req_capability == SPEC_SEARCH)
            {
                req.optimal = EQ_K9 | EQ_ROPE;
            }
            else if(incident->req_capability == SPEC_MEDICAL)
            {
                req.minimum = EQ_MEDICAL_KIT;
            }
            else if(incident->req_capability == SPEC_EXTRACTION)
            {
                req.minimum = EQ_ROPE;
                req.optimal = EQ_SPREADER;
            }
            break;

        case INCIDENT_SURVIVOR_SIGNAL:
            if(incident->req_capability == SPEC_SEARCH)
            {
                req.optimal = EQ_K9;
            }
            else if(incident->req_capability == SPEC_EXTRACTION)
            {
                req.minimum = EQ_ROPE;
                req.optimal = EQ_SPREADER;
            }
            break;

        case INCIDENT_GAS_LEAK:
            if(incident->req_capability == SPEC_PERIMETER)
            {
                req.minimum = EQ_HAZMAT_SUIT;
                req.optimal = EQ_STRUCTURAL;
            }
            break;

        case INCIDENT_ACTIVE_FIRE:
            if(incident->req_capability == SPEC_PERIMETER)
            {
                req.minimum = EQ_STRUCTURAL;
                req.optimal = EQ_ROPE | EQ_MEDICAL_KIT;
            }
            else if(incident->req_capability == SPEC_EXTRACTION)
            {
                req.minimum = EQ_ROPE;
                req.optimal = EQ_SPREADER;
            }
            break;

        default:
            break;
    }

    return req;
}

static int equipment_requirement_pair_is_defined(
    uint8_t incident_type,
    uint8_t req_capability)
{
    switch(incident_type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
            return req_capability == SPEC_PERIMETER ||
                   req_capability == SPEC_MEDICAL ||
                   req_capability == SPEC_EXTRACTION;

        case INCIDENT_CONFINED_SPACE:
            return req_capability == SPEC_PERIMETER ||
                   req_capability == SPEC_MEDICAL ||
                   req_capability == SPEC_EXTRACTION;

        case INCIDENT_MASS_CASUALTY:
            return req_capability == SPEC_MEDICAL;

        case INCIDENT_CHEMICAL_EXPOSURE:
            return req_capability == SPEC_PERIMETER ||
                   req_capability == SPEC_MEDICAL;

        case INCIDENT_UNSEARCHED_SECTOR:
            return req_capability == SPEC_SEARCH ||
                   req_capability == SPEC_MEDICAL ||
                   req_capability == SPEC_EXTRACTION;

        case INCIDENT_SURVIVOR_SIGNAL:
            return req_capability == SPEC_SEARCH ||
                   req_capability == SPEC_EXTRACTION;

        case INCIDENT_GAS_LEAK:
            return req_capability == SPEC_PERIMETER;

        case INCIDENT_ACTIVE_FIRE:
            return req_capability == SPEC_PERIMETER ||
                   req_capability == SPEC_EXTRACTION;

        default:
            return 0;
    }
}

int validate_equipment_requirement_matrix(void)
{
    static const struct
    {
        uint8_t incident_type;
        uint8_t req_capability;
    } reachable_pairs[] =
    {
        {INCIDENT_STRUCTURAL_COLLAPSE, SPEC_PERIMETER},
        {INCIDENT_STRUCTURAL_COLLAPSE, SPEC_MEDICAL},
        {INCIDENT_STRUCTURAL_COLLAPSE, SPEC_EXTRACTION},

        {INCIDENT_CONFINED_SPACE, SPEC_PERIMETER},
        {INCIDENT_CONFINED_SPACE, SPEC_MEDICAL},
        {INCIDENT_CONFINED_SPACE, SPEC_EXTRACTION},

        {INCIDENT_MASS_CASUALTY, SPEC_MEDICAL},

        {INCIDENT_CHEMICAL_EXPOSURE, SPEC_PERIMETER},
        {INCIDENT_CHEMICAL_EXPOSURE, SPEC_MEDICAL},

        {INCIDENT_UNSEARCHED_SECTOR, SPEC_SEARCH},
        {INCIDENT_UNSEARCHED_SECTOR, SPEC_MEDICAL},
        {INCIDENT_UNSEARCHED_SECTOR, SPEC_EXTRACTION},

        {INCIDENT_SURVIVOR_SIGNAL, SPEC_SEARCH},
        {INCIDENT_SURVIVOR_SIGNAL, SPEC_EXTRACTION},

        {INCIDENT_GAS_LEAK, SPEC_PERIMETER},

        {INCIDENT_ACTIVE_FIRE, SPEC_PERIMETER},
        {INCIDENT_ACTIVE_FIRE, SPEC_EXTRACTION}
    };

    size_t pair_count =
        sizeof(reachable_pairs) /
        sizeof(reachable_pairs[0]);

    for(size_t i = 0; i < pair_count; i++)
    {
        if(!equipment_requirement_pair_is_defined(
            reachable_pairs[i].incident_type,
            reachable_pairs[i].req_capability))
        {
            return 0;
        }
    }

    return 1;
}

int team_has_minimum_equipment(
    const TeamState_t *team,
    const IncidentState_t *incident)
{
    EquipmentRequirement_t req =
        equipment_requirements(incident);

    return
        (team->equipment_flags & req.minimum)
        == req.minimum;
}

static float equipment_score(
    const TeamState_t *team,
    const IncidentState_t *incident)
{
    EquipmentRequirement_t req =
        equipment_requirements(incident);

    if((team->equipment_flags & req.minimum)
        != req.minimum)
    {
        return 0.0f;
    }

    if(req.optimal == 0)
        return 1.0f;

    uint8_t owned =
        team->equipment_flags & req.optimal;

    uint8_t optimal_count = 0;
    uint8_t owned_count = 0;

    for(uint8_t bit = 0; bit < 8; bit++)
    {
        uint8_t mask = (uint8_t)(1u << bit);

        if(req.optimal & mask)
            optimal_count++;

        if(owned & mask)
            owned_count++;
    }

    return
        (float)owned_count /
        (float)optimal_count;
}

static float equipment_condition_factor(
    const TeamState_t *team)
{
    float level =
        (float)team->equipment_level /
        (float)EQ_MAX_LEVEL;

    if(level < 0.0f)
        level = 0.0f;

    if(level > 1.0f)
        level = 1.0f;

    return
        MIN_EQUIPMENT_FACTOR +
        (1.0f - MIN_EQUIPMENT_FACTOR) * level;
}

static float team_stamina_cost(const TeamState_t *team)
{
    return 1.0f - stamina_score(team);
}

static float team_equipment_cost(const TeamState_t *team)
{
    return 1.0f - equipment_condition_factor(team);
}

static float logarithmic_waiting_time_bonus(uint32_t age_ticks)
{
    const float normalized_age_limit = log1pf(2000.0f);
    float normalized_age = log1pf((float)age_ticks);

    if(normalized_age_limit <= 0.0f)
        return 0.0f;

    normalized_age /= normalized_age_limit;

    if(normalized_age < 0.0f)
        normalized_age = 0.0f;

    if(normalized_age > 1.0f)
        normalized_age = 1.0f;

    return 0.50f * normalized_age;
}

static void normalize_scoring_weights(ScoringWeights_t *weights)
{
    float total =
        weights->capability +
        weights->travel +
        weights->priority +
        weights->stamina +
        weights->equipment;

    if(total <= 0.0f)
    {
        *weights = g_default_scoring_weights;
        return;
    }

    weights->capability /= total;
    weights->travel /= total;
    weights->priority /= total;
    weights->stamina /= total;
    weights->equipment /= total;
}

void scoring_set_weights(ScoringWeights_t weights)
{
    normalize_scoring_weights(&weights);
    g_scoring_weights = weights;
}

ScoringWeights_t scoring_get_weights(void)
{
    return g_scoring_weights;
}

void scoring_reset_weights(void)
{
    g_scoring_weights = g_default_scoring_weights;
}

static const float CAPABILITY_MATRIX[5][5] = {
/*              MED    SRCH   EXTR   PERIM  SUPP  */
/* Medical   */ {1.00, 0.45,  0.15,  0.30,  0.60},
/* Search    */ {0.35, 1.00,  0.70,  0.60,  0.50},
/* Extraction*/ {0.25, 0.55,  1.00,  0.45,  0.50},
/* Perimeter */ {0.30, 0.40,  0.45,  1.00,  0.75},
/* Support   */ {0.60, 0.50,  0.55,  0.70,  1.00},
};

static float path_distance_score(
    uint8_t start_x,
    uint8_t start_y,
    const IncidentState_t *incident)
{
    int path_cost = find_next_path_step(start_x,start_y,incident->loc_x,incident->loc_y,1,NULL,NULL);

    if(path_cost < 0)
        return 0.0f;
    float score =
        (MAX_ROUTE_DISTANCE - (float)path_cost) /
        MAX_ROUTE_DISTANCE;

    if(score < 0.0f)
        score = 0.0f;

    if(score > 1.0f)
        score = 1.0f;

    return score;
}

float distance_score(
    const TeamState_t *team,
    const IncidentState_t *incident)
{
    return path_distance_score(
        team->loc_x,
        team->loc_y,
        incident);
}

float capability_score(const TeamState_t *team, const IncidentState_t *incident)
{
    return CAPABILITY_MATRIX[team->specialization][incident->req_capability];
}

float priority_score(const IncidentState_t *incident)
{
    float base_priority;

    switch(incident->priority)
    {
        case PRIORITY_CRITICAL:
            base_priority = 1.00f;
            break;

        case PRIORITY_HIGH:
            base_priority = 0.75f;
            break;

        case PRIORITY_MEDIUM:
            base_priority = 0.50f;
            break;

        case PRIORITY_LOW:
            base_priority = 0.25f;
            break;

        default:
            return 0.0f;
    }

    uint32_t age_ticks = 0;

    if(g_current_tick >= incident->reported_at_tick)
    {
        age_ticks =
            g_current_tick - incident->reported_at_tick;
    }

    float score = base_priority + logarithmic_waiting_time_bonus(age_ticks);

    if(score > 1.0f)
        score = 1.0f;

    return score;
}

float calculate_score(
    const TeamState_t *team,
    const IncidentState_t *incident)
{
    float equipment_quality =
        equipment_score(team, incident) *
        equipment_condition_factor(team);

    float final_score =
        g_scoring_weights.capability *
            capability_score(team, incident)
        +
        g_scoring_weights.travel *
            distance_score(team, incident)
        +
        g_scoring_weights.priority *
            priority_score(incident)
        +
        g_scoring_weights.stamina *
            stamina_score(team)
        +
        g_scoring_weights.equipment *
            equipment_quality;

    return final_score;
}


static float incident_utility_score(
    const TeamState_t *team,
    const IncidentState_t *incident)
{
    float equipment_quality =
        equipment_score(team, incident) *
        equipment_condition_factor(team);

    return
        g_scoring_weights.capability * capability_score(team, incident) +
        g_scoring_weights.priority * priority_score(incident) +
        g_scoring_weights.stamina * stamina_score(team) +
        g_scoring_weights.equipment * equipment_quality;
}


static int route_path_cost(const TeamState_t *team,const TaskRef_t *bundle,uint8_t bundle_size)
{
    uint8_t start_x = team->loc_x;
    uint8_t start_y = team->loc_y;

    int total_cost = 0;

    for(uint8_t i = 0; i < bundle_size; i++)
    {
        TaskRef_t task = bundle[i];

        if(task.pool != TASK_POOL_INCIDENT ||
           task.slot >= NUM_INCIDENTS)
        {
            continue;
        }

        const IncidentState_t *incident =
            &g_incidents[task.slot];

        if(!incident->slot_occupied)
            continue;

        int path_cost = find_next_path_step(
            start_x,
            start_y,
            incident->loc_x,
            incident->loc_y,
            1,
            NULL,
            NULL);

        if(path_cost < 0)
            return -1;

        total_cost += path_cost;

        start_x = incident->loc_x;
        start_y = incident->loc_y;
    }

    return total_cost;
}

float calculate_route_utility(
    const TeamState_t *team,
    const TaskRef_t *path,
    uint8_t path_size)
{
    float task_utility = 0.0f;

    for(uint8_t i = 0; i < path_size; i++)
    {
        TaskRef_t task = path[i];

        if(task.pool != TASK_POOL_INCIDENT ||
           task.slot >= NUM_INCIDENTS)
        {
            continue;
        }

        const IncidentState_t *incident =
            &g_incidents[task.slot];

        if(!incident->slot_occupied)
            continue;

        task_utility +=
            incident_utility_score(
                team,
                incident);
    }

    int total_route_cost =
        route_path_cost(
            team,
            path,
            path_size);

    if(total_route_cost < 0)
        return -1.0f;

    float normalized_route_cost =
        (float)total_route_cost /
        (MAX_ROUTE_DISTANCE *
         (float)CBBA_MAX_BUNDLE_SIZE);

    float distance_penalty =
        g_scoring_weights.travel *
        normalized_route_cost;

    return
        task_utility -
        distance_penalty;
}


float calculate_marginal_bundle_bid(
    uint8_t team_id,
    const CBBAAgentState_t *agent,
    const IncidentState_t *incident,
    uint8_t *best_insert_position)
{
    const TeamState_t *team =
        &g_teams[team_id];

    TaskRef_t candidate;

    candidate.pool =
        TASK_POOL_INCIDENT;

    candidate.slot =
        (uint8_t)(incident - g_incidents);

    float current_utility =
        calculate_route_utility(
            team,
            agent->path,
            agent->path_size);

    if(current_utility < 0.0f)
        return -1.0f;

    float best_marginal_bid = -1.0f;
    uint8_t best_position = 0;
    float stamina_penalty =
        g_scoring_weights.stamina * team_stamina_cost(team);
    float equipment_penalty =
        g_scoring_weights.equipment * team_equipment_cost(team);

    for(uint8_t position = 0;
        position <= agent->path_size;
        position++)
    {
        TaskRef_t trial_path[
            CBBA_MAX_BUNDLE_SIZE];

        uint8_t trial_index = 0;

        for(uint8_t i = 0;
            i < agent->path_size;
            i++)
        {
            if(trial_index == position)
            {
                trial_path[trial_index] =
                    candidate;

                trial_index++;
            }

            trial_path[trial_index] =
                agent->path[i];

            trial_index++;
        }

        if(trial_index == position)
        {
            trial_path[trial_index] =
                candidate;

            trial_index++;
        }

        float trial_utility =
            calculate_route_utility(
                team,
                trial_path,
                trial_index);

        if(trial_utility < 0.0f)
            continue;

        float marginal_bid =
            trial_utility -
            current_utility -
            stamina_penalty -
            equipment_penalty;

        if(marginal_bid >
           best_marginal_bid)
        {
            best_marginal_bid =
                marginal_bid;

            best_position =
                position;
        }
    }

    if(best_insert_position != NULL)
    {
        *best_insert_position =
            best_position;
    }

    return best_marginal_bid;
}

float stamina_score(const TeamState_t *team)
{
    float normalized = (float)team->stamina /(float)MAX_STAMINA;
    return normalized * normalized;
}

int candidate_claim_is_better(
    uint8_t candidate_team,
    float candidate_bid,
    uint8_t current_team,
    float current_bid,
    const IncidentState_t *incident)
{
    const float BID_EPSILON = 0.0001f;

    const TeamState_t *candidate =
        &g_teams[candidate_team];

    if(current_team == NO_ASSIGNMENT)
        return 1;

    const TeamState_t *current =
        &g_teams[current_team];

    if(candidate_bid > current_bid + BID_EPSILON)
        return 1;

    if(candidate_bid < current_bid - BID_EPSILON)
        return 0;

    float candidate_capability =
        capability_score(candidate, incident);

    float current_capability =
        capability_score(current, incident);

    if(candidate_capability >
       current_capability + BID_EPSILON)
    {
        return 1;
    }

    if(candidate_capability <
       current_capability - BID_EPSILON)
    {
        return 0;
    }

    float candidate_equipment =
        equipment_score(candidate, incident);

    float current_equipment =
        equipment_score(current, incident);

    if(candidate_equipment >
       current_equipment + BID_EPSILON)
    {
        return 1;
    }

    if(candidate_equipment <
       current_equipment - BID_EPSILON)
    {
        return 0;
    }

    float candidate_distance =
        distance_score(candidate, incident);

    float current_distance =
        distance_score(current, incident);

    if(candidate_distance >
       current_distance + BID_EPSILON)
    {
        return 1;
    }

    if(candidate_distance <
       current_distance - BID_EPSILON)
    {
        return 0;
    }

    if(candidate->stamina > current->stamina)
        return 1;

    if(candidate->stamina < current->stamina)
        return 0;

    return candidate_team < current_team;
}
