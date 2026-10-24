# Known Limitations

What the submitted system does not do, or has not yet been shown to do. Every
item is current as of this build. Developer-level detail (dataset caveats,
accepted code-audit findings, implementation notes) is in
[`docs/DEVELOPER_NOTES.md`](docs/DEVELOPER_NOTES.md).

## A. Board and hardware

1. **The board runs from the debugger, not from flash.** The system is
   confirmed running on the STM32N6570-DK through the STM32CubeIDE `DevRam`
   configuration: the application is loaded into on-chip SRAM by the debugger
   (boot switches BOOT1 on, BOOT0 off) and runs the full mission, including
   handshake, world transfer, live telemetry and fault detection. Standalone
   Flash Boot hangs at the FSBL → application handoff. The diagnosis is in
   [`docs/reports/ST_Support_Report.pdf`](docs/reports/ST_Support_Report.pdf)
   and has been reported to STMicroelectronics (support case 00269955). Launch
   steps: [`docs/DK_DEMO_LAUNCH.md`](docs/DK_DEMO_LAUNCH.md).
2. **Reset the board between missions.** Starting a new mission without
   resetting the board leaves the mission-link session stuck and reporting
   stops.
3. **On-board timing and memory headroom are not yet measured.** Tick
   duration, stack/heap headroom over a full run, and the AI's inference time
   on the board (18,112 multiply-adds per team per tick, about 73 KB of
   weights) have not been instrumented. A 10× pace runs reliably; faster paces
   have run but are not guaranteed.
4. **All ten teams share one board, and there is no radio.** Team-to-team
   messages travel over an in-memory virtual bus with simulated packet loss,
   not over a radio link. The multi-board LoRa design is not part of this
   submission.

## B. Fault detection and the AI

5. **The AI acts on three of its five fault classes, because of the recovery
   layer, not the model.** It is trained on mobility, communication, resource,
   environment and mission faults, but only the first three act. Environment
   and mission faults don't damage the team: an environment fault raises the
   cost of a road cell, and a mission fault makes the incident worse. Every
   recovery action acts on the team (recover, reassign, send back, abort), so
   acting on these faults only pulls healthy teams off their work. A test with
   a perfect detector confirms it: letting environment and mission act costs
   8.8 incidents per run (95% CI 7.25–10.40) even when every detection is
   correct. Enabling them needs recovery actions on the map or the incident,
   not a better model (`ml/MODEL_REPORT.md` §7).
6. **The AI's gain is modest, and the model is not what limits it.** With the
   AI, teams resolve 38.95 incidents per run instead of 35.9 (+3.05, 95% CI
   1.30–4.80, held-out seeds). A perfect detector reaches only 39.7, so a
   better model could add at most about 0.75. The limit is the simulator's
   fault model: a fault clears on a fixed timer that no recovery action can
   shorten, and only resource faults are measurably recoverable by the current
   recovery actions (`ml/MODEL_REPORT.md` §9).
7. **Environment faults are nearly invisible.** An environment fault only
   raises the travel cost of one map cell, which leaves almost no trace in a
   team's own telemetry. The rules and consensus detect about 0% of them.
8. **The AI cannot act alone on serious actions.** On its own it can trigger
   only local recovery. Task reallocation, return to safe zone and abort also
   need the watchdog's flag, 8 consecutive ticks of model confidence, or peer
   consensus. This is deliberate (simplex design); it also means some real
   faults the AI sees are not acted on.
9. **Peer consensus adds robustness, not accuracy.** It stops one team's
   faulty self-report from hiding a fault, and stops one faulty peer from
   faking one. It does not catch more faults than the rules.
10. **`make evaluate`'s per-class detection table undercounts the AI.** Its
    recovery actions are not attributed to a fault class there. Use the
    mission outcome (incidents resolved) to judge the AI.
11. **All results come from the simulator.** Training, tuning and testing use
    simulated faults at a high rate (faults active on about 58% of team-ticks).
    Nothing has been validated against real incidents.

## C. Simulation model

12. **A tired team can be pulled off an almost-finished task.** When a team's
    stamina runs out it is sent to rest even if its incident is nearly done;
    another team usually finishes it later. This applies to every assignment,
    including manual overrides.

## D. Dashboard

13. **Manual overrides work only in simulation mode.** Dragging a team onto an
    incident is not available in replay mode or when the dashboard is
    connected to the board. After an override is committed, normal rules
    (such as item 12) still apply.
14. **Replay take-over needs a matching simulator.** A recording can be taken
    over only by the same simulator build that made it (same code, AI on or
    off); otherwise the dashboard refuses rather than show a diverging run.
15. **Tested on macOS and Linux.** Windows is not tested. Absolute results
    differ slightly between platforms for the same seed.
16. **The bundled demo recording does not replay exactly on Linux.** The
    simulator is deterministic on a given platform, but not across platforms.
    `demo_seed1002_combined_600ticks.jsonl.gz` was recorded on macOS (arm64,
    clang) and replays exactly there. On Linux x86_64 the same seed diverges
    at frame 88 (team 8 picks a different slot and incident), so "Take over
    from here" refuses it, and the take-over step of
    `make check-dashboard-e2e` is expected to fail on Linux. Everything else
    in that check passes. This is believed to be cross-platform
    floating-point divergence; the cause has not been confirmed.
17. **The 3D incident pin has no time-pressure ring yet.** The simulator does
    not send an incident's age, deadline or victims' survival time, so the
    dashboard cannot show how urgent an incident is in time. It is left out
    on purpose (not even a "seen for N ticks" stand-in, which would not be
    the incident's real age). To do: add the ring once the simulator sends
    incident age / survival from the causal mission model.
18. **The 3D pin's progress arc is an estimate.** The simulator sends the work
    remaining on an incident but not the starting amount, so the arc is
    measured against the largest value the dashboard has seen for that
    incident. It starts empty when the dashboard connects to an incident
    that is already partly worked, and shows nothing when the remaining work
    is sent as 0.
19. **Evacuation gateway markers show where an exit is, nothing more.** The
    simulator sends each gateway's position only (STATICENV `gateways`), so the
    card has no evacuee counts and nothing says whether a gateway is reachable
    for a given team. In 2D and 3D the marker stays inside its own edge cell
    (3D: outside the map, one cell wide); in 2.5D the glyph stands upright on
    its cell and can cover part of a neighbouring cell on screen. Recordings
    made before the field existed show no gateways.
20. **Some evacuation gateways cannot be reached by some teams (world
    generation, not changed in v1).** An audit of seeds 1001-1100 (300
    gateways) with the simulator's own pathfinder (`find_path_steps`, the A*
    teams use, at world creation) found every gateway reachable from at least
    one safe zone, but not always from all of them:
    - 21 of 300 gateways are unreachable from some safe zone (seeds 1027,
      1034, 1041, 1047, 1050, 1051, 1069);
    - 7 of 300 are unreachable from some team's starting position (seeds 1034,
      1047, 1069). Worst cases: seed 1034 gateway 0 at (18,35), reachable from
      only 1 of 10 team starts; seed 1047 gateway 1 at (35,21), reachable from
      none of the 10 team starts (one safe zone can reach it).
    This is a property of how `core/environment.c` places gateways (a
    non-water border cell, then a road is carved to the nearest road; the
    result can still sit on a part of the map a team's start is cut off from,
    for example by the river). It is not a dashboard issue and was left
    unchanged so every dataset stays identical. A candidate for an opt-in world
    revision that picks gateways reachable from every safe zone.
