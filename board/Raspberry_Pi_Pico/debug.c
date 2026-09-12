/*
 * Board implementation of the debug.h interface (Raspberry Pi Pico,
 * µT-Kernel 3.0 BSP (v1)).
 *
 * Mirrors laptop/debug.c function-for-function so core/
 * never needs to know which platform it's linked against. Debug
 * output goes over USART1 (115200-8-N-1) through the ST-LINK
 * micro-USB connector, via the BSP's T-Monitor-compatible API
 * (tm_printf / tm_putstring). See vendor/README.md for how that was
 * verified against the real BSP headers.
 *
 * IMPORTANT: tm_printf does NOT support floating point conversion
 * specifiers - verified directly in this BSP's own source, not cited
 * from a manual: board/Raspberry_Pi_Pico/vendor/mtk3_bsp/lib/libtm/
 * libtm_printf.c's own top-of-file comment states "Unsupported
 * specifiers: floating point, long long and others" outright, and its
 * format-specifier switch has no 'f' case. Same conclusion the DK's
 * copy of this file reached (there, cited from the BSP2 STM32Cube
 * manual instead, since that BSP's tm_printf source wasn't checked
 * directly at the time) - both BSPs' tm_printf turn out to lack float
 * support, confirmed by two different means rather than assumed to
 * match just because the function name is the same.
 * Every float this file needs to print (stamina, CBBA scores) goes
 * through fmt_fixed3() first, which converts to a fixed-point decimal
 * string using only integer arithmetic, then gets printed with %s.
 * Do not add a %f to any tm_printf call in this file — it will not
 * do what you expect, and there's no compiler warning that catches
 * it since tm_printf is a variadic function like printf().
 */

#include <stdint.h>
#include <stdarg.h>

#include <tm/tmonitor.h>

#include "debug.h"
#include "mission_state.h"
#include "incident.h"
#include "mission_state_utils.h"
#include "grid.h"
#include "environment.h"
#include "team.h"
#include "safezone.h"

/*
 * Fixed-point float formatter for tm_printf, which has no %f support.
 * Formats `value` with exactly `decimals` digits after the point into
 * `buf` (caller-supplied, must be at least 16 bytes). Truncates
 * (does not round) toward zero on the last digit, same as printf's
 * %f would for the values this file prints (stamina 0-100, CBBA
 * scores) where sub-thousandth precision doesn't matter for a debug
 * log line.
 */
static void fmt_fixed3(char *buf, uint32_t bufsz, float value, int decimals)
{
    /* Bug fix: this used to return without touching buf at all when
       bufsz < 2, leaving it uninitialized/non-terminated for any future
       caller that passes a smaller buffer than today's callers do (all
       currently pass 16). Zero-terminate the one byte that is actually
       safe to write instead of leaving buf in an undefined state. */
    if(bufsz == 0)
        return;
    if(bufsz < 2)
    {
        buf[0] = '\0';
        return;
    }

    uint32_t pos = 0;
    int32_t sign = 1;

    if(value < 0.0f)
    {
        sign = -1;
        value = -value;
    }

    int32_t scale = 1;
    for(int i = 0; i < decimals; i++)
        scale *= 10;

    /* Truncate to integer at the target scale using only integer ops
     * once we're past the initial float multiply — avoids relying on
     * any libm rounding function that might not be linked in. */
    int32_t scaled = (int32_t)(value * (float)scale);
    int32_t whole = scaled / scale;
    int32_t frac = scaled % scale;

    if(sign < 0 && pos + 1 < bufsz)
        buf[pos++] = '-';

    /* Whole part */
    char tmp[12];
    int tlen = 0;
    if(whole == 0)
    {
        tmp[tlen++] = '0';
    }
    else
    {
        while(whole > 0 && tlen < (int)sizeof(tmp))
        {
            tmp[tlen++] = (char)('0' + (whole % 10));
            whole /= 10;
        }
    }
    while(tlen > 0 && pos + 1 < bufsz)
        buf[pos++] = tmp[--tlen];

    if(decimals > 0 && pos + 1 < bufsz)
    {
        buf[pos++] = '.';
        /* Fractional digits, most significant first, zero-padded. */
        for(int i = decimals - 1; i >= 0 && pos + 1 < bufsz; i--)
        {
            int32_t digit_scale = 1;
            for(int j = 0; j < i; j++)
                digit_scale *= 10;
            buf[pos++] = (char)('0' + ((frac / digit_scale) % 10));
        }
    }

    buf[pos] = '\0';
}

void debug_trace(const char *fmt, ...)
{
    (void)fmt;
    /* Trace call sites (allocator.c, simulation.c) pass printf-style
     * varargs including a float, which tm_printf can't accept
     * directly via a forwarded va_list without a custom
     * mini-formatter. Given trace output is diagnostic-only and
     * gated behind g_simulation_trace_enabled (off by default), it's
     * left silent here rather than building a full va_list-based
     * printf-workalike whose only job is to intercept and reformat
     * %f. If on-device trace turns out to be needed, the two call
     * sites in core/ are few enough (allocator.c:1, simulation.c:2)
     * to give them dedicated typed board hooks instead of routing
     * through a generic varargs sink.
     */
}

