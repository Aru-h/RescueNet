#ifndef FAULT_COMMS_H
#define FAULT_COMMS_H

/*
 * Cross-board fault evidence + vote comms (Prompt 1). Two functions,
 * mirroring core/cbba.c's cbba_broadcast_local_teams()/cbba_ingest_
 * remote_mesh_replies() naming and shape.
 *
 * NOT wired into core/simulation.c's simulation_step() (the shared
 * per-tick loop laptop/main.c and both board mains drive via core/
 * sim_runner.c's sim_tick()) - matching how cbba_broadcast_local_teams()/
 * cbba_ingest_remote_mesh_replies() themselves are not wired in there
 * either. simulation_step() runs cbba_consensus()'s in-process, single-
 * process-simulates-every-team shortcut instead; the real cross-board
 * path (both CBBA's and this one) is only exercised today via a
 * dedicated two-board test harness (laptop/tools/two_board_cbba_
 * harness.c is CBBA's). These two functions are meant to be driven the
 * same way, once an equivalent harness exists for fault comms - not
 * something this pass adds.
 *
 * Call fault_comms_ingest_remote_frames() once per tick, before
 * fault_comms_broadcast_local_teams() - it calls fault_buffer_round_
 * reset() first thing, so votes arriving for the new round aren't
 * wiped by the reset meant for the round that just ended.
 *
 * Both send loops in this file iterate comms_transport_board_count()
 * (the real, runtime board count), not a compile-time constant - the
 * laptop build's -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS override
 * collapses everything to one board, and looping past that would
 * (harmlessly, since comms_transport_send() still routes correctly by
 * team_id, but pointlessly) resend to a board that doesn't exist under
 * that build's own configuration. representative_team_id = board *
 * FAULT_BUFFER_TEAMS_PER_BOARD stays the compile-time hardware value
 * (5u) for the multiplier itself - that's only ever evaluated for
 * boards the loop bound confirms actually exist, where it matches real
 * hardware layout exactly.
 *
 * KNOWN HAZARD, not fixable from this file: comms_transport_poll_
 * received() drains one shared per-board inbox, destructively (pops on
 * every call - see comms_transport.c), and core/cbba.c's own receiver
 * (cbba_ingest_remote_mesh_replies()) already drains that same inbox
 * with its own "continue" on any non-CBBA frame_type - which discards
 * the frame, not defers it, since it's already been popped. If this
 * file's receiver and CBBA's are ever both driven from the same tick,
 * whichever runs first silently consumes and drops the other's frames.
 * comms_transport.c is out of scope for this prompt and cbba.c isn't in
 * its file list either, so this can't be fixed here - flagging it so
 * whoever wires either receiver into a real shared main loop sees this
 * before it becomes a silent, hard-to-diagnose data-loss bug.
 */
void fault_comms_broadcast_local_teams(void);
void fault_comms_ingest_remote_frames(void);

/*
 * Item 7 (Prompt 1) + the num_boards/teams_per_board fix (Prompt 1b
 * follow-up): call once at startup, alongside recovery_manager_init()
 * (same call site - see core/simulation.c's generate_world()). Reads
 * the REAL runtime board layout via comms_transport_board_count() and
 * a one-time count of how many teams board 0 owns (comms_transport.c
 * doesn't expose its per-board team count as a getter, and adding one
 * is out of scope - "don't touch comms_transport.c" - so this derives
 * it from the public comms_transport_board_id_for_team() instead), and
 * passes both into fault_buffer_init(). This is the one place in this
 * file that needs to run before anything else - fault_buffer_fuse()'s
 * N_live math is meaningless against default/stale board-layout values.
 */
void fault_comms_init(void);

#endif
