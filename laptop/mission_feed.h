#ifndef MISSION_FEED_H
#define MISSION_FEED_H

/*
 * Laptop -> board mission data (`tron ... --mission-feed`).
 *
 * The laptop is the world authority (core/mission_link.h): boards never
 * generate a world, they wait for MissionLinkRoster_t frames and then work
 * the incidents and faults the laptop sends. Nothing on the laptop used to
 * produce those frames, so real boards sat in their roster wait forever.
 *
 * In feed mode this simulator writes every frame the laptop owes the
 * boards to stdout, one per line:
 *
 *     MLFRAME <tick> <hex bytes of the complete USART1 wire frame>
 *
 * - every team's roster once, then clock tick 0, before the first tick;
 * - an incident frame whenever an incident's wire fields change (new,
 *   discovered, reprioritised, assigned, next stage, resolved) - but not
 *   for work_remaining alone, which on a board mission is the boards'
 *   own progress (laptop/mission_mirror.h);
 * - the same for search sweep tasks (INCIDENT_UNSEARCHED_SECTOR from the
 *   system task pool), which on a board mission are only created for
 *   districts below the trust threshold (ZONE_SWEEP_DISPATCH_TRUST_
 *   PERCENT) and move to a new frontier cell as coverage grows, and for
 *   evacuee runs (INCIDENT_EVACUEE_OFFBOARD): safe zone, then gateway;
 * - the safe zone/gateway sites and every route map row before the
 *   first tick, then each map row whose cells changed;
 * - a fault frame whenever a team's active/severity fault state changes;
 * - after all of those, the clock frame for the tick just finished,
 *   which is what lets the boards run that tick (core/mission_link.h
 *   section 5b).
 *
 * laptop/tools/telemetry_bridge.py's MissionFeed reads these lines, paces
 * them onto the serial port, and re-sends rosters, open incidents and the
 * clock periodically (a frame relayed over LoRa can be lost). Other stdout
 * lines (LIVEFRAME/STATICENV for the dashboard) are handled separately.
 */

#include <stdint.h>

/* Emits every team's roster. Call once after the world is generated. */
void mission_feed_begin(void);

/* Emits changed incident, sweep, evacuee-run, fault and map frames, then
   the clock. Call after each sim_tick(). */
void mission_feed_tick(uint32_t tick);

/* Emits only changed incident/sweep/evacuee-run frames. Call after
   applying board reports between ticks: a stage a report just finished
   must reach the boards now, not a tick (up to 36 s) later while the
   bridge keeps re-sending the old stage. */
void mission_feed_tasks(uint32_t tick);

#endif /* MISSION_FEED_H */
