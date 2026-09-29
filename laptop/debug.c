#include "consensus.h"
#include "mission_state.h"
#include "incident.h"
#include "mission_state_utils.h"
#include "grid.h"
#include "random.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "debug.h"
#include "simulation.h"
#include "environment.h"
#include "sim_runner.h"
#include "fault_config.h"
#include "path.h"
#include "mission_link_override.h"

/* Part 4's non-blocking stdin poll (debug_live_override_poll() below) -
 * desktop test tooling only, same "no platform.h wrapping" precedent
 * every other laptop-only helper in this file already follows. Neither
 * branch is used outside that one function. */
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/select.h>
#endif

/*
 * Laptop implementation of the debug_trace() sink declared in debug.h.
 * Mirrors printf() so core/ trace call sites don't need platform
 * knowledge. The board implementation (board/STM32N6570-DK/debug.c) routes
 * the same calls to a UART/USB log sink instead of stdout.
 */
void debug_trace(const char *fmt, ...)
{
    /* stderr, not stdout: [DIAG ...] lines elsewhere in this codebase
     * (main.c's summary/work/throughput prints, simulation.c's
     * rest-cycle tracing below) all go to stderr, and app.py's batch
     * (non---live) endpoint captures stdout/stderr separately
     * (subprocess.run(..., capture_output=True)) then parses only
     * result.stderr for [DIAG ...] lines. --live mode merges the two
     * streams (stderr=subprocess.STDOUT) so it doesn't care either
     * way, but batch mode does - stdout here would silently vanish
     * from what the dashboard parses. */
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static const char *generator_load_to_string(uint8_t load)
{
    switch(load)
    {
        case 0: return "IDLE";
        case 1: return "LOW";
        case 2: return "MODERATE";
        case 3: return "HIGH";
        case 4: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

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
                                      uint32_t next_evaluation_tick)
{
    if(!g_simulation_trace_enabled) return;

    printf("[GEN] tick=%lu load=%s prob=%lu roll=%lu spawn=%s | teams(A=%u,E=%u,O=%u,R=%u) | incidents(O=%u,A=%u,P=%u) | system=%u degraded=%u next=%lu\n",
           (unsigned long)tick,
           generator_load_to_string(load),
           (unsigned long)probability,
           (unsigned long)roll,
           spawned ? "YES" : "NO",
           available_teams,
           en_route_teams,
           on_scene_teams,
           resting_teams,
           open_incidents,
           assigned_incidents,
           in_progress_incidents,
           active_system_tasks,
           degraded_cells,
           (unsigned long)next_evaluation_tick);
}

void debug_print_generator_selection(const uint32_t *weights,
                                     uint32_t weight_count,
                                     uint32_t total_weight,
                                     uint32_t type_roll,
                                     uint8_t selected_type)
{
    if(!g_simulation_trace_enabled) return;

    printf("[GEN-TYPE] weights=[");

    for(uint32_t i = 0; i < weight_count; i++)
    {
        printf("%s%lu",
               i == 0 ? "" : ",",
               (unsigned long)weights[i]);
    }

    printf("] total=%lu roll=%lu selected_type=%u\n",
           (unsigned long)total_weight,
           (unsigned long)type_roll,
           selected_type);
}


/*
 * Static layout - buildings, roads, districts don't move once
 * generated, so this is sent exactly once per run, not every tick, to
 * keep LIVEFRAME payloads small. Desktop test tooling only (used by
 * the live dashboard) - not part of the firmware port surface, so no
 * platform.h wrapping.
 */
void debug_emit_static_environment(void)
{
    printf("STATICENV {\"buildings\":[");
    uint8_t first_b = 1;
    for(uint16_t i = 0; i < MAX_ENV_BUILDINGS; i++)
    {
        const EnvironmentBuilding_t *b = &g_environment_buildings[i];
        if(b->footprint == 0)
            continue; /* unused slot */
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u,\"type\":%u,"
               "\"floors\":%u,\"damage\":%u,\"risk\":%u,\"hazard_flags\":%u}",
               first_b ? "" : ",",
               b->building_id, b->loc_x, b->loc_y, b->footprint_width, b->footprint_height,
               b->building_type, b->floors, b->damage_state, b->risk_level, b->hazard_flags);
        first_b = 0;
    }
    printf("],\"epicenter\":{\"active\":%u,\"x\":%d,\"y\":%d,\"intensity\":%u}}\n",
           g_environment_summary.earthquake_applied,
           g_environment_summary.earthquake_epicenter_x,
           g_environment_summary.earthquake_epicenter_y,
           g_environment_summary.earthquake_intensity);
    fflush(stdout);
}

/*
 * Part 3 (dashboard terrain map): per-cell travel cost, navigable,
 * flood, road class and district id, as one compact STATICMAP line -
 * rows of hex, one row per y, each cell packed as 8 hex chars (flags
 * byte, travel_cost byte, district_id u16). Terrain is normally fixed
 * at world-gen time (same as debug_emit_static_environment()'s
 * buildings), so this only actually prints when at least one cell's
 * value differs from the last emission - laptop/main.c calls it once
 * before the tick loop (always prints, nothing emitted yet) and again
 * every tick (usually a no-op; flood is the one field that can change
 * mid-run). Purely additional stdout output: reads g_environment
 * through the same accessors/fields debug_emit_live_frame()'s
 * hazard_cells loop already reads, doesn't touch the CSV path, and has
 * no effect on simulator behavior.
 */
static uint8_t s_static_map_emitted = 0;
static uint8_t s_static_map_prev_flags[GRID_WIDTH][GRID_HEIGHT];
static uint8_t s_static_map_prev_cost[GRID_WIDTH][GRID_HEIGHT];
static uint16_t s_static_map_prev_district[GRID_WIDTH][GRID_HEIGHT];

static uint8_t static_map_cell_flags(uint8_t gx, uint8_t gy)
{
    uint8_t nav = environment_cell_is_navigable(gx, gy) ? 1u : 0u;
    uint8_t flood = environment_cell_is_flood(gx, gy) ? 1u : 0u;
    uint8_t road_class = (uint8_t)(g_environment[gx][gy].road_class & 0x07u);
    uint8_t road_blocked = g_environment[gx][gy].road_blocked ? 1u : 0u;
    return (uint8_t)(nav | (uint8_t)(flood << 1) | (uint8_t)(road_class << 2) | (uint8_t)(road_blocked << 5));
}

void debug_emit_static_map_if_changed(void)
{
    uint8_t changed = !s_static_map_emitted;
    for(uint8_t gx = 0; gx < GRID_WIDTH && !changed; gx++)
    {
        for(uint8_t gy = 0; gy < GRID_HEIGHT; gy++)
        {
            uint8_t flags = static_map_cell_flags(gx, gy);
            uint8_t cost = environment_cell_travel_cost(gx, gy);
            uint16_t district = g_environment[gx][gy].district_id;
            if(flags != s_static_map_prev_flags[gx][gy] ||
               cost != s_static_map_prev_cost[gx][gy] ||
               district != s_static_map_prev_district[gx][gy])
            {
                changed = 1;
                break;
            }
        }
    }
    if(!changed)
        return;

    printf("STATICMAP {\"width\":%u,\"height\":%u,\"rows\":[", (unsigned)GRID_WIDTH, (unsigned)GRID_HEIGHT);
    for(uint8_t gy = 0; gy < GRID_HEIGHT; gy++)
    {
        printf("%s\"", gy == 0 ? "" : ",");
        for(uint8_t gx = 0; gx < GRID_WIDTH; gx++)
        {
            uint8_t flags = static_map_cell_flags(gx, gy);
            uint8_t cost = environment_cell_travel_cost(gx, gy);
            uint16_t district = g_environment[gx][gy].district_id;
            printf("%02x%02x%04x", flags, cost, district);
            s_static_map_prev_flags[gx][gy] = flags;
            s_static_map_prev_cost[gx][gy] = cost;
            s_static_map_prev_district[gx][gy] = district;
        }
        printf("\"");
    }
    printf("]}\n");
    fflush(stdout);
    s_static_map_emitted = 1;
}

/* --- Part 4: sim-mode manual override over --live's own stdin -----------
 * See core/debug.h's doc comment for the line protocol and the full
 * rationale (why text lines rather than the board's binary frame, why
 * rejection reasons never use ground-truth fault state). Everything
 * below is reachable ONLY when main.c's tick loop is in --live mode -
 * the board's USART1 override path (core/mission_link_override.c,
 * laptop/tools/app.py's /api/board/override) is a completely separate
 * code path, untouched by any of this.
 */
#define LIVE_OVERRIDE_LINE_MAX 128u
/* How many ticks old a request may be before it's rejected as stale -
 * generous enough to absorb ordinary UI/network round-trip latency
 * (app.py's SSE loop, the confirm click, the write to this process's
 * stdin) without accepting a request built against a world state the
 * sim has long since moved past. */
#define LIVE_OVERRIDE_STALE_TICKS 20u

static char s_override_line[LIVE_OVERRIDE_LINE_MAX];
static uint32_t s_override_line_len;
static int s_override_stdin_open = 1;
/* Set once a line exceeds s_override_line's capacity, until the '\n'
 * that ends it - every further byte of that same overlong line is
 * discarded (not just the part that didn't fit), so exactly one
 * bad_format ack is sent for the whole line and normal accumulation
 * resumes cleanly on the next one, with no desync. */
static int s_override_line_overflowed;

void debug_live_override_init(void)
{
    s_override_line_len = 0;
    s_override_stdin_open = 1;
    s_override_line_overflowed = 0;
}

/* Finds a REAL incident (g_incidents only - NOT g_system_tasks) by id.
 * core/cbba.c's own cbba_find_incident_slot_by_id() (static, not
 * visible outside that file) is what cbba_apply_override_pin() uses
 * every tick to keep a pin alive, and it ONLY searches g_incidents -
 * a pin set on a system-task id would validate here, ack "applied",
 * and then self-clear on the very next tick with no further signal,
 * which is exactly the "silently dropped" failure mode overrides must
 * never have. So this deliberately matches cbba.c's own scope rather
 * than the wider dual-pool search debug_emit_live_frame() does for
 * the dashboard's "incidents" JSON array (which merges both pools
 * purely for map display) - live_override_is_system_task() below is
 * how a system-task id gets its own clear rejection reason instead of
 * a misleading "unknown_incident". */
static const IncidentState_t *live_override_find_incident(uint32_t incident_id)
{
    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        if(g_incidents[i].slot_occupied && g_incidents[i].incident_id == incident_id)
            return &g_incidents[i];
    }
    return NULL;
}

