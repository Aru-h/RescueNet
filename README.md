# TRON 2026 — Real-Time Rescue Coordination on µT-Kernel 3.0

A real-time disaster rescue team coordination system built for the TRON Programming Contest 2026 (Student Category), running on µT-Kernel 3.0 with edge AI on STM32N657.

The system simulates five human rescue teams as concurrent RTOS tasks and performs deterministic task reallocation under failure conditions using watchdog-triggered anomaly detection, capability-aware task scoring, and on-device AI inference.

## Core Features

- Real-time team coordination on µT-Kernel 3.0
- Five concurrent rescue team tasks + watchdog supervisor
- Deterministic anomaly detection and task preemption
- Capability-aware CBBA task allocation
- Edge AI inference on STM32N657 Neural-ART NPU
- UART telemetry pipeline to external dashboard
- Live mission visualization and event trace

## System Overview

The system operates as two connected layers:

### Embedded Layer (STM32N657 + µT-Kernel 3.0)
- 5 concurrent rescue team tasks
- 1 watchdog task
- shared state registry
- anomaly detection
- deterministic reallocation trigger
- UART telemetry output

### Simulation + Visualization Layer
- task scoring and allocation
- dynamic task generation
- telemetry parser
- FastAPI + WebSocket backend
- browser dashboard

## Project Structure

- `firmware/` — STM32 firmware and µT-Kernel tasks
- `simulator/` — task allocation and environment simulation
- `dashboard/` — telemetry backend and visualization
- `ml/` — model training and deployment artifacts
- `docs/` — architecture, contracts, tests, and report material

## Status

Active development.

## License

MIT