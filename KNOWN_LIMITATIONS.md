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