/* True if incident_id exists, but only as a system task - the id is
 * real (shows up on the dashboard map, drag-droppable there like any
 * other marker), it just isn't one cbba_apply_override_pin() can pin
 * a team to. Distinguishing this from "doesn't exist at all" is what
 * lets the reject reason be honest instead of just "unknown_incident". */
static int live_override_is_system_task(uint32_t incident_id)
{
    for(uint16_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(g_system_tasks[i].slot_occupied && g_system_tasks[i].incident_id == incident_id)
            return 1;
    }
    return 0;
}

/* "Is this team currently unavailable for a manual reassignment", per
 * ONLY what the system itself has already concluded about it - never
 * ground-truth fault state (core/fault_config.h's TeamFaultState_t.
 * active_mask), which would leak a hidden injected fault to the
 * operator through the reject reason before any detector actually
 * found it. The recovery manager's own `degraded` flag and peer
 * consensus's ELEVATED tier are both "the system observed and
 * concluded this", the same ground-truth-vs-observed line
 * docs/architecture/CONSENSUS_DESIGN.md draws everywhere else in this project (see e.g.
 * laptop/tools/static/dashboard/timeline.js's top-of-file comment on
 * the exact same distinction for the Detection Timeline panel). */
static int live_override_team_unavailable(uint8_t team_id)
{
    const TeamState_t *team = &g_teams[team_id];

    if(team->degraded)
        return 1;

#if VIRTUAL_BUS_ENABLED
    if(consensus_get_result(team_id, CONSENSUS_CLASS_MOBILITY).tier == CONSENSUS_TIER_ELEVATED)
        return 1;
    if(consensus_get_result(team_id, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_ELEVATED)
        return 1;
#endif

    return 0;
}

/* One OVERRIDEACK JSON line per request - reason is NULL (prints as
 * JSON null) on success, one of bad_team/unknown_incident/
 * system_task/incident_finished/team_unavailable/stale_tick/
 * future_tick on rejection. effective_tick is always the tick this was actually
 * processed on (== requested_tick on success in practice, since a
 * stale/future mismatch is itself a rejection) - laptop/tools/app.py
 * forwards this line over SSE unparsed, same as every other debug.c
 * JSON line. */
static void live_override_emit_ack(uint32_t requested_tick, uint32_t effective_tick,
                                    uint8_t team_id, uint32_t incident_id,
                                    int applied, const char *reason)
{
    printf("OVERRIDEACK {\"team_id\":%u,\"incident_id\":%u,\"requested_tick\":%u,"
           "\"effective_tick\":%u,\"result\":\"%s\",\"reason\":%s%s%s}\n",
           (unsigned)team_id, incident_id, requested_tick, effective_tick,
           applied ? "applied" : "rejected",
           reason ? "\"" : "", reason ? reason : "null", reason ? "\"" : "");
    fflush(stdout);
}

/* Ack for a line that never resolved into a real request - a bad_format
 * (recognized command, malformed arguments) or unknown_command
 * (unrecognized command word) rejection. Unlike live_override_emit_ack()
 * above, there's no real team_id/incident_id to report (the line's own
 * shape never established one), so those come back as JSON null rather
 * than a fabricated 0 - the UI must never confuse "field is null because
 * the line was malformed" with "field is legitimately team/incident 0".
 * requested_tick is echoed only if that much of the line did parse. */
static void live_override_emit_malformed_ack(uint32_t effective_tick, int tick_known,
                                               uint32_t requested_tick, const char *reason)
{
    printf("OVERRIDEACK {\"team_id\":null,\"incident_id\":null,\"requested_tick\":");
    if(tick_known)
        printf("%u", requested_tick);
    else
        printf("null");
    printf(",\"effective_tick\":%u,\"result\":\"rejected\",\"reason\":\"%s\"}\n",
           effective_tick, reason);
    fflush(stdout);
}

/* Parses and applies one already newline-stripped, non-empty line.
 * Every non-empty line gets exactly one OVERRIDEACK: a recognized
 * command word (OVERRIDE/OVERRIDE_CLEAR) whose arguments don't parse
 * cleanly - wrong field count, a non-numeric field, a field too large
 * for its type, or trailing junk after the last field - is
 * "bad_format"; anything else is "unknown_command". This mirrors the
 * "don't crash on malformed input" policy laptop/mission_mirror.c's
 * mission_mirror_ingest_line() already uses for its own stdin-line
 * reader, but a reply is always owed here (a human or a UI is waiting
 * on the other end of this one), unlike that fire-and-forget feed. */
static void live_override_process_line(const char *line, uint32_t tick)
{
    char cmd[24];
    size_t cmd_len = 0;
    while(line[cmd_len] != '\0' && line[cmd_len] != ' ' && cmd_len < sizeof(cmd) - 1u)
    {
        cmd[cmd_len] = line[cmd_len];
        cmd_len++;
    }
    cmd[cmd_len] = '\0';
    const char *rest = line + cmd_len;

    if(strcmp(cmd, "OVERRIDE_CLEAR") == 0)
    {
        unsigned long requested_tick = 0, team_id = 0;
        int consumed = -1;
        int n = sscanf(rest, " %lu %lu %n", &requested_tick, &team_id, &consumed);
        int tick_overflow = n >= 1 && requested_tick > 0xFFFFFFFFUL;
        int trailing_junk = consumed >= 0 && rest[consumed] != '\0';
        if(n != 2 || tick_overflow || (n >= 2 && team_id > 0xFFFFFFFFUL) || trailing_junk)
        {
            live_override_emit_malformed_ack(tick, n >= 1 && !tick_overflow, (uint32_t)requested_tick, "bad_format");
            return;
        }
        if(team_id >= NUM_TEAMS)
        {
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, 0, 0, "bad_team");
            return;
        }
        /* Reports the incident that was actually pinned (read before
         * clearing), not 0 - more useful in the ack than an
         * unconditional zero, and costs nothing extra to capture. */
        uint32_t cleared_incident_id = g_cbba_agent_state[team_id].override_incident_id;
        g_cbba_agent_state[team_id].override_active = 0u;
        live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, cleared_incident_id, 1, NULL);
        return;
    }

    if(strcmp(cmd, "OVERRIDE") == 0)
    {
        unsigned long requested_tick = 0, team_id = 0, incident_id = 0;
        int consumed = -1;
        int n = sscanf(rest, " %lu %lu %lu %n", &requested_tick, &team_id, &incident_id, &consumed);
        int tick_overflow = n >= 1 && requested_tick > 0xFFFFFFFFUL;
        int inc_overflow = n >= 3 && incident_id > 0xFFFFFFFFUL;
        int trailing_junk = consumed >= 0 && rest[consumed] != '\0';
        if(n != 3 || tick_overflow || (n >= 2 && team_id > 0xFFFFFFFFUL) || inc_overflow || trailing_junk)
        {
            live_override_emit_malformed_ack(tick, n >= 1 && !tick_overflow, (uint32_t)requested_tick, "bad_format");
            return;
        }
        if(team_id >= NUM_TEAMS)
        {
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "bad_team");
            return;
        }
        /* Two distinct staleness directions: a request timestamped
         * AFTER the tick this process has actually reached is
         * meaningless (nothing to compare it against yet - could only
         * happen from a clock/tick mismatch bug on the sender side),
         * and one timestamped too far BEFORE it was built against a
         * world state the sim has long since moved past. */
        if(requested_tick > tick)
        {
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "future_tick");
            return;
        }
        if(tick - requested_tick > LIVE_OVERRIDE_STALE_TICKS)
        {
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "stale_tick");
            return;
        }
        if(live_override_team_unavailable((uint8_t)team_id))
        {
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "team_unavailable");
            return;
        }

        const IncidentState_t *inc = live_override_find_incident((uint32_t)incident_id);
        if(!inc)
        {
            if(live_override_is_system_task((uint32_t)incident_id))
            {
                live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "system_task");
                return;
            }
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "unknown_incident");
            return;
        }
        if(inc->status == (uint8_t)INC_RESOLVED)
        {
            live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 0, "incident_finished");
            return;
        }

        MissionLinkOverride_t msg;
        msg.team_id = (uint8_t)team_id;
        msg.incident_id = (uint32_t)incident_id;
        /* The exact function the board's own USART1 RX path calls
         * (core/mission_link_override.h step 4) - in this sim build
         * COMMS_TRANSPORT_TEAMS_PER_BOARD == NUM_TEAMS (see Makefile),
         * so comms_transport_board_id_for_team() always resolves to
         * this same in-process board and this always takes the
         * local-apply branch, never the relay path. */
        mission_link_override_route_or_apply(&msg);
        live_override_emit_ack((uint32_t)requested_tick, tick, (uint8_t)team_id, (uint32_t)incident_id, 1, NULL);
        return;
    }

    /* A real line, just not a command word we recognize. */
    live_override_emit_malformed_ack(tick, 0, 0, "unknown_command");
}

