/*
 * sx1262_setup.h - SX1262 clock, calibration and PA setup shared by both
 * boards' radio HALs (board/STM32N6570-DK/sx1262_hal.c and
 * board/Raspberry_Pi_Pico/sx1262_hal.c), so the two can't drift apart.
 *
 * Why this exists. Neither HAL's bring-up used to do three things the
 * Semtech SX1261/2 datasheet (Rev 1.2) calls for, and one of them can leave
 * the radio unable to transmit or receive at all:
 *
 *  1. TCXO. The Ebyte E22-900M22S manual says "DIO 3 is used to supply 32
 *     MHz TCXO". If the module has a TCXO, the chip does not power it
 *     until told to with SetDIO3AsTCXOCtrl (datasheet 13.3.6). Without
 *     that, the 32 MHz oscillator never starts and the chip can't leave
 *     STDBY_RC for RX/TX; power-on calibration also fails, so a full
 *     recalibration is required afterwards (datasheet 9.2.1). Some E22
 *     variants have a plain crystal instead, and telling a crystal-based
 *     chip to expect a TCXO would break it - so this is PROBED, not
 *     assumed: the command is sent only if the oscillator fails to start
 *     without it.
 *  2. Image calibration. The default is tuned for 902-928 MHz; this
 *     project runs at 868 MHz, so the 863-870 MHz band is calibrated
 *     explicitly (datasheet Table 9-2: 0xD7, 0xDB).
 *  3. PA configuration. Step 4 of the datasheet's own basic TX sequence
 *     (14.2) is SetPaConfig, which is also the command that selects the
 *     SX1262 power amplifier (13.1.14).
 *
 * Header-only and hardware-independent (it only calls the vendored
 * sx126x_* driver API), so laptop/tools/sx1262_setup_selftest.c runs the
 * exact same code against a simulated chip.
 */

#ifndef SX1262_SETUP_H
#define SX1262_SETUP_H

#include <stdint.h>

#include "sx126x.h"

/* Not <stddef.h>: the Pico BSP defines its own size_t, which clashes with the
   toolchain's. Only NULL is needed here. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* TCXO supply voltage, used ONLY if the oscillator probe fails. 1.8 V is
 * the most common TCXO supply and the lowest of the options, so it cannot
 * overstress a 1.8-3.3 V part; if a module needs more, override with
 * -DSX1262_SETUP_TCXO_VOLTAGE=SX126X_TCXO_CTRL_x_xV after checking the
 * module's real TCXO rating. Note the chip needs supply >= TCXO voltage
 * + 200 mV (datasheet 13.3.6). */
#ifndef SX1262_SETUP_TCXO_VOLTAGE
#define SX1262_SETUP_TCXO_VOLTAGE  SX126X_TCXO_CTRL_1_8V
#endif

/* TCXO start-up gating delay, in 15.625 us steps (320 = 5 ms). */
#ifndef SX1262_SETUP_TCXO_DELAY_STEPS
#define SX1262_SETUP_TCXO_DELAY_STEPS  320u
#endif

/* Image-calibration band, datasheet Table 9-2 row "863 - 870 MHz". */
#ifndef SX1262_SETUP_IMG_CAL_FREQ1
#define SX1262_SETUP_IMG_CAL_FREQ1  0xD7u
#endif
#ifndef SX1262_SETUP_IMG_CAL_FREQ2
#define SX1262_SETUP_IMG_CAL_FREQ2  0xDBu
#endif

typedef enum
{
    SX1262_SETUP_OK = 0,
    SX1262_SETUP_FAIL_STANDBY_RC,
    SX1262_SETUP_FAIL_CLEAR_ERRORS,
    SX1262_SETUP_FAIL_XOSC_PROBE,
    SX1262_SETUP_FAIL_TCXO_CMD,
    SX1262_SETUP_FAIL_CALIBRATE,
    SX1262_SETUP_FAIL_XOSC_STILL_DEAD,
    SX1262_SETUP_FAIL_IMAGE_CAL,
    SX1262_SETUP_FAIL_READ_ERRORS
} sx1262_setup_result_t;

typedef struct
{
    sx1262_setup_result_t result;
    int tcxo_enabled;          /* 1 if this code had to switch DIO3 into TCXO-supply mode */
    uint16_t device_errors;    /* SX126X_ERRORS_* bits still set at the end (0 = clean) */
} sx1262_setup_report_t;

static inline const char *sx1262_setup_result_name(sx1262_setup_result_t r)
{
    switch (r)
    {
    case SX1262_SETUP_OK:                   return "ok";
    case SX1262_SETUP_FAIL_STANDBY_RC:      return "standby_rc (no SPI answer?)";
    case SX1262_SETUP_FAIL_CLEAR_ERRORS:    return "clear_device_errors";
    case SX1262_SETUP_FAIL_XOSC_PROBE:      return "xosc_probe";
    case SX1262_SETUP_FAIL_TCXO_CMD:        return "set_dio3_as_tcxo_ctrl";
    case SX1262_SETUP_FAIL_CALIBRATE:       return "calibrate";
    case SX1262_SETUP_FAIL_XOSC_STILL_DEAD: return "xosc still not starting even with TCXO on";
    case SX1262_SETUP_FAIL_IMAGE_CAL:       return "image_calibration";
    case SX1262_SETUP_FAIL_READ_ERRORS:     return "get_device_errors";
    default:                                return "?";
    }
}

