/*
 * soft_spi.h - bit-banged SPI master, mode 0 (CPOL=0, CPHA=0), MSB first.
 *
 * Exists so a board's radio HAL can talk to the SX1262 over plain GPIO
 * when the SPI peripheral itself can't be trusted (see the SPI5 MASTER-bit
 * note in board/STM32N6570-DK/sx1262_hal.c). Hardware-independent on
 * purpose: the caller supplies pin access and the half-period delay, so
 * the exact same code runs on the board and in
 * laptop/tools/soft_spi_selftest.c against a simulated slave.
 *
 * Chip select is NOT handled here - the caller owns NSS, exactly as the
 * hardware-SPI path does.
 *
 * Mode 0 timing, per bit:
 *   MOSI set while SCK low -> delay -> SCK rises (slave samples MOSI)
 *   -> delay -> master samples MISO -> SCK falls (slave shifts next bit)
 * MISO is read at the end of the high phase, after the slave has had the
 * whole low phase plus the high phase to present it.
 */

#ifndef SOFT_SPI_H
#define SOFT_SPI_H

#include <stdint.h>

typedef struct
{
    void (*set_sck)(int level);
    void (*set_mosi)(int level);
    int  (*get_miso)(void);
    void (*half_period_delay)(void);
} soft_spi_ops_t;

/* Shifts one byte out on MOSI while shifting one byte in from MISO.
 * SCK must be low on entry (idle, mode 0) and is low on exit. */
static inline uint8_t soft_spi_xfer_byte(const soft_spi_ops_t *ops, uint8_t out)
{
    uint8_t in = 0u;
    int bit;

    for (bit = 7; bit >= 0; bit--)
    {
        ops->set_mosi((out >> bit) & 1u);
        ops->half_period_delay();
        ops->set_sck(1);
        ops->half_period_delay();
        in = (uint8_t)((in << 1) | (ops->get_miso() ? 1u : 0u));
        ops->set_sck(0);
    }

    return in;
}

#endif /* SOFT_SPI_H */