/* One non-blocking poll of stdin, called once per tick from main.c's
 * --live loop. Never blocks: a zero-timeout select() (POSIX) or a
 * PeekNamedPipe() size check (Windows) both return immediately
 * whether or not data is waiting, so a tick with nothing on stdin
 * costs one cheap syscall, not a stall. EOF/closed stdin sets
 * s_override_stdin_open = 0 so every later call is a single branch
 * (no syscall at all) rather than repeatedly probing a channel that's
 * gone - the sim keeps running exactly as if --live had never gained
 * this capability. */
void debug_live_override_poll(uint32_t tick)
{
    if(!s_override_stdin_open)
        return;

    char chunk[256];
    long n_read = 0;

#if defined(_WIN32)
    {
        /* Best-effort: not compiled/tested on this project's toolchain
         * (no Windows build target exists anywhere in this repo today -
         * see the Makefile), included per explicit instruction that
         * override support on Windows must either work or be compiled
         * out. PeekNamedPipe() is the standard non-blocking-size-check
         * for a pipe HANDLE, mirroring select()'s zero-timeout role
         * below; app.py always launches this process with a real pipe
         * for stdin, so the "not a pipe" case is a defensive fallback,
         * not the expected shape. */
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0, got = 0;
        if(h == INVALID_HANDLE_VALUE || h == NULL ||
           !PeekNamedPipe(h, NULL, 0, NULL, &available, NULL) || available == 0)
            return;
        DWORD to_read = available < (DWORD)sizeof(chunk) ? available : (DWORD)sizeof(chunk);
        if(!ReadFile(h, chunk, to_read, &got, NULL) || got == 0)
        {
            s_override_stdin_open = 0;
            return;
        }
        n_read = (long)got;
    }
#else
    {
        fd_set readable;
        struct timeval zero_timeout;
        zero_timeout.tv_sec = 0;
        zero_timeout.tv_usec = 0;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        if(select(STDIN_FILENO + 1, &readable, NULL, NULL, &zero_timeout) <= 0)
            return; /* nothing waiting (or select() itself was interrupted) - not an error */

        ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if(got <= 0)
        {
            s_override_stdin_open = 0;
            return;
        }
        n_read = (long)got;
    }
#endif

    for(long i = 0; i < n_read; i++)
    {
        char c = chunk[i];
        if(c == '\n')
        {
            if(s_override_line_overflowed)
            {
                live_override_emit_malformed_ack(tick, 0, 0, "bad_format");
                s_override_line_overflowed = 0;
            }
            else if(s_override_line_len > 0)
            {
                /* An all-whitespace line still counts as non-empty here
                 * (e.g. a single space) and is handled the same as any
                 * other line - live_override_process_line() itself
                 * resolves that to unknown_command, not a silent drop. */
                s_override_line[s_override_line_len] = '\0';
                live_override_process_line(s_override_line, tick);
            }
            /* else: an empty line - ignored silently, not an error. */
            s_override_line_len = 0;
        }
        else if(s_override_line_overflowed)
        {
            /* still discarding the remainder of an overlong line */
        }
        else if(s_override_line_len + 1u < LIVE_OVERRIDE_LINE_MAX)
        {
            s_override_line[s_override_line_len++] = (char)c;
        }
        else
        {
            s_override_line_overflowed = 1;
            s_override_line_len = 0;
        }
    }
}