void print_world_state(void)
{
    char stamina_buf[16];

    tm_printf((UB*)"Environment | Seed: %lu | Roads: %u | Navigable: %u | Buildings: %u | Damaged: %u | Blocked Roads: %u | Validated: %s\n",
        (unsigned long)g_environment_summary.seed,
        g_environment_summary.road_cell_count,
        g_environment_summary.navigable_cell_count,
        g_environment_summary.building_count,
        g_environment_summary.damaged_building_count,
        g_environment_summary.blocked_road_count,
        g_environment_summary.validation_passed ? (UB*)"YES" : (UB*)"NO");

    if(g_environment_summary.earthquake_applied)
    {
        tm_printf((UB*)"Earthquake | Epicenter: (%d,%d) | Intensity: %u\n",
            g_environment_summary.earthquake_epicenter_x,
            g_environment_summary.earthquake_epicenter_y,
            g_environment_summary.earthquake_intensity);
    }

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        fmt_fixed3(stamina_buf, sizeof(stamina_buf), g_teams[i].stamina, 3);
        tm_printf((UB*)"Team %d | Tier: %d | Status: %s%s | Stamina: %s | Equipment: %d\n",
            i,
            g_teams[i].fitness_tier,
            (UB*)team_status_to_string(g_teams[i].status),
            g_teams[i].degraded ? (UB*)" (DEGRADED)" : (UB*)"",
            (UB*)stamina_buf,
            g_teams[i].equipment_level);
    }

    /* Per-incident, per-system-task, and per-safe-zone dumps
     * (present in laptop/debug.c) are intentionally left
     * out of the on-device summary: at up to ~50+ live incidents,
     * dumping one line each over a 115200bps UART on every call is
     * a meaningful amount of airtime for output that's rarely what
     * you want mid-run on hardware. If you need the full per-object
     * dump for on-device debugging, it's a straightforward addition
     * here — same loop shape as the laptop version, gated behind
     * something like a DEBUG_FULL_DUMP build flag so it stays opt-in.
     */
}

void initialize_test_scenario(void)
{
    /* Intentionally not ported: desktop test-harness helper only.
     * If board-side scripted scenarios are ever needed, give this a
     * distinct board-specific entry point rather than reusing this
     * one, since the desktop version's assumptions (fixed seed,
     * fixed scenario shape) are test-tooling concerns, not firmware
     * concerns. */
}

void debug_emit_live_frame(uint32_t tick)
{
    char stamina_buf[16];

    /* TODO(usb): this is the board's actual job, not a stub-forever
     * function. The laptop's LIVEFRAME is a JSON text line over
     * stdout for the Flask dashboard's SSE stream; the board
     * equivalent should be a compact binary telemetry frame over
     * USB carrying the same per-tick team/incident/safezone state.
     *
     * Below is a text-UART stopgap, not that eventual design: just
     * enough per-tick output (tick + compact per-team state) to see
     * the sim is alive on real hardware right now, over the existing
     * 115200bps debug UART via tm_printf. Deliberately narrower than
     * laptop/debug.c's own LIVEFRAME (no per-incident array, no
     * fault_mask/degraded/hours worked/dest fields/diagnostic-counter
     * fields) - see this file's own comment above print_world_state()
     * on why dumping that much every tick over this UART is real airtime;
     * the same reasoning applies here. A fuller per-object dump, if
     * ever wanted on-device, is that same comment's proposed
     * DEBUG_FULL_DUMP-gated addition, not something to build in here.
     */
    tm_printf((UB*)"LIVEFRAME tick=%lu", (unsigned long)tick);

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        fmt_fixed3(stamina_buf, sizeof(stamina_buf), g_teams[i].stamina, 3);
        tm_printf((UB*)" team%d={id=%d,x=%d,y=%d,stamina=%s,status=%s,pool=%d,slot=%d}",
            i,
            g_teams[i].team_id,
            g_teams[i].loc_x,
            g_teams[i].loc_y,
            (UB*)stamina_buf,
            (UB*)team_status_to_string(g_teams[i].status),
            g_teams[i].assigned_task_pool,
            g_teams[i].assigned_task_slot);
    }

    tm_printf((UB*)"\n");
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
    (void)tick; (void)load; (void)probability; (void)roll; (void)spawned;
    (void)available_teams; (void)en_route_teams; (void)on_scene_teams;
    (void)resting_teams; (void)open_incidents; (void)assigned_incidents;
    (void)in_progress_incidents; (void)active_system_tasks;
    (void)degraded_cells; (void)next_evaluation_tick;
    /* Low-priority, verbose incident-generator debug output — no
     * floats involved so this one's a plain tm_printf port whenever
     * it's wanted; left silent for now since nothing in core/ needs
     * it to run correctly. */
}

void debug_print_generator_selection(const uint32_t *weights,
                                     uint32_t weight_count,
                                     uint32_t total_weight,
                                     uint32_t type_roll,
                                     uint8_t selected_type)
{
    (void)weights; (void)weight_count; (void)total_weight;
    (void)type_roll; (void)selected_type;
    /* Same as above. */
}