#define SX1262_SETUP_STEP(expr, code)                       \
    do {                                                    \
        if ((expr) != SX126X_STATUS_OK) {                   \
            rep->result = (code);                           \
            return 1;                                       \
        }                                                   \
    } while (0)

/* Call right after the chip has been reset, before any other
 * configuration. Leaves the chip in STDBY_RC. Returns 0 on success, 1 on
 * failure (rep->result says where). rep->device_errors is filled either
 * way it can be, so the caller can print it. */
static inline int sx1262_setup_clock_and_calibration(sx1262_setup_report_t *rep)
{
    sx126x_errors_mask_t errs = 0u;

    rep->result = SX1262_SETUP_OK;
    rep->tcxo_enabled = 0;
    rep->device_errors = 0u;

    SX1262_SETUP_STEP(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC), SX1262_SETUP_FAIL_STANDBY_RC);

    /* A TCXO module raises XOSC_START_ERR at power-on by design (datasheet
       13.3.6) - clear it so the probe below sees only what happens now. */
    SX1262_SETUP_STEP(sx126x_clear_device_errors(NULL), SX1262_SETUP_FAIL_CLEAR_ERRORS);

    /* Probe: does the 32 MHz oscillator start without any TCXO command? */
    SX1262_SETUP_STEP(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_XOSC), SX1262_SETUP_FAIL_XOSC_PROBE);
    SX1262_SETUP_STEP(sx126x_get_device_errors(NULL, &errs), SX1262_SETUP_FAIL_XOSC_PROBE);

    if ((errs & SX126X_ERRORS_XOSC_START) != 0u)
    {
        /* No clock: this is a TCXO module the chip isn't powering yet. */
        SX1262_SETUP_STEP(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC), SX1262_SETUP_FAIL_STANDBY_RC);
        SX1262_SETUP_STEP(sx126x_clear_device_errors(NULL), SX1262_SETUP_FAIL_CLEAR_ERRORS);
        SX1262_SETUP_STEP(sx126x_set_dio3_as_tcxo_ctrl(NULL, SX1262_SETUP_TCXO_VOLTAGE,
                                                       SX1262_SETUP_TCXO_DELAY_STEPS),
                          SX1262_SETUP_FAIL_TCXO_CMD);
        /* Power-on calibration ran without the TCXO and failed; redo all. */
        SX1262_SETUP_STEP(sx126x_cal(NULL, SX126X_CAL_ALL), SX1262_SETUP_FAIL_CALIBRATE);
        SX1262_SETUP_STEP(sx126x_clear_device_errors(NULL), SX1262_SETUP_FAIL_CLEAR_ERRORS);

        SX1262_SETUP_STEP(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_XOSC), SX1262_SETUP_FAIL_XOSC_PROBE);
        SX1262_SETUP_STEP(sx126x_get_device_errors(NULL, &errs), SX1262_SETUP_FAIL_XOSC_PROBE);
        if ((errs & SX126X_ERRORS_XOSC_START) != 0u)
        {
            rep->device_errors = errs;
            rep->result = SX1262_SETUP_FAIL_XOSC_STILL_DEAD;
            return 1;
        }
        rep->tcxo_enabled = 1;
    }

    /* Image calibration must be launched from STDBY_RC. */
    SX1262_SETUP_STEP(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC), SX1262_SETUP_FAIL_STANDBY_RC);
    SX1262_SETUP_STEP(sx126x_cal_img(NULL, SX1262_SETUP_IMG_CAL_FREQ1, SX1262_SETUP_IMG_CAL_FREQ2),
                      SX1262_SETUP_FAIL_IMAGE_CAL);

    SX1262_SETUP_STEP(sx126x_get_device_errors(NULL, &errs), SX1262_SETUP_FAIL_READ_ERRORS);
    rep->device_errors = errs;
    return 0;
}

/* SX1262 power-amplifier configuration: paDutyCycle 0x04 and hpMax 0x07 are
 * the datasheet's maximums for the SX1262 (13.1.14: paDutyCycle must not
 * exceed 0x04), deviceSel 0 = SX1262, paLut reserved = 0x01. The actual
 * output power is still chosen by SetTxParams. Call after SetRfFrequency
 * and before SetTxParams. Returns 0 on success. */
static inline int sx1262_setup_pa_config(void)
{
    sx126x_pa_cfg_params_t pa;

    pa.pa_duty_cycle = 0x04u;
    pa.hp_max = 0x07u;
    pa.device_sel = 0x00u;
    pa.pa_lut = 0x01u;

    return (sx126x_set_pa_cfg(NULL, &pa) == SX126X_STATUS_OK) ? 0 : 1;
}

#undef SX1262_SETUP_STEP

#endif /* SX1262_SETUP_H */