/* Emits one line of JSON to stdout describing the full world state for
 * this tick: every team's position/status/stamina/fault info, and every
 * active incident + system task. Desktop test tooling only (used by the
 * live dashboard) - not part of the firmware port surface, so no
 * platform.h wrapping. Caller is responsible for pacing (sleep) between
 * ticks if a human needs to watch it happen instead of it flying by. */
void debug_emit_live_frame(uint32_t tick)
{
    extern uint32_t g_diag_incidents_resolved[NUM_TEAMS];
    extern uint32_t g_diag_stage_completions[NUM_TEAMS];
    extern uint32_t g_diag_work_progress_calls;
    extern uint32_t g_diag_in_progress_entries;
    extern uint32_t g_diag_ticks_resting[NUM_TEAMS];
    extern uint32_t g_diag_ticks_working[NUM_TEAMS];
    extern uint32_t g_diag_ticks_available[NUM_TEAMS];
    extern uint32_t g_diag_safezone_fallback_count;
    extern uint32_t g_diag_discarded_signal_count;

    /* Cumulative, monotonic totals across all teams - these are the
       authoritative counters the sim itself tracks (incident.c,
       recovery_manager.c). Sent every tick so the frontend can build a
       live throughput history without waiting for end-of-run [DIAG]
       output. Per-team breakdowns are included on each team object
       below rather than duplicated here. */
    uint32_t total_incidents_resolved = 0, total_stage_completions = 0;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        total_incidents_resolved += g_diag_incidents_resolved[t];
        total_stage_completions += g_diag_stage_completions[t];
    }

    /* Virtual peer bus counters (docs/architecture/CONSENSUS_DESIGN.md 5.3). Always present so
       the frame schema does not depend on the build; zeros without the bus. */
    unsigned vbus_sent = 0, vbus_delivered = 0, vbus_dropped = 0, vbus_airtime_ms = 0;
#if VIRTUAL_BUS_ENABLED
    {
        VbusStats_t vs = vbus_get_stats();

        vbus_sent = vs.sent;
        vbus_delivered = vs.delivered;
        vbus_dropped = vs.dropped;
        vbus_airtime_ms = vs.airtime_ms_total;
    }
#endif

    printf("LIVEFRAME {\"tick\":%u,"
           "\"incidents_resolved\":%u,\"stage_completions\":%u,"
           "\"work_progress_calls\":%u,\"in_progress_entries\":%u,"
           "\"safezone_fallback_count\":%u,\"discarded_signal_count\":%u,"
           "\"vbus\":{\"sent\":%u,\"delivered\":%u,\"dropped\":%u,\"airtime_ms\":%u},"
           "\"teams\":[",
           tick, total_incidents_resolved, total_stage_completions,
           g_diag_work_progress_calls, g_diag_in_progress_entries,
           g_diag_safezone_fallback_count, g_diag_discarded_signal_count,
           vbus_sent, vbus_delivered, vbus_dropped, vbus_airtime_ms);
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];

        /* Resolve the task this team is currently assigned to (if any) so
           the dashboard can show "where is this team actually headed"
           without duplicating incident-lookup logic client-side. */
        int16_t dest_x = -1, dest_y = -1;
        uint16_t dest_id = 0;
        uint8_t has_dest = 0;
        if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
           team->assigned_task_slot < NUM_INCIDENTS &&
           g_incidents[team->assigned_task_slot].slot_occupied)
        {
            const IncidentState_t *task = &g_incidents[team->assigned_task_slot];
            dest_x = task->loc_x;
            dest_y = task->loc_y;
            dest_id = task->incident_id;
            has_dest = 1;
        }
        else if(team->assigned_task_pool == TASK_POOL_SYSTEM &&
                team->assigned_task_slot < NUM_SYSTEM_TASKS &&
                g_system_tasks[team->assigned_task_slot].slot_occupied)
        {
            const IncidentState_t *task = &g_system_tasks[team->assigned_task_slot];
            dest_x = task->loc_x;
            dest_y = task->loc_y;
            dest_id = task->incident_id;
            has_dest = 1;
        }

        /* Peer consensus about this team as a target: tier per class
           [mobility, communication] (-1 = not built, 0 NONE, 1 LOCAL,
           2 ELEVATED) and the votes behind it [fault, clear, abstain]. */
        int tier_mob = CONSENSUS_TIER_NOT_BUILT, tier_comm = CONSENSUS_TIER_NOT_BUILT;
        unsigned mob_f = 0, mob_c = 0, mob_a = 0, comm_f = 0, comm_c = 0, comm_a = 0;
        unsigned peer_live = 0, peer_dark = 0, peer_fast = 0;
        /* Part 3 (dashboard consensus mesh): this team AS OBSERVER, bit j
           set if it heard target j's beacon on time this tick
           (PeerObservation_t.rx_on_time via consensus_get_peer_observation() -
           docs/architecture/CONSENSUS_DESIGN.md 2.6). Self-bit (j==t) is always 0: a team
           doesn't beacon itself. Lets the dashboard draw the real 10x10
           mesh instead of the earlier hub-and-spoke live/dark proxy. */
        unsigned heard_mask = 0;
#if VIRTUAL_BUS_ENABLED
        {
            ConsensusResult_t cm = consensus_get_result(t, CONSENSUS_CLASS_MOBILITY);
            ConsensusResult_t cc = consensus_get_result(t, CONSENSUS_CLASS_COMMUNICATION);

            tier_mob = (int)cm.tier;
            tier_comm = (int)cc.tier;
            mob_f = cm.n_fault; mob_c = cm.n_clear; mob_a = cm.n_abstain;
            comm_f = cc.n_fault; comm_c = cc.n_clear; comm_a = cc.n_abstain;
            peer_live = cm.n_live;
            peer_dark = cm.n_dark;
            peer_fast = cc.fast_path;

            for(uint8_t target = 0; target < NUM_TEAMS; target++)
            {
                if(target == t)
                    continue;
                PeerObservation_t obs;
                consensus_get_peer_observation(t, target, &obs);
                if(obs.rx_on_time)
                    heard_mask |= (1u << target);
            }
        }
