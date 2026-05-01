# Data Flow

## Objective

Define how state, decisions, and telemetry move across the system from task execution to visualization.

---

## End-to-End Flow

The system operates as a closed loop:

1. Team tasks execute mission work
2. Team state is updated
3. Watchdog monitors system state
4. anomaly is detected
5. reallocation is triggered
6. assignments are updated
7. telemetry is emitted
8. dashboard reflects updated mission state

This loop repeats continuously during mission execution.

---

## Embedded Flow

Inside the embedded system:

1. Team task executes assigned work
2. Team task updates TeamStateRegistry
3. Team task publishes heartbeat
4. Watchdog scans shared state
5. Watchdog detects anomaly
6. Watchdog triggers inference
7. Watchdog updates TaskAssignmentTable
8. UART packet is emitted

This defines the embedded control path.

---

## Host Flow

On the host system:

1. UART packet is received
2. packet is parsed
3. event is normalized
4. dashboard state is updated
5. event is logged
6. UI is re-rendered

This defines the visualization path.

---

## Decision Flow

Decision-making follows this path:

1. Team state changes
2. anomaly threshold exceeded
3. watchdog raises event
4. reallocation trigger issued
5. allocator / inference computes reassignment
6. updated assignments applied

This defines the reallocation path.

---

## Failure Flow

Failure handling follows this path:

1. team degrades or stalls
2. heartbeat / stamina anomaly emerges
3. watchdog detects abnormal state
4. anomaly flag raised
5. task reassignment triggered
6. mission continues under updated allocation

This defines the recovery path.

---

## Logging Flow

All major events are logged:

- heartbeat updates
- anomaly events
- reallocation events
- assignment changes
- telemetry packets
- dashboard state transitions

This creates a full event trace for debugging and validation.