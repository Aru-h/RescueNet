/*
 * sx1262_setup_selftest.c - runs core/sx1262_setup.h (the clock/calibration/
 * PA bring-up both boards' HALs share) against a simulated SX1262 chip.
 * Host-only, no hardware. Run via `make check-sx1262-setup-selftest`.
 *
 * The fake chip models the one thing that matters here: whether its 32 MHz
 * oscillator can start. A TCXO module can't until SetDIO3AsTCXOCtrl has
 * powered the TCXO at a voltage it accepts (and raises XOSC_START_ERR at
 * power-on, as the datasheet says a TCXO module does); a crystal module
 * always starts.
 *
 * What this proves, per scenario:
 *   A. TCXO module: the TCXO command is sent once, at 1.8 V, BEFORE the
 *      full calibration; image calibration comes after it, with the 863-870
 *      MHz values; calibrations run in STDBY_RC; the errors end up clean.
 *   B. Crystal module: the TCXO command is NEVER sent (sending it would
 *      break a crystal-based chip), and no full calibration is forced.
 *   C. TCXO module that needs a different voltage: setup fails cleanly with
 *      the oscillator error and does not try other voltages (a wrong guess
 *      on a TCXO's supply could damage it).
 *   D. Dead SPI bus: fails at the very first step, reported as such.
 *   E. PA config bytes are the datasheet's SX1262 values.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sx126x.h"
#include "sx126x_hal.h"
#include "sx1262_setup.h"

#define OP_SET_STANDBY   0x80u
#define OP_CLR_ERRORS    0x07u
#define OP_GET_ERRORS    0x17u
#define OP_CALIBRATE     0x89u
#define OP_CAL_IMAGE     0x98u
#define OP_SET_PA_CFG    0x95u
#define OP_SET_TCXO      0x97u

typedef struct
{
    /* scenario */
    int has_tcxo;
    uint8_t tcxo_ok_voltage;
    int spi_dead;

    /* state */
    int mode;              /* 2 = STDBY_RC, 3 = STDBY_XOSC */
    int tcxo_active;
    uint16_t errors;

    /* record */
    uint8_t log[64];
    int log_n;
    int tcxo_count;
    uint8_t tcxo_voltage;
    uint32_t tcxo_delay;
    int cal_count;
    uint8_t cal_param;
    int img_count;
    uint8_t img1;
    uint8_t img2;
    int pa_count;
    uint8_t pa[4];
    int pos_tcxo;
    int pos_cal;
    int pos_img;
    int cal_outside_rc;
} chip_t;

static chip_t g_chip;

static void chip_init(int has_tcxo, uint8_t tcxo_ok_voltage, int spi_dead)
{
    memset(&g_chip, 0, sizeof(g_chip));
    g_chip.has_tcxo = has_tcxo;
    g_chip.tcxo_ok_voltage = tcxo_ok_voltage;
    g_chip.spi_dead = spi_dead;
    g_chip.mode = 2;
    g_chip.pos_tcxo = g_chip.pos_cal = g_chip.pos_img = -1;
    /* Datasheet 13.3.6: a TCXO module flags XOSC_START_ERR at power-on. */
    g_chip.errors = has_tcxo ? (uint16_t)SX126X_ERRORS_XOSC_START : 0u;
}

static void chip_log(uint8_t op)
{
    if (g_chip.log_n < (int)sizeof(g_chip.log))
    {
        g_chip.log[g_chip.log_n++] = op;
    }
}

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command,
                                      const uint16_t command_length, const uint8_t *data,
                                      const uint16_t data_length)
{
    (void)context; (void)data; (void)data_length;

    if (g_chip.spi_dead)
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    assert(command_length > 0u);
    chip_log(command[0]);

    switch (command[0])
    {
    case OP_SET_STANDBY:
        if (command[1] == 0x01u)   /* XOSC: try to start the 32 MHz clock */
        {
            int ok = (!g_chip.has_tcxo) || g_chip.tcxo_active;
            if (ok)
            {
                g_chip.mode = 3;
            }
            else
            {
                g_chip.errors |= (uint16_t)SX126X_ERRORS_XOSC_START;
                g_chip.mode = 2;
            }
        }
        else
        {
            g_chip.mode = 2;
        }
        break;
    case OP_CLR_ERRORS:
        g_chip.errors = 0u;
        break;
    case OP_SET_TCXO:
        g_chip.tcxo_count++;
        g_chip.tcxo_voltage = command[1];
        g_chip.tcxo_delay = ((uint32_t)command[2] << 16) | ((uint32_t)command[3] << 8) | command[4];
        g_chip.pos_tcxo = g_chip.log_n - 1;
        if (g_chip.has_tcxo && command[1] == g_chip.tcxo_ok_voltage)
        {
            g_chip.tcxo_active = 1;
        }
        break;
    case OP_CALIBRATE:
        g_chip.cal_count++;
        g_chip.cal_param = command[1];
        g_chip.pos_cal = g_chip.log_n - 1;
        if (g_chip.mode != 2) g_chip.cal_outside_rc++;
        break;
    case OP_CAL_IMAGE:
        g_chip.img_count++;
        g_chip.img1 = command[1];
        g_chip.img2 = command[2];
        g_chip.pos_img = g_chip.log_n - 1;
        if (g_chip.mode != 2) g_chip.cal_outside_rc++;
        break;
    case OP_SET_PA_CFG:
        g_chip.pa_count++;
        memcpy(g_chip.pa, &command[1], 4);
        break;
    default:
        break;
    }
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
                                     const uint16_t command_length, uint8_t *data,
                                     const uint16_t data_length)
{
    (void)context;

    if (g_chip.spi_dead)
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    assert(command_length > 0u);
    chip_log(command[0]);

    memset(data, 0, data_length);
    if (command[0] == OP_GET_ERRORS && data_length >= 2u)
    {
        data[0] = (uint8_t)(g_chip.errors >> 8);
        data[1] = (uint8_t)(g_chip.errors & 0xFFu);
    }
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context) { (void)context; return SX126X_HAL_STATUS_OK; }
sx126x_hal_status_t sx126x_hal_wakeup(const void *context) { (void)context; return SX126X_HAL_STATUS_OK; }