#endif

        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"stamina\":%.2f,\"status\":%u,"
               "\"spec\":%u,\"pool\":%u,\"slot\":%u,\"fault_mask\":%u,"
               "\"degraded\":%u,\"hours\":%u,\"has_dest\":%u,\"dest_x\":%d,"
               "\"dest_y\":%d,\"dest_id\":%u,\"incidents_resolved\":%u,"
               "\"stage_completions\":%u,\"ticks_resting\":%u,"
               "\"ticks_working\":%u,\"ticks_available\":%u,"
               "\"consensus_tier\":[%d,%d],"
               "\"peer_votes\":{\"mob\":[%u,%u,%u],\"comm\":[%u,%u,%u],"
               "\"live\":%u,\"dark\":%u,\"fast\":%u},"
               "\"heard\":%u,"
               /* Part 4: reuses the pin state cbba_apply_override_pin()
                * (core/cbba.c) already reads every tick to force-inject
                * the incident into this team's bundle - no new state,
                * just exposing what already exists. That function
                * clears override_active on its own the moment the
                * pinned incident leaves INC_OPEN - i.e. the moment it's
                * genuinely committed (INC_ASSIGNED), typically just a
                * few ticks after the pin, NOT when it finishes: from
                * that tick on the assignment is carried by the sim's
                * own normal state (team->assigned_task_pool/slot,
                * incident->assigned_team), so there's nothing left for
                * a stale pin to do. See core/debug.h's protocol doc for
                * the full pinned -> committed -> resolved (or
                * interrupted) lifecycle - the dashboard just has to
                * keep reading this live, never latch a stale "still
                * overridden" client-side. */
               "\"override\":%u,\"override_incident_id\":%u}",
               t == 0 ? "" : ",",
               team->team_id, team->loc_x, team->loc_y, team->stamina,
               team->status, team->specialization,
               team->assigned_task_pool, team->assigned_task_slot,
               team->fault_state.active_mask, team->degraded,
               team->hours_worked, has_dest, dest_x, dest_y, dest_id,
               g_diag_incidents_resolved[t], g_diag_stage_completions[t],
               g_diag_ticks_resting[t], g_diag_ticks_working[t],
               g_diag_ticks_available[t],
               tier_mob, tier_comm, mob_f, mob_c, mob_a, comm_f, comm_c, comm_a,
               peer_live, peer_dark, peer_fast, heard_mask,
               g_cbba_agent_state[t].override_active, g_cbba_agent_state[t].override_incident_id);
    }
    printf("],\"incidents\":[");
    uint8_t first = 1;
    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        const IncidentState_t *inc = &g_incidents[i];
        if(!inc->slot_occupied)
            continue;
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"priority\":%u,\"type\":%u,"
               "\"status\":%u,\"assigned_team\":%u,\"support_team\":%u,"
               "\"req_capability\":%u,\"victims\":%u,\"work_remaining\":%u,"
               "\"discovered\":%u,\"floor\":%u,\"building_id\":%d,"
               "\"pool\":0}",
               first ? "" : ",",
               inc->incident_id, inc->loc_x, inc->loc_y, inc->priority,
               inc->incident_type, inc->status, inc->assigned_team,
               inc->support_team, inc->req_capability, inc->victim_count,
               inc->work_remaining, inc->discovered, inc->floor,
               inc->building_id == INCIDENT_NO_BUILDING ? -1 : (int)inc->building_id);
        first = 0;
    }
    for(uint16_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        const IncidentState_t *inc = &g_system_tasks[i];
        if(!inc->slot_occupied)
            continue;
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"priority\":%u,\"type\":%u,"
               "\"status\":%u,\"assigned_team\":%u,\"support_team\":%u,"
               "\"req_capability\":%u,\"victims\":%u,\"work_remaining\":%u,"
               "\"discovered\":%u,\"floor\":%u,\"building_id\":%d,"
               "\"pool\":1}",
               first ? "" : ",",
               inc->incident_id, inc->loc_x, inc->loc_y, inc->priority,
               inc->incident_type, inc->status, inc->assigned_team,
               inc->support_team, inc->req_capability, inc->victim_count,
               inc->work_remaining, inc->discovered, inc->floor,
               inc->building_id == INCIDENT_NO_BUILDING ? -1 : (int)inc->building_id);
        first = 0;
    }
    printf("],\"safezones\":[");
    for(uint8_t s = 0; s < MAX_SAFE_ZONES; s++)
    {
        const SafeZone_t *sz = &g_safezones[s];
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"capacity\":%u,\"evacuees\":%u}",
               s == 0 ? "" : ",",
               sz->safezone_id, sz->loc_x, sz->loc_y, sz->capacity, sz->evacuee_count);
    }
    printf("],\"hazard_cells\":[");
    /* Only cells that are actually blocked/damaged/hazardous are sent -
     * out of GRID_WIDTH*GRID_HEIGHT (1296) cells, sending all of them
     * every tick would bloat the frame for mostly-static terrain. Only
     * the exceptional ones are worth the map drawing anything special. */
    uint8_t first_h = 1;
    for(uint8_t gx = 0; gx < GRID_WIDTH; gx++)
    {
        for(uint8_t gy = 0; gy < GRID_HEIGHT; gy++)
        {
            const EnvironmentCell_t *cell = &g_environment[gx][gy];
            /* Deliberately a higher bar than "any damage at all" - the
             * earthquake leaves low-level road_damage almost everywhere,
             * which would swamp the map with noise. Only genuinely
             * actionable conditions are sent: actually blocked, an
             * active hazard flag, or damage severe enough to matter
             * (>=50, same rough scale as the 0-255 damage fields
             * elsewhere in this codebase using a meaningful-threshold
             * convention). */
            if(!cell->road_blocked && cell->hazard_flags == 0 &&
               cell->road_damage < 50 && cell->infrastructure_damage < 50)
            {
                continue;
            }
            printf("%s{\"x\":%u,\"y\":%u,\"blocked\":%u,\"hazard_flags\":%u,"
                   "\"road_damage\":%u,\"infra_damage\":%u}",
                   first_h ? "" : ",",
                   gx, gy, cell->road_blocked, cell->hazard_flags,
                   cell->road_damage, cell->infrastructure_damage);
            first_h = 0;
        }
    }
    printf("]}\n");
    fflush(stdout);
}

