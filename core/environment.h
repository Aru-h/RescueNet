#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

#include <stdint.h>
#include "mission_state.h"

#define MAX_ENV_BUILDINGS 192
#define MAX_ENV_NAV_NODES 1024
#define MAX_ENV_NAV_EDGES 4096
#define ENV_NO_BUILDING 0xFFFF
#define ENV_NO_ROAD 0xFFFF
#define ENV_NO_NAV_NODE 0xFFFF
#define ENV_DAMAGE_NONE 0
#define ENV_DAMAGE_MINOR 25
#define ENV_DAMAGE_MODERATE 50
#define ENV_DAMAGE_SEVERE 75
#define ENV_DAMAGE_DESTROYED 100

#define ENV_INCIDENT_BIT(type) (1u << (type))
#define ENV_HAZARD_FIRE (1u << 0)
#define ENV_HAZARD_GAS  (1u << 1)
#define ENV_HAZARD_COLLAPSE_RISK (1u << 2)
#define MAX_ENV_PENDING_HAZARDS 16

typedef enum {
    DISTRICT_RESCUE_BASE = 0,
    DISTRICT_RESIDENTIAL,
    DISTRICT_COMMERCIAL,
    DISTRICT_INDUSTRIAL,
    DISTRICT_MEDICAL,
    DISTRICT_GOVERNMENT,
    DISTRICT_EDUCATION,
    DISTRICT_MIXED_USE,
    DISTRICT_PARK,
    DISTRICT_WATERFRONT,
    NUM_DISTRICT_TYPES
} DistrictType_t;

typedef enum {
    ROAD_NONE = 0,
    ROAD_SERVICE,
    ROAD_LOCAL,
    ROAD_COLLECTOR,
    ROAD_ARTERIAL
} RoadClass_t;

typedef enum {
    BUILDING_NONE = 0,
    BUILDING_RESIDENTIAL,
    BUILDING_APARTMENT,
    BUILDING_COMMERCIAL,
    BUILDING_OFFICE,
    BUILDING_INDUSTRIAL,
    BUILDING_MEDICAL,
    BUILDING_CIVIC,
    BUILDING_EDUCATION,
    BUILDING_PARKING,
    BUILDING_UTILITY
} BuildingType_t;

typedef enum {
    INFRA_NONE = 0,
    INFRA_SAFE_ZONE,
    INFRA_GATEWAY,
    INFRA_BRIDGE,
    INFRA_INTERSECTION,
    INFRA_OPEN_SPACE,
    INFRA_UTILITY
} InfrastructureType_t;

typedef struct {
    uint8_t district;
    uint8_t road_class;
    uint16_t district_id;
    uint16_t road_id;
    uint16_t building_id;
    uint16_t nav_node_id;
    uint8_t infrastructure_type;
    uint8_t base_safety;
    uint8_t travel_cost;
    uint8_t navigable;
    uint8_t flood;
    uint8_t safe_zone_candidate;
    uint8_t terrain_elevation;
    uint8_t terrain_slope;
    uint8_t soil_condition;
    uint8_t road_capacity;
    uint8_t bridge;
    uint8_t district_density;
    uint8_t road_damage;
    uint8_t road_blocked;
    uint8_t infrastructure_damage;
    uint8_t hazard_flags;
    uint32_t spawned_incident_mask;
} EnvironmentCell_t;

typedef struct {
    uint16_t building_id;
    uint8_t building_type;
    uint8_t district;
    uint16_t district_id;
    int16_t loc_x;
    int16_t loc_y;
    uint8_t footprint;
    uint8_t footprint_width;
    uint8_t footprint_height;
    uint8_t height_m;
    uint8_t floors;
    uint8_t occupancy;
    uint8_t expected_civilians;
    uint8_t risk_level;
    uint8_t construction_quality;
    uint8_t collapse_resistance;
    uint8_t seismic_vulnerability;
    uint8_t fire_susceptibility;
    uint8_t gas_infrastructure;
    uint8_t electrical_infrastructure;
    uint8_t access_road_class;
    uint8_t access_count;
    uint8_t damage_state;
    uint8_t hazard_flags;
    uint32_t spawned_incident_mask;
} EnvironmentBuilding_t;

