#ifndef COMMS_PRECEDENCE_H
#define COMMS_PRECEDENCE_H

/*
 * Message precedence tiers for core/comms_transport.c's TX dispatcher,
 * modeled on the CCEB military message-precedence standard (FLASH /
 * IMMEDIATE / PRIORITY / ROUTINE) - see the design conversation this
 * header comes out of for the full message-by-message mapping. Kept
 * as its own tiny header, not folded into comms_transport.h, because
 * both comms_transport.c (the dispatcher, which only needs to know
 * "there are 4 tiers, ordered") AND mission_link.c/cbba.c/
 * mission_link_override.c (the classifiers, which decide WHICH tier a
 * given message belongs in) need this enum, and comms_transport.h
 * should not have to pull in mission_link.h/cbba.h to expose it -
 * see comms_transport.h's own "payload-agnostic" doc for why that
 * dependency direction is deliberately avoided.
 *
 * Numeric values matter: the dispatcher loops downward from
 * COMMS_PRECEDENCE_FLASH to COMMS_PRECEDENCE_ROUTINE checking each
 * tier's TX queue in turn (non-preemptive strict-priority / head-of-
 * line scheduling - see core/comms_transport.c's dispatch function).
 * Don't renumber without also checking that loop.
 */
typedef enum
{
    COMMS_PRECEDENCE_ROUTINE   = 0, /* heartbeats, one-shot bootstrap - CCEB ROUTINE */
    COMMS_PRECEDENCE_PRIORITY  = 1, /* CBBA bidding, ordinary incident/fault updates - CCEB PRIORITY */
    COMMS_PRECEDENCE_IMMEDIATE = 2, /* handshake, dispatcher override, elevated fault severity - CCEB IMMEDIATE */
    COMMS_PRECEDENCE_FLASH     = 3, /* newly-discovered critical incidents, stamina/equipment abort-threshold status - CCEB FLASH */

    COMMS_PRECEDENCE_COUNT     = 4
} CommsPrecedence_t;

#endif /* COMMS_PRECEDENCE_H */