void print_world_state(void) //debug
{
    printf("Environment | Seed: %lu | Roads: %u | Navigable: %u | Buildings: %u | Damaged Buildings: %u | Blocked Roads: %u | Damaged Infrastructure: %u | Bridges: %u | Nav Nodes: %u | Nav Edges: %u | Validated: %s\n",
           (unsigned long)g_environment_summary.seed,
           g_environment_summary.road_cell_count,
           g_environment_summary.navigable_cell_count,
           g_environment_summary.building_count,
           g_environment_summary.damaged_building_count,
           g_environment_summary.blocked_road_count,
           g_environment_summary.damaged_infrastructure_count,
           g_environment_summary.bridge_count,
           g_environment_summary.nav_node_count,
           g_environment_summary.nav_edge_count,
           g_environment_summary.validation_passed ? "YES" : "NO");

    if(g_environment_summary.earthquake_applied)
    {
        printf("Earthquake | Epicenter: (%d,%d) | Intensity: %u\n",
               g_environment_summary.earthquake_epicenter_x,
               g_environment_summary.earthquake_epicenter_y,
               g_environment_summary.earthquake_intensity);
    }

    printf("Hazard Propagation | Fire Spread: %lu | Gas Explosions: %lu | Aftershock Collapses: %lu | Road Cost Increases: %lu\n",
           (unsigned long)g_environment_summary.fire_spread_count,
           (unsigned long)g_environment_summary.gas_explosion_count,
           (unsigned long)g_environment_summary.aftershock_collapse_count,
           (unsigned long)g_environment_summary.road_degradation_count);

    for(uint8_t district = 0; district < NUM_DISTRICT_TYPES; district++)
    {
        printf("District %-12s | Cells: %u\n",
               environment_district_name(district),
               g_environment_summary.district_count[district]);
    }

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        printf("Team %d | Tier: %d | Status: %s%s | Stamina: %.3f | Equipment: %d\n",
               i,
               g_teams[i].fitness_tier,
               team_status_to_string(g_teams[i].status),
               g_teams[i].degraded ? " (DEGRADED)" : "",
               g_teams[i].stamina,
               g_teams[i].equipment_level);
    }

    for(int i = 0; i < NUM_INCIDENTS; i++)
    {
        if(!g_incidents[i].slot_occupied)
            continue;

        printf("Incident ID: %lu | Slot: %d | Type: %d | Status: %s | Assigned: %d | Spawned By: %d | Loc: (%d,%d)\n",
               (unsigned long)g_incidents[i].incident_id,
               i,
               g_incidents[i].incident_type,
               incident_status_to_string(g_incidents[i].status),
               g_incidents[i].assigned_team,
               g_incidents[i].spawned_by,
               g_incidents[i].loc_x,
               g_incidents[i].loc_y);
    }

    for(int i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(!g_system_tasks[i].slot_occupied)
            continue;

        printf("System Task ID: %lu | Slot: %d | Type: %d | Status: %s | Assigned: %d | Spawned By: %d | Loc: (%d,%d)\n",
               (unsigned long)g_system_tasks[i].incident_id,
               i,
               g_system_tasks[i].incident_type,
               incident_status_to_string(g_system_tasks[i].status),
               g_system_tasks[i].assigned_team,
               g_system_tasks[i].spawned_by,
               g_system_tasks[i].loc_x,
               g_system_tasks[i].loc_y);
    }


    for(int i = 0; i < g_num_safe_zones; i++)
    {
        printf(
            "Safe Zone %d | Evacuees: %d/%d | Loc: (%d,%d)\n",
            i,
            g_safezones[i].evacuee_count,
            g_safezones[i].capacity,
            g_safezones[i].loc_x,
            g_safezones[i].loc_y
        );
    }
}

void initialize_test_scenario(void)         //debug
{
    g_world_seed = 1234;
    g_current_tick = 0;
    random_seed_all(g_world_seed);
    generate_environment(g_world_seed);
    initialize_incidents();

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        g_teams[i].team_id = i;
        /* Wrap: only NUM_TEAM_SPECIALIZATIONS (5) exist but NUM_TEAMS is
         * 10. A plain `= i` gave teams 5-9 out-of-range specializations,
         * so the fitness_tier switch below never matched them either. */
        g_teams[i].specialization = (TeamSpecialization_t)(i % NUM_TEAM_SPECIALIZATIONS);
        g_teams[i].status = STATUS_RESTING;
        g_teams[i].assigned_task_pool = TASK_POOL_NONE;
        g_teams[i].assigned_task_slot = NO_ASSIGNMENT;
        g_teams[i].incident_queue_size = 0;
        g_teams[i].incident_queue_index = 0;
        for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
        {
            g_teams[i].incident_queue_slot[q] = NO_ASSIGNMENT;
            g_teams[i].incident_queue_id[q] = 0;
        }
        g_teams[i].loc_x = 2;
        g_teams[i].loc_y = 2;
        g_teams[i].stamina = 100;
        g_teams[i].hours_worked = 0;
        g_teams[i].ticks_worked_this_period = 0;
        g_teams[i].equipment_flags = 0;
        g_teams[i].last_heartbeat_tick = 0;
        g_teams[i].resting_zone_id = 0;
        g_teams[i].heading_to_rest = 0;
        g_teams[i].equipment_level = EQ_MAX_LEVEL;

        switch(g_teams[i].specialization)
        {
            case SPEC_EXTRACTION:
                g_teams[i].fitness_tier = ARDUOUS;
                g_teams[i].stamina = ARDUOUS_MIN_STAMINA;
                break;

            case SPEC_SEARCH:
            case SPEC_PERIMETER:
                g_teams[i].fitness_tier = MODERATE;
                g_teams[i].stamina = MODERATE_MIN_STAMINA;
                break;

            case SPEC_MEDICAL:
            case SPEC_SUPPORT:
                g_teams[i].fitness_tier = LIGHT;
                g_teams[i].stamina = LIGHT_MIN_STAMINA;
                break;
        }
    }

    g_teams[0].status = STATUS_ON_SCENE;
    g_teams[0].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[0].assigned_task_slot = 0;
    g_teams[0].equipment_level = 1;

    g_incidents[0].incident_id = allocate_incident_id();
    g_incidents[0].slot_occupied = 1;
    g_incidents[0].status = INC_IN_PROGRESS;
    g_incidents[0].assigned_team = 0;
    g_incidents[0].priority = PRIORITY_HIGH;
    g_incidents[0].req_capability = SPEC_SEARCH;
    g_incidents[0].loc_x = 2;
    g_incidents[0].loc_y = 2;
    g_incidents[0].work_remaining = 100;
    g_incidents[0].incident_type = INCIDENT_UNSEARCHED_SECTOR;

    g_num_safe_zones = 1;
    g_safezones[0].safezone_id = 0;
    g_safezones[0].loc_x = 2;
    g_safezones[0].loc_y = 2;
    g_safezones[0].capacity = 20;
    g_safezones[0].evacuee_count = 16;
    environment_register_safe_zone(0, g_safezones[0].loc_x, g_safezones[0].loc_y);

    for(int i = 0; i < MAX_SAFE_ZONES; i++)
    {
        g_safezone_evacuation_active[i] = 0;
    }

    initialize_evacuation_gateways(1234);
    environment_apply_earthquake(g_world_seed);
    initialize_grid();
    initialize_evacuation_teams();
}