typedef struct {
    uint16_t node_id;
    int16_t x;
    int16_t y;
    uint8_t kind;
} EnvironmentNavNode_t;

typedef struct {
    uint16_t from_node;
    uint16_t to_node;
    uint16_t cost;
    uint8_t road_class;
    uint8_t blocked;
} EnvironmentNavEdge_t;

typedef struct {
    uint8_t fire_spread_enabled;
    uint8_t gas_escalation_enabled;
    uint8_t aftershock_enabled;
    uint8_t road_degradation_enabled;
    uint8_t fire_spread_interval_ticks;
    uint8_t gas_escalation_interval_ticks;
    uint8_t aftershock_interval_ticks;
    uint8_t road_degradation_interval_ticks;
    uint8_t fire_spread_probability;
    uint8_t gas_explosion_probability;
    uint8_t aftershock_collapse_probability;
    uint8_t road_degradation_amount;
} HazardPropagationConfig_t;

typedef struct {
    uint8_t occupied;
    uint8_t incident_type;
    int16_t loc_x;
    int16_t loc_y;
} EnvironmentHazardEvent_t;

typedef struct {
    uint32_t seed;
    uint8_t generated;
    uint8_t earthquake_applied;
    int16_t earthquake_epicenter_x;
    int16_t earthquake_epicenter_y;
    uint8_t earthquake_intensity;
    uint16_t district_count[NUM_DISTRICT_TYPES];
    uint16_t road_cell_count;
    uint16_t navigable_cell_count;
    uint16_t blocked_road_count;
    uint16_t damaged_infrastructure_count;
    uint16_t district_total;
    uint16_t bridge_count;
    uint16_t safe_zone_candidate_count;
    uint16_t nav_node_count;
    uint16_t nav_edge_count;
    uint8_t validation_passed;
    uint8_t connected_districts;
    uint32_t fire_spread_count;
    uint32_t gas_explosion_count;
    uint32_t aftershock_collapse_count;
    uint32_t road_degradation_count;
    uint16_t building_count;
    uint16_t damaged_building_count;
} EnvironmentSummary_t;

extern EnvironmentCell_t g_environment[GRID_WIDTH][GRID_HEIGHT];
extern EnvironmentBuilding_t g_environment_buildings[MAX_ENV_BUILDINGS];
extern EnvironmentSummary_t g_environment_summary;
extern HazardPropagationConfig_t g_hazard_config;

void generate_environment(uint32_t seed);
void environment_apply_earthquake(uint32_t seed);
uint8_t environment_cell_is_navigable(int x, int y);
uint8_t environment_cell_is_flood(int x, int y);
uint8_t environment_cell_travel_cost(int x, int y);
uint8_t environment_cell_base_safety(int x, int y);
void environment_register_safe_zone(uint8_t safezone_id, int16_t x, int16_t y);
void environment_register_gateway(uint8_t gateway_id, int16_t x, int16_t y);
void environment_set_cell_operational_state(int x, int y, uint8_t travel_cost, uint8_t navigable, uint8_t road_blocked);
uint8_t environment_find_safe_zone_site(uint8_t index, uint8_t total, int16_t *x, int16_t *y);
uint8_t environment_find_gateway_site(uint8_t index, int16_t *x, int16_t *y);
uint8_t environment_find_incident_site(IncidentType_t type, int16_t *x, int16_t *y);
uint8_t environment_get_district_center(uint8_t district_id, int16_t *x, int16_t *y);
uint8_t environment_find_incident_site_in_zone(IncidentType_t type, uint8_t zone_id, int16_t *x, int16_t *y);
void environment_update_hazard_propagation(uint32_t tick);
uint8_t environment_pop_pending_hazard_event(IncidentType_t *type, int16_t *x, int16_t *y);
const char *environment_district_name(uint8_t district);
const char *environment_road_name(uint8_t road_class);

#endif
