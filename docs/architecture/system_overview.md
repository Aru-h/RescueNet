# System Overview

## Objective

Build a real-time disaster rescue coordination system on µT-Kernel 3.0 that simulates multiple human rescue teams as concurrent RTOS tasks and performs deterministic task reallocation under failure conditions using watchdog-triggered anomaly detection and on-device AI inference.

---

## Problem

Disaster response environments are dynamic, time-critical, and resource-constrained.

Rescue teams must be coordinated under changing mission conditions:
- new incidents appear
- teams lose stamina
- communication degrades
- tasks must be reassigned quickly

In these environments, delayed coordination causes mission failure.

The system addresses this by performing deterministic, on-device rescue team coordination with bounded response latency and no cloud dependency.

---

## Scope

The contest implementation includes:

- 5 concurrent rescue team tasks
- 1 watchdog supervisor task
- heartbeat-based anomaly detection
- capability-aware task scoring
- deterministic task reallocation
- on-device MLP inference on STM32N657
- UART telemetry output
- browser-based mission dashboard

This implementation is single-board and self-contained.

All coordination logic executes locally.

---

## System Model

The system is composed of two connected layers:

### Embedded Layer
Runs on STM32N657 with µT-Kernel 3.0.

Responsible for:
- concurrent team task execution
- watchdog supervision
- shared state updates
- anomaly detection
- inference trigger
- telemetry output

### Simulation + Visualization Layer
Runs on host machine.

Responsible for:
- task allocation logic
- environment simulation
- telemetry parsing
- dashboard rendering
- event trace logging

---

## Success Criteria

The system is considered successful if it can:

- coordinate 5 concurrent rescue teams
- detect task anomalies within bounded time
- trigger deterministic reallocation
- preserve system responsiveness under failure
- maintain stable telemetry output
- visualize mission state in real time

---

## Non-Goals

The contest implementation does not include:

- multi-board deployment
- distributed radio mesh
- voice / NLP mission input
- cloud inference
- autonomous robotics
- physical rescue hardware

These are outside contest scope.