int debug_run_priority1_repro(void)         //debug
{
    initialize_test_scenario();

    /* initialize_test_scenario() parks team 0 ON_SCENE at the same
     * cell as its incident (2,2) - fine for other debug uses, but this
     * harness needs a team actually traveling so it can have its path
     * cut mid-route. Retarget the incident somewhere far across the
     * generated map and put the team EN_ROUTE to it. */
    g_incidents[0].loc_x = 25;
    g_incidents[0].loc_y = 25;
    g_teams[0].status = STATUS_EN_ROUTE;
    g_teams[0].heading_to_rest = 0;

    TeamState_t *team = &g_teams[0];
    IncidentState_t *task = &g_incidents[0];

    int nx, ny;
    int initial_cost = find_next_path_step(team->loc_x, team->loc_y, task->loc_x, task->loc_y, 0, &nx, &ny);
    printf("[REPRO P1] sanity: path (2,2)->(25,25) cost=%d before any wall-off\n", initial_cost);
    if(initial_cost <= 0)
    {
        printf("[REPRO P1] INVALID: no path even before wall-off on this map - can't run repro.\n");
        return 2;
    }

    /* Let the team actually travel for a few real ticks on the still-
     * open path first, so this matches "a road gets damaged while a
     * team's already traveling" rather than "destination was
     * unreachable from the moment it was assigned". */
    g_current_tick = 1;
    for(int t = 0; t < 5; t++)
    {
        move_team(team);
        g_current_tick++;
    }
    printf("[REPRO P1] after 5 real ticks of travel: team now at (%d,%d) (started at (2,2))\n",
           team->loc_x, team->loc_y);
    if(team->loc_x == 2 && team->loc_y == 2)
    {
        printf("[REPRO P1] INVALID: team never left the start cell in 5 ticks.\n");
        return 4;
    }

    /* Sever the path out from under the already-traveling team: wall
     * off a full ring around the destination so no route through it
     * survives, regardless of where the team currently stands. */
    for(int x = 20; x <= 30; x++)
    {
        for(int y = 20; y <= 30; y++)
        {
            if(x == 20 || x == 30 || y == 20 || y == 30)
                g_environment[x][y].navigable = 0;
        }
    }

    int post_wall_cost = find_next_path_step(team->loc_x, team->loc_y, task->loc_x, task->loc_y, 0, &nx, &ny);
    printf("[REPRO P1] after mid-route wall-off: find_next_path_step returns %d (<=0 = unreachable, as intended)\n",
           post_wall_cost);
    if(post_wall_cost > 0)
    {
        printf("[REPRO P1] INVALID: destination still reachable after wall-off - widen the ring.\n");
        return 3;
    }

    uint8_t start_pool = team->assigned_task_pool;
    uint8_t start_slot = team->assigned_task_slot;
    int start_x = team->loc_x, start_y = team->loc_y;
    uint8_t start_status = team->status;

    extern unsigned long g_probe_no_path_hits;
    extern uint32_t g_diag_safezone_fallback_count;

    int recovered_at_tick = -1;
    for(uint32_t t = 1; t <= 2000; t++)
    {
        g_current_tick++;
        move_team(team);

        int moved = (team->loc_x != start_x || team->loc_y != start_y);
        int pool_changed = (team->assigned_task_pool != start_pool || team->assigned_task_slot != start_slot);
        int status_changed = (team->status != start_status);

        if(moved || pool_changed || status_changed)
        {
            recovered_at_tick = (int)t;
            break;
        }
    }

    printf("[REPRO P1] g_probe_no_path_hits=%lu g_diag_safezone_fallback_count=%u\n",
           g_probe_no_path_hits, g_diag_safezone_fallback_count);
    printf("[REPRO P1] final: status=%u (was %u) pool/slot=%u/%u (was %u/%u) loc=(%d,%d) (was (%d,%d))\n",
           team->status, start_status, team->assigned_task_pool, team->assigned_task_slot,
           start_pool, start_slot, team->loc_x, team->loc_y, start_x, start_y);

    if(recovered_at_tick > 0)
    {
        printf("[REPRO P1] VERDICT: FIXED - un-stuck at tick %d (within 2000-tick cap).\n", recovered_at_tick);
        return 0;
    }

    printf("[REPRO P1] VERDICT: BUG REPRODUCED - frozen for the full 2000-tick budget.\n");
    return 10;
}

/* Any single cell's travel_cost is a uint8_t (max 255) and A* takes at
 * most one cell's cost worth of ticks to clear a single step. 600 ticks
 * frozen at the same (x,y) while EN_ROUTE is therefore not "an
 * unusually expensive step" under any cost value this codebase can
 * produce - it can only mean the team stopped making progress, which
 * is exactly the pre-fix move_team() task-branch signature. Padding
 * well above the theoretical max (255) rather than cutting it close
 * avoids flagging a legitimately slow-but-moving team as frozen. */
#define P1_SWEEP_STUCK_TICKS_THRESHOLD 10
#define P1_SWEEP_MAX_REPORTED_STREAKS 64

typedef struct
{
    uint32_t seed;
    uint8_t team_id;
    uint32_t start_tick;
    uint32_t streak_len;
} FrozenStreak_t;

