# Allocator I/O Contract

## Objective

Define the input, output, and decision boundary for the task allocation system.

---

## Purpose

The allocator consumes team state and task state, computes assignment utility, and returns a deterministic task allocation decision.

It is responsible for selecting the best available team-task assignment under current mission conditions.

---

## Inputs

The allocator consumes:

### Team State
Per team:
- team_id
- position (x, y)
- stamina
- capability_type
- availability
- current_assignment

### Task State
Per task:
- task_id
- task_type
- task_position (x, y)
- priority
- status

### System State
- active_team_count
- active_task_count
- anomaly_flag
- current_tick

---

## Output

The allocator produces:

### Assignment Output
Per assignment:
- team_id
- assigned_task_id
- assignment_score
- assignment_reason

### Allocation Result
- allocation_status
- reassignment_count
- unresolved_task_count

---

## Decision Rules

The allocator must:

- produce deterministic output for identical inputs
- prefer higher utility assignments
- reject infeasible assignments
- avoid assigning unavailable teams
- avoid assigning completed tasks
- preserve bounded execution time

---

## Failure Behavior

If no valid assignment exists:

- task remains unassigned
- allocation_status is marked degraded
- unresolved_task_count increments

Allocator failure must not stall system execution.

---

## Notes

This contract defines allocator boundary only.

Scoring internals are defined separately.