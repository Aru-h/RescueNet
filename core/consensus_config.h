#ifndef CONSENSUS_CONFIG_H
#define CONSENSUS_CONFIG_H

/*
 * Build flags for peer consensus (CONSENSUS_DESIGN.md 6.1). Kept apart from
 * consensus.h so recovery_manager_config.h can size the evaluator table
 * without pulling in the whole consensus API.
 *
 * VIRTUAL_BUS_ENABLED   Defaults to 0 in core/vbus.h. The laptop `tron`
 *                       target sets 1; every board and Pico build leaves 0
 *                       and so compiles none of the consensus code.
 * CONSENSUS_ENFORCE     0 (default) = shadow mode: consensus runs and is
 *                       logged, recovery behaviour is unchanged. 1 = the
 *                       peer evaluator joins the recovery manager and
 *                       consensus ELEVATED counts as corroboration for the
 *                       fault predictor (decision D1).
 * CONSENSUS_CAP_LOCAL_EVALUATORS
 *                       0 (default). 1 also caps the local mobility and
 *                       communication evaluators at LOCAL_RECOVERY unless
 *                       consensus is ELEVATED (design 4.2(a)). Needs
 *                       CONSENSUS_ENFORCE.
 * CONSENSUS_FAST_PATH   1 (default). Communication fast path (design 2.6). On by
 *                       default (decision D7, revised at merge): with the default
 *                       IID link loss it gives 0 false ELEVATED up to q = 0.20, and
 *                       on the combined profile, where most comm faults are outside
 *                       the f = 2 budget, it detects 98.8% vs 48.3% with it off
 *                       (CONSENSUS_DESIGN.md 10.2, 10.4). Known cost: under
 *                       PER_SOURCE loss it raises about 10 false ELEVATED per 1000
 *                       target-rounds at q = 0.05. Set 0 for the vote path only.
 * CONSENSUS_TEST_HOOKS  0 (default). Test-only forcing hooks.
 */

#include "vbus.h"

#ifndef CONSENSUS_ENFORCE
#define CONSENSUS_ENFORCE 0
#endif
#ifndef CONSENSUS_CAP_LOCAL_EVALUATORS
#define CONSENSUS_CAP_LOCAL_EVALUATORS 0
#endif
#ifndef CONSENSUS_FAST_PATH
#define CONSENSUS_FAST_PATH 1
#endif
#ifndef CONSENSUS_TEST_HOOKS
#define CONSENSUS_TEST_HOOKS 0
#endif

#if CONSENSUS_ENFORCE && !VIRTUAL_BUS_ENABLED
#error "CONSENSUS_ENFORCE=1 requires VIRTUAL_BUS_ENABLED=1"
#endif
#if CONSENSUS_CAP_LOCAL_EVALUATORS && !CONSENSUS_ENFORCE
#error "CONSENSUS_CAP_LOCAL_EVALUATORS=1 requires CONSENSUS_ENFORCE=1"
#endif

#endif /* CONSENSUS_CONFIG_H */
