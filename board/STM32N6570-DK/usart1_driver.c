/*
 * usart1_driver.c - real (non-weak) board_usart1_read_byte() for the
 * DK's laptop-facing USART1 link. See mission_link_task.c's own
 * weak-stub comment on board_usart1_read_byte() - this file is "the
 * UART driver's own .c file" that comment says would override it;
 * nothing in mission_link_task.c needs to change once this is linked
 * in, the linker resolves every call to this definition automatically.
 *
 * mtk3_bsp2's own USART1 path (vendor/mtk3_bsp2/sysdepend/stm32_cube/
 * lib/libtm/discovery_stm32n657/tm_com.c) already brings this UART up
 * automatically before usermain() ever runs - mtkernel's sysinit.c
 * calls libtm_init(), which calls tm_com_init(): 115200-8-N-1, both TX
 * and RX enabled at the register level (CR1 = UE|RE|TE). Its own RX
 * path (tm_getchar()/tm_rcv_dat()) is unusable here though - both
 * block forever waiting for a byte with interrupts globally disabled
 * (see libtm.c) - not something a per-tick poll that must never stall
 * the sim can call.
 *
 * Bug fix #1: originally used the plain `USART1` macro from the CMSIS
 * header. That macro resolves to the SECURE alias (USART1_S,
 * 0x52001000) only when CPU_IN_SECURE_STATE is defined at compile
 * time - it isn't, in this project's build (no such define in either
 * build config) - so it silently resolved to the NON-secure alias
 * (USART1_NS, 0x42001000) instead. tm_com_init() (and every other
 * read/write this board does of this peripheral) targets the SECURE
 * alias directly, unconditionally, because TRUSTZONE_SECURE=1 for
 * this target (see vendor/mtk3_bsp2's own sysdef.h) - it doesn't key
 * off CPU_IN_SECURE_STATE at all. Reading the non-secure alias meant
 * RXNE never appeared to be set (a different, inactive view of the
 * same physical UART), so this function silently returned -1 forever
 * no matter what arrived on the wire. Fixed by using USART1_S
 * explicitly, matching tm_com.c's own convention.
 *
 * Bug fix #2 (this revision): a first pass of this file polled RXNE
 * from board_usart1_read_byte() instead of using an interrupt - fine
 * in isolation, but demonstrated (with real hardware, real laptop
 * traffic) to lose bytes in practice: mission_link_handshake_tx_step()
 * (mission_link_task.c) retries a relay Send to the LoRa peer every
 * HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS (500ms) via comms_transport_
 * send() -> sx1262_hal.c's SPI path, which - with no SX1262 module
 * actually wired up (as during this exact bring-up phase) - blocks on
 * wait_busy_low()'s BUSY-pin timeout for real, measured hundreds of
 * milliseconds per attempt. That's long enough that a multi-byte
 * frame arriving mid-block overruns: USART1 has a single-byte receive
 * buffer, no FIFO enabled, so every byte after the first in a frame
 * that arrives while nothing is polling gets silently discarded via
 * ORE before board_usart1_read_byte() next runs - confirmed via a
 * debug trace that showed exactly one magic byte received per
 * handshake attempt, forever, no matter how many times the laptop
 * retried.
 *
 * Deliberately NOT fixed by shrinking sx1262_hal.c's SPI/BUSY
 * timeouts - those are sized against real Tswmode datasheet timing
 * with a documented safety margin for when a real module IS attached
 * (Goal 2); shrinking them to paper over this is exactly the kind of
 * change that quietly breaks real hardware later. Fixed here instead,
 * at the actual point of loss: RXNE now drives a real NVIC interrupt
 * into a small software ring buffer, so a byte is captured within
 * interrupt latency of arriving regardless of how long anything else
 * (SPI busy-waits included) blocks the main loop. Safe to hook a bare
 * NVIC interrupt here despite mtk3_bsp2 not exposing tk_def_int() for
 * USART1 (see vendor/README.md - only I2C/ADC have device wrappers):
 * this ISR never calls any tk_* kernel service and touches nothing
 * but this file's own ring buffer, so it cannot corrupt kernel state
 * no matter what priority it runs at or what DI()-protected critical
 * section it preempts - the concern that blocked this approach the
 * first time this file was written doesn't apply to a leaf ISR this
 * plain.
 */

