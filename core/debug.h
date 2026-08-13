#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

/*
 * Platform-provided trace sink. Gated at call sites by
 * g_simulation_trace_enabled so it costs nothing when trace is off.
 * Laptop implementation: vfprintf to stderr (matches every other
 * [DIAG ...] line in this codebase; app.py's batch endpoint parses
 * stderr specifically, not stdout - see debug.c for why).
 * Board implementation: routes to UART/USB log sink (TODO(usb)).
 * Signature intentionally mirrors printf() so existing call sites need
 * only a name change, not a rewrite.
 */
void debug_trace(const char *fmt, ...);

void print_world_state(void);
void initialize_test_scenario(void);
/*
 * Static environment layout - buildings, epicenter - as a one-shot
 * STATICENV JSON line on stdout. Unlike debug_emit_live_frame(), this
 * doesn't change tick-to-tick (buildings/roads/districts are fixed at
 * world-gen time), so it's meant to be called once per run, not every
 * tick, keeping the dashboard's per-tick LIVEFRAME payload smaller.
 */
void debug_emit_static_environment(void);
void debug_emit_live_frame(uint32_t tick);

void debug_print_generator_evaluation(uint32_t tick,
                                      uint8_t load,
                                      uint32_t probability,
                                      uint32_t roll,
                                      uint8_t spawned,
                                      uint8_t available_teams,
                                      uint8_t en_route_teams,
                                      uint8_t on_scene_teams,
                                      uint8_t resting_teams,
                                      uint8_t open_incidents,
                                      uint8_t assigned_incidents,
                                      uint8_t in_progress_incidents,
                                      uint8_t active_system_tasks,
                                      uint16_t degraded_cells,
                                      uint32_t next_evaluation_tick);

void debug_print_generator_selection(const uint32_t *weights,
                                     uint32_t weight_count,
                                     uint32_t total_weight,
                                     uint32_t type_roll,
                                     uint8_t selected_type);

#endif