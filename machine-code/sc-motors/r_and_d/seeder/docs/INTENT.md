# Intent

## Code style
The goal is a testable C++ control system built around:

- small objects with explicit responsibilities
- pure functions for machine decisions and sequencing rules
- thin hardware adapters around the Teknic SDK and physical I/O
- clear separation between machine policy, runtime orchestration, and device control

We want the application logic to be runnable and testable without real motors.

Teknic SDK objects should stay at the adapter boundary. Machine-level behavior should be expressed in our own types, interfaces, and state transitions.

## Testing
Tests should be split into two layers:
- pure functions tested independently
- imperative code tested through virtual ports and fake hardware adapters
