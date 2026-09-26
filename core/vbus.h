#ifndef VBUS_H
#define VBUS_H

#include <stdint.h>

/*
 * Virtual bus: a simulated LoRa-like broadcast medium so each team can be
 * its own node on one board (see CONSENSUS_DESIGN.md sections 1.3, 1.4,
 * 3.5 and 6.2). Compiled only when VIRTUAL_BUS_ENABLED is nonzero (default
 * 0), so multi-board builds contain none of it.
 *
 * Determinism and isolation:
 *  - The bus owns a private RNG stream seeded from VbusConfig_t.seed. It
 *    never touches core/random.c, so enabling it cannot change the world.
 *  - Frames are delivered in ascending src id, then send order, then
 *    ascending dst id, so a fixed seed gives a fixed delivery trace.
 *
 * Timing: a frame passed to vbus_send() between two vbus_step() calls is
 * delivered by the vbus_step() that follows it (delay_ticks = 0) or by the
 * one after (delay_ticks = 1). The call sequence per tick is therefore
 * vbus_send()... -> vbus_step(now) -> vbus_poll()...
 *
 * Airtime: every accepted vbus_send() is charged the real SF7/BW125/CR4:5
 * time on air of its WIRE size (payload + LORA_FRAME_HEADER_SIZE +
 * LORA_FRAME_TRAILER_SIZE), once per transmission (a broadcast is one
 * transmission however many nodes hear it), whether or not it is delivered.
 * Collisions are not modelled.
 *
 * Inbox overflow: each node has a VBUS_INBOX_DEPTH-frame FIFO. A frame
 * arriving at a full inbox is discarded (oldest kept, newest dropped) and
 * counted in VbusStats_t.dropped. The same applies if the bus-wide
 * in-flight queue (VBUS_PENDING_MAX) is full at vbus_send(): the send is
 * rejected (-1), counted as dropped, and no airtime is charged.
 */

#ifndef VIRTUAL_BUS_ENABLED
#define VIRTUAL_BUS_ENABLED 0
#endif

#if VIRTUAL_BUS_ENABLED

#include "lora_frame.h"   /* LORA_FRAME_MAX_PAYLOAD, header/trailer sizes */

#define VBUS_BROADCAST 0xFFu

#define VBUS_MAX_NODES    16u
#define VBUS_INBOX_DEPTH  16u
#define VBUS_PENDING_MAX  128u

typedef enum {
    VBUS_LOSS_IID,
    VBUS_LOSS_PER_SOURCE,
    VBUS_LOSS_GILBERT_ELLIOTT,
    VBUS_LOSS_SHARED_BURST
} VbusLossModel_t;

/*
 * Loss model parameters:
 *  IID:            each (src,dst) copy lost with probability link_loss.
 *  PER_SOURCE:     one draw per sender per step, lost with probability
 *                  link_loss for all of that sender's copies.
 *  GILBERT_ELLIOTT: per (src,dst) link two-state Markov chain, advanced once
 *                  per frame copy: good->bad with p_gb, bad->good with p_bg.
 *                  Loss probability is link_loss in good, loss_bad in bad.
 *                  Links start good.
 *  SHARED_BURST:   one channel-wide chain advanced once per step: good->burst
 *                  with p_gb, burst->good with p_bg. In a burst EVERY copy is
 *                  dropped (loss_bad is ignored); outside a burst copies are
 *                  lost IID with link_loss (0 by default = clean).
 * Probabilities are clamped to [0,1]; delay_ticks > 1 is clamped to 1.
 */
typedef struct {
    uint8_t  delay_ticks;          /* nominal delivery delay, must be <= 1 */
    VbusLossModel_t loss_model;
    float    link_loss;            /* IID / per-source loss probability */
    float    p_gb, p_bg, loss_bad; /* Gilbert-Elliott / burst parameters */
    uint32_t seed;                 /* derived from world_seed */
} VbusConfig_t;

typedef struct { uint32_t sent, delivered, dropped; uint32_t airtime_ms_total; } VbusStats_t;

/* Resets all state. num_nodes is clamped to VBUS_MAX_NODES; cfg NULL means
   zero-delay clean IID. */
void vbus_init(uint8_t num_nodes, const VbusConfig_t *cfg);

/* Queue a frame from src to dst (or VBUS_BROADCAST = every node but src).
   Returns 0 if accepted, -1 if rejected (bad node id, payload too long or
   NULL with len > 0, src dark, or in-flight queue full). */
int  vbus_send(uint8_t src, uint8_t dst, uint8_t frame_type, const uint8_t *payload, uint8_t len);

/* Deliver frames due at this step. now_tick is informational; delivery is
   counted in vbus_step() calls. */
void vbus_step(uint32_t now_tick);

/* Pop the oldest frame from node's inbox. Returns 1 and fills the outputs
   (any may be NULL; out_payload needs LORA_FRAME_MAX_PAYLOAD bytes), or 0 if
   empty / node invalid. */
int  vbus_poll(uint8_t node, uint8_t *out_src, uint8_t *out_frame_type,
               uint8_t *out_payload, uint8_t *out_len);

/* A dark node neither transmits (vbus_send returns -1) nor receives (copies
   addressed to it are dropped, and its inbox is not delivered to). Frames it
   sent before going dark stay in flight. */
void vbus_set_node_dark(uint8_t node, uint8_t dark);

/* Extra independent receive loss on one node, on top of the loss model. */
void vbus_set_node_rx_loss(uint8_t node, float extra_loss);

VbusStats_t vbus_get_stats(void);

/* Semtech AN1200.13 time on air, in microseconds (= ms x 1000, exact), for
   a WIRE frame of wire_len bytes: SF7, BW125, CR4/5, explicit header, CRC
   on, 8-symbol preamble, LDRO off. 80 bytes -> 143616 (143.616 ms). */
uint32_t lora_time_on_air_ms_x1000(uint32_t wire_len);

#endif /* VIRTUAL_BUS_ENABLED */

#endif /* VBUS_H */