#include "stm32n657xx.h"

/* Sized for the laptop's real traffic, not a single frame: the bridge
   sends up to 10 frames/s of <= 40 B at 115200 baud (~11.5 B/ms on the
   wire), and the sim task can legitimately go hundreds of ms between
   pumps (a CBBA round, an SX1262 BUSY-pin timeout - see Bug fix #2
   above). The old 64-byte ring overflowed after ~100-150 ms without a
   pump; 512 B covers ~45 ms of back-to-back line-rate bytes, or several
   seconds of the bridge's actual frame rate. Keep it a power of 2: the
   head/tail math below uses %, which then compiles to a mask. */
#define USART1_RX_RING_CAPACITY 512u
_Static_assert((USART1_RX_RING_CAPACITY & (USART1_RX_RING_CAPACITY - 1u)) == 0u,
               "USART1_RX_RING_CAPACITY must be a power of 2");

static volatile uint8_t g_rx_ring[USART1_RX_RING_CAPACITY];
static volatile uint32_t g_rx_ring_head; /* next slot the ISR writes */
static volatile uint32_t g_rx_ring_tail; /* next slot board_usart1_read_byte() reads */
/* Bytes the ISR received but had to drop because the ring was full.
   Written only by the ISR; a single aligned 32-bit word, so the
   getter's plain read is atomic on this core. */
static volatile uint32_t g_rx_ring_drop_count;

void USART1_IRQHandler(void)
{
    while ((USART1_S->ISR & USART_ISR_RXNE) != 0u)
    {
        uint8_t byte = (uint8_t)(USART1_S->RDR & 0xFFu);

        if ((USART1_S->ISR & USART_ISR_ORE) != 0u)
        {
            USART1_S->ICR = USART_ICR_ORECF;
        }

        /* Drop the byte if the ring is full rather than overwrite an
           unread one - same "never block, bounded loss under real
           overload" contract this file's read side already accepted
           for the hardware's own single-byte buffer, just moved one
           level up to a deeper (USART1_RX_RING_CAPACITY) buffer. The
           drop is counted, not silent: see
           board_usart1_rx_drop_count(). */
        if ((g_rx_ring_head - g_rx_ring_tail) < USART1_RX_RING_CAPACITY)
        {
            g_rx_ring[g_rx_ring_head % USART1_RX_RING_CAPACITY] = byte;
            g_rx_ring_head++;
        }
        else
        {
            g_rx_ring_drop_count++;
        }
    }
}

void board_usart1_rx_init(void)
{
    USART1_S->CR1 |= USART_CR1_RXNEIE;

    NVIC_SetPriority(USART1_IRQn, 15u); /* lowest priority - this ISR
        never contends with kernel-managed interrupts for correctness,
        only for CPU time, and it's a handful of instructions per
        byte */
    NVIC_EnableIRQ(USART1_IRQn);
}

int board_usart1_read_byte(void)
{
    uint8_t byte;

    if (g_rx_ring_head == g_rx_ring_tail)
    {
        return -1;
    }

    byte = g_rx_ring[g_rx_ring_tail % USART1_RX_RING_CAPACITY];
    g_rx_ring_tail++;

    return (int)byte;
}

/* Running total of received bytes dropped because the RX ring was full
   (the sim task went too long without calling board_usart1_read_byte()).
   Monotonic; wraps at 2^32. */
uint32_t board_usart1_rx_drop_count(void)
{
    return g_rx_ring_drop_count;
}
