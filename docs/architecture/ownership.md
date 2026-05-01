# Ownership Map

This document defines subsystem ownership by role.
Ownership is assigned by responsibility, not by person.

---

## Embedded Lead
Owns:
- firmware/
- µT-Kernel task architecture
- watchdog task
- task scheduling
- shared memory
- mutex / semaphore behavior
- ISR timing
- docs/architecture/rtos_execution.md
- docs/contracts/team_state_struct.md
- docs/tests/watchdog_validation.md
- docs/logs/embedded_log.md

---

## Algorithms Lead
Owns:
- simulator/
- Agent State Manager
- task scoring
- CBBA / ICBAA
- 2-opt post-processing
- dynamic task generation
- docs/contracts/allocator_io.md
- docs/tests/allocator_validation.md
- docs/logs/algorithms_log.md

---

## ML Lead
Owns:
- ml/
- dataset generation
- feature vector definition
- model training
- quantisation
- inference mapping
- docs/contracts/inference_io.md
- docs/tests/inference_latency.md
- docs/logs/ml_log.md

---

## Integration Lead
Owns:
- dashboard/
- UART bridge
- parser
- telemetry
- FastAPI / WebSocket
- dashboard rendering
- event logging
- docs/contracts/uart_protocol.md
- docs/tests/telemetry_validation.md
- docs/logs/integration_log.md

---

## Shared Ownership
Shared by all technical roles:
- docs/architecture/system_overview.md
- docs/architecture/dataflow.md
- docs/architecture/ownership.md
- README.md
- docs/report/