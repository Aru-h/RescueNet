# RTOS Execution Model
> Status: Draft — pending Embedded Lead validation.
## Objective

Define the µT-Kernel 3.0 execution model for deterministic multi-task rescue coordination on STM32N657.

---

## Task Model

The embedded system runs 6 concurrent µT-Kernel tasks:

1. Team_1_Task
2. Team_2_Task
3. Team_3_Task
4. Team_4_Task
5. Team_5_Task
6. Watchdog_Task

Each team task simulates one rescue team operating in the mission environment.

The watchdog task supervises all team tasks and has highest scheduling priority.

---

## Scheduling Model

The scheduler uses priority-based preemptive execution.

- Watchdog_Task runs at highest priority
- Team tasks run at equal priority
- Team tasks are scheduled round-robin
- Watchdog preempts immediately on anomaly event

This ensures anomaly handling is deterministic and bounded.

---

## Task Responsibilities

### Team Tasks
Each team task is responsible for:

- maintaining team state
- executing assigned mission task
- updating heartbeat
- consuming stamina
- publishing telemetry state

Each task operates independently and updates shared state at fixed intervals.

### Watchdog Task
The watchdog task is responsible for:

- monitoring heartbeats
- tracking stamina drain
- detecting anomalies
- triggering reallocation
- invoking inference
- updating task assignments

The watchdog is the system supervisor.

---

## Shared Memory Model

Two shared structures are maintained:

### TeamStateRegistry
Stores:
- team position
- stamina
- current assignment
- availability
- heartbeat timestamp

Access:
- read/write by team tasks
- read by watchdog

Protected by mutex.

### TaskAssignmentTable
Stores:
- active task assignments
- reassignment state
- task ownership

Access:
- read by team tasks
- write by watchdog

Protected by semaphore.

---

## Synchronization Model

The system uses:

- mutex for TeamStateRegistry
- semaphore for TaskAssignmentTable
- event signal for anomaly trigger
- ISR-safe UART buffer for telemetry

Synchronization is bounded and deterministic.

No unbounded blocking is permitted in critical execution paths.

---

## Timing Model

Execution is designed around bounded timing behavior.

Target constraints:

- heartbeat update at fixed task interval
- watchdog scan within bounded cycle
- anomaly detection within deadline
- inference trigger immediately after anomaly
- telemetry output at fixed periodic rate

All critical paths must remain deterministic.

---

## Failure Handling

On anomaly detection:

1. Watchdog preempts normal execution
2. anomaly flag is raised
3. inference is triggered
4. task assignment is recomputed
5. TaskAssignmentTable is updated
6. team tasks resume under new assignments

The system recovers without requiring restart.

---

## Design Constraints

- no dynamic allocation in critical loop
- bounded synchronization only
- deterministic shared state access
- watchdog always preemptive
- telemetry must not block control path