int debug_run_priority1_sweep(uint32_t first_seed, uint32_t seed_count, uint32_t ticks_per_seed, int fault_profile_int)
{
    FaultProfile_t fault_profile = (FaultProfile_t)fault_profile_int;

    extern unsigned long g_probe_no_path_hits;
    extern uint32_t g_diag_safezone_fallback_count;
    extern unsigned long g_diag_carve_repair_count;
    extern unsigned long g_diag_carve_repair_failed;
    extern unsigned long g_diag_incidents_quarantined;
    extern uint32_t g_diag_sweep_cross_district_cells;
    extern unsigned long g_diag_gateway_degenerate_worlds;
    extern unsigned long g_diag_gateway_repaired;
    extern unsigned long g_diag_safezone_maxattempts_fallback;
    extern unsigned long g_diag_team_score_ties;

    uint32_t per_team_streak[NUM_TEAMS];
    int16_t per_team_last_x[NUM_TEAMS];
    int16_t per_team_last_y[NUM_TEAMS];
    uint32_t per_team_streak_start[NUM_TEAMS];

    FrozenStreak_t flagged[P1_SWEEP_MAX_REPORTED_STREAKS];
    uint32_t flagged_count = 0;
    uint32_t max_streak_seen = 0;

    unsigned long probe_hits_before_sweep = g_probe_no_path_hits;
    uint32_t fallback_before_sweep = g_diag_safezone_fallback_count;
    unsigned long carve_repair_before_sweep = g_diag_carve_repair_count;
    unsigned long carve_failed_before_sweep = g_diag_carve_repair_failed;
    unsigned long quarantined_before_sweep = g_diag_incidents_quarantined;
    uint32_t cross_district_before_sweep = g_diag_sweep_cross_district_cells;
    unsigned long gateway_degenerate_before_sweep = g_diag_gateway_degenerate_worlds;
    unsigned long gateway_repaired_before_sweep = g_diag_gateway_repaired;
    unsigned long safezone_fallback_before_sweep = g_diag_safezone_maxattempts_fallback;
    unsigned long team_score_ties_before_sweep = g_diag_team_score_ties;

    printf("[SWEEP P1] starting: seeds %u..%u (%u seeds), %u ticks/seed, fault_profile=%d\n",
           first_seed, first_seed + seed_count - 1, seed_count, ticks_per_seed, (int)fault_profile);
    printf("[SWEEP P1] stuck threshold: %u consecutive ticks EN_ROUTE at the same (x,y)\n",
           P1_SWEEP_STUCK_TICKS_THRESHOLD);

    /* Counter loop, not `seed < first_seed + seed_count`: that sum can
     * overflow uint32_t for large inputs and run zero (or wrong) seeds.
     * Same shape as laptop/main.c's run loop. */
    for(uint32_t seed_index = 0; seed_index < seed_count; seed_index++)
    {
        uint32_t seed = first_seed + seed_index;

        sim_start_seed(seed, fault_profile);

        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            per_team_streak[t] = 0;
            per_team_last_x[t] = -1;
            per_team_last_y[t] = -1;
            per_team_streak_start[t] = 0;
        }

        for(uint32_t tick = 0; tick < ticks_per_seed; tick++)
        {
            sim_tick();

            for(uint8_t t = 0; t < NUM_TEAMS; t++)
            {
                const TeamState_t *team = &g_teams[t];

                if(team->status != STATUS_EN_ROUTE)
                {
                    per_team_streak[t] = 0;
                    per_team_last_x[t] = -1;
                    per_team_last_y[t] = -1;
                    continue;
                }

                if(team->loc_x == per_team_last_x[t] && team->loc_y == per_team_last_y[t])
                {
                    per_team_streak[t]++;
                }
                else
                {
                    per_team_last_x[t] = team->loc_x;
                    per_team_last_y[t] = team->loc_y;
                    per_team_streak[t] = 1;
                    per_team_streak_start[t] = tick;
                }

                if(per_team_streak[t] > max_streak_seen)
                    max_streak_seen = per_team_streak[t];

                if(per_team_streak[t] == P1_SWEEP_STUCK_TICKS_THRESHOLD)
                {
                    printf("[SWEEP P1] FROZEN: seed=%u team=%u stuck at (%d,%d) since tick=%u, "
                           "still EN_ROUTE with no movement at tick=%u (%u ticks and counting)\n",
                           seed, t, team->loc_x, team->loc_y, per_team_streak_start[t], tick,
                           per_team_streak[t]);

                    if(flagged_count < P1_SWEEP_MAX_REPORTED_STREAKS)
                    {
                        flagged[flagged_count].seed = seed;
                        flagged[flagged_count].team_id = t;
                        flagged[flagged_count].start_tick = per_team_streak_start[t];
                        flagged[flagged_count].streak_len = per_team_streak[t];
                        flagged_count++;
                    }
                }
            }
        }

        sim_end_seed();

        if((seed_index + 1) % 10 == 0 || seed_index + 1 == seed_count)
        {
            printf("[SWEEP P1] progress: %u/%u seeds done, %u frozen streaks so far, "
                   "max streak so far=%u, cumulative no_path_hits=%lu, safezone_fallback=%u\n",
                   seed_index + 1, seed_count, flagged_count, max_streak_seen,
                   g_probe_no_path_hits, g_diag_safezone_fallback_count);
        }
    }

    unsigned long probe_hits_in_sweep = g_probe_no_path_hits - probe_hits_before_sweep;
    uint32_t fallback_in_sweep = g_diag_safezone_fallback_count - fallback_before_sweep;

    printf("\n[SWEEP P1] === SUMMARY ===\n");
    printf("[SWEEP P1] seeds run: %u (seed %u..%u), ticks/seed: %u, total team-ticks scanned: %llu\n",
           seed_count, first_seed, first_seed + seed_count - 1, ticks_per_seed,
           (unsigned long long)seed_count * ticks_per_seed * NUM_TEAMS);
    printf("[SWEEP P1] task-branch no-path hits this sweep:  %lu (cumulative process total: %lu)\n",
           probe_hits_in_sweep, g_probe_no_path_hits);
    printf("[SWEEP P1] safezone fallback count this sweep:   %u (cumulative process total: %u)\n",
           fallback_in_sweep, g_diag_safezone_fallback_count);
    printf("[SWEEP P1] carve_repair_to_nearest_road fires this sweep: %lu (of which carve failed: %lu)\n",
           g_diag_carve_repair_count - carve_repair_before_sweep,
           g_diag_carve_repair_failed - carve_failed_before_sweep);
    printf("[SWEEP P1] incidents quarantined this sweep (unreachable_streak >= %u): %lu (cumulative process total: %lu)\n",
           INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD,
           g_diag_incidents_quarantined - quarantined_before_sweep,
           g_diag_incidents_quarantined);
    printf("[SWEEP P1] passive-sweep cells straddling a district boundary (credited to the cell's own district since the M37 fix) this sweep: %u (cumulative process total: %u)\n",
           g_diag_sweep_cross_district_cells - cross_district_before_sweep,
           g_diag_sweep_cross_district_cells);
    printf("[SWEEP P1] worlds with non-distinct evacuation gateways this sweep: %lu (expected 0; cumulative process total: %lu)\n",
           g_diag_gateway_degenerate_worlds - gateway_degenerate_before_sweep,
           g_diag_gateway_degenerate_worlds);
    printf("[SWEEP P1] evacuation gateways repaired (not road-connected when chosen) this sweep: %lu (cumulative process total: %lu)\n",
           g_diag_gateway_repaired - gateway_repaired_before_sweep,
           g_diag_gateway_repaired);
    printf("[SWEEP P1] safe zones placed via MAX_ATTEMPTS fallback this sweep: %lu (expected 0; cumulative process total: %lu)\n",
           g_diag_safezone_maxattempts_fallback - safezone_fallback_before_sweep,
           g_diag_safezone_maxattempts_fallback);
    printf("[SWEEP P1] select_best_team score ties this sweep: %lu (cumulative process total: %lu)\n",
           g_diag_team_score_ties - team_score_ties_before_sweep,
           g_diag_team_score_ties);
    printf("[SWEEP P1] longest EN_ROUTE-same-location streak observed anywhere: %u ticks "
           "(flag threshold: %u)\n", max_streak_seen, P1_SWEEP_STUCK_TICKS_THRESHOLD);
    printf("[SWEEP P1] frozen streaks flagged (>= threshold): %u\n", flagged_count);

    for(uint32_t i = 0; i < flagged_count; i++)
    {
        printf("[SWEEP P1]   #%u: seed=%u team=%u first stuck at tick=%u, streak>=%u ticks\n",
               i + 1, flagged[i].seed, flagged[i].team_id, flagged[i].start_tick, flagged[i].streak_len);
    }
    if(flagged_count == P1_SWEEP_MAX_REPORTED_STREAKS)
        printf("[SWEEP P1]   (further streaks not individually listed - cap of %u reached)\n",
               P1_SWEEP_MAX_REPORTED_STREAKS);

    if(flagged_count == 0)
    {
        printf("[SWEEP P1] VERDICT: PASS - no team was ever stuck EN_ROUTE at one location for "
               "%u+ ticks across %u seeds x %u ticks.\n", P1_SWEEP_STUCK_TICKS_THRESHOLD, seed_count, ticks_per_seed);
        return 0;
    }

    printf("[SWEEP P1] VERDICT: FAIL - %u frozen streak(s) detected. The task-branch no-path "
           "fallback is not preventing every freeze.\n", flagged_count);
    return (int)(flagged_count > 255 ? 255 : flagged_count);
}