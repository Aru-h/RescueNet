#ifndef STATE_HASH_H
#define STATE_HASH_H

#include <stdint.h>

/*
 * Per-tick determinism fingerprint of the simulation state.
 *
 * FNV-1a (64-bit) over the fields of the team roster, the incident pool,
 * every team's fault state and the recovery manager's per-team verdicts
 * and metrics - field by field, never raw struct bytes, so padding (whose
 * contents C does not define) can never leak into the hash. Floats go in
 * by bit pattern, so a 1-ulp difference changes the hash.
 *
 * Read-only: calling this changes no simulation state and consumes no RNG
 * draw, so enabling it cannot change a simulation result. Intended use is
 * to print it every N ticks on every platform and diff the lines: same
 * seed + same inputs must give the same hashes (docs/RTOS_DESIGN.md).
 *
 * Sub-hashes are kept separate so a mismatch says WHICH part diverged.
 * `all` folds the four sub-hashes and the tick together.
 */
typedef struct
{
    uint64_t teams;
    uint64_t incidents;
    uint64_t faults;
    uint64_t recovery;
    uint64_t all;
} StateHash_t;

void state_hash_compute(StateHash_t *out);

#endif