static int g_checks;
static int g_failures;

#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: %s\n", msg); } } while (0)

int main(void)
{
    sx1262_setup_report_t rep;
    int rc;

    /* ---- A: TCXO module (needs 1.8 V, the default) ---- */
    chip_init(1, SX126X_TCXO_CTRL_1_8V, 0);
    rc = sx1262_setup_clock_and_calibration(&rep);
    CHECK(rc == 0 && rep.result == SX1262_SETUP_OK, "A: setup succeeds on a TCXO module");
    CHECK(rep.tcxo_enabled == 1, "A: reports that it switched the TCXO on");
    CHECK(g_chip.tcxo_count == 1, "A: TCXO command sent exactly once");
    CHECK(g_chip.tcxo_voltage == SX126X_TCXO_CTRL_1_8V, "A: TCXO voltage is 1.8 V");
    CHECK(g_chip.tcxo_delay == SX1262_SETUP_TCXO_DELAY_STEPS, "A: TCXO delay bytes are 320 steps (5 ms)");
    CHECK(g_chip.cal_count == 1 && g_chip.cal_param == 0x7Fu, "A: one full calibration (0x7F)");
    CHECK(g_chip.pos_tcxo >= 0 && g_chip.pos_cal > g_chip.pos_tcxo, "A: full calibration comes AFTER the TCXO command");
    CHECK(g_chip.img_count == 1 && g_chip.img1 == 0xD7u && g_chip.img2 == 0xDBu, "A: image calibration is the 863-870 MHz band");
    CHECK(g_chip.pos_img > g_chip.pos_cal, "A: image calibration comes AFTER the full calibration");
    CHECK(g_chip.cal_outside_rc == 0, "A: calibrations were launched from STDBY_RC");
    CHECK(rep.device_errors == 0u, "A: device errors end clean");

    /* ---- B: crystal module (must never be told about a TCXO) ---- */
    chip_init(0, 0, 0);
    rc = sx1262_setup_clock_and_calibration(&rep);
    CHECK(rc == 0 && rep.result == SX1262_SETUP_OK, "B: setup succeeds on a crystal module");
    CHECK(rep.tcxo_enabled == 0, "B: reports no TCXO switched on");
    CHECK(g_chip.tcxo_count == 0, "B: TCXO command NEVER sent to a crystal module");
    CHECK(g_chip.cal_count == 0, "B: no forced full calibration");
    CHECK(g_chip.img_count == 1 && g_chip.img1 == 0xD7u && g_chip.img2 == 0xDBu, "B: image calibration still done for 863-870 MHz");
    CHECK(g_chip.cal_outside_rc == 0, "B: calibrations were launched from STDBY_RC");
    CHECK(rep.device_errors == 0u, "B: device errors end clean");

    /* ---- C: TCXO module that needs 3.0 V; default is 1.8 V ---- */
    chip_init(1, SX126X_TCXO_CTRL_3_0V, 0);
    rc = sx1262_setup_clock_and_calibration(&rep);
    CHECK(rc != 0 && rep.result == SX1262_SETUP_FAIL_XOSC_STILL_DEAD, "C: fails with the oscillator-still-dead result");
    CHECK((rep.device_errors & SX126X_ERRORS_XOSC_START) != 0u, "C: report carries the XOSC_START error bit");
    CHECK(g_chip.tcxo_count == 1 && g_chip.tcxo_voltage == SX126X_TCXO_CTRL_1_8V,
          "C: tried only the one safe voltage, did not guess higher ones");

    /* ---- D: dead SPI bus ---- */
    chip_init(0, 0, 1);
    rc = sx1262_setup_clock_and_calibration(&rep);
    CHECK(rc != 0 && rep.result == SX1262_SETUP_FAIL_STANDBY_RC, "D: dead bus fails at the first step");

    /* ---- E: PA config ---- */
    chip_init(0, 0, 0);
    rc = sx1262_setup_pa_config();
    CHECK(rc == 0 && g_chip.pa_count == 1, "E: PA config sent once");
    CHECK(g_chip.pa[0] == 0x04u && g_chip.pa[1] == 0x07u && g_chip.pa[2] == 0x00u && g_chip.pa[3] == 0x01u,
          "E: PA config bytes are 04 07 00 01 (SX1262, datasheet 13.1.14)");

    if (g_failures != 0)
    {
        printf("sx1262_setup_selftest: FAILED (%d of %d checks)\n", g_failures, g_checks);
        return 1;
    }
    printf("sx1262_setup_selftest: OK (%d checks passed)\n", g_checks);
    return 0;
}
