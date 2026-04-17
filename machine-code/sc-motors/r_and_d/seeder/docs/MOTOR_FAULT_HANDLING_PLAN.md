# Motor Fault Handling Plan

## Intent

Add a clear, testable motor fault-handling policy for the seeder runtime before adding more machine behaviors.

This document is about runtime behavior and recovery policy, not low-level implementation details.

## Why this needs to exist

The current runtime detects some motor problems, but only opportunistically:

- `SCVelocityAxis` and `SCPositionAxis` fail a command if `AlertPresent` is already set
- `ClearCoreClient::enable_node()` clears alerts and stop state once during startup, then enables the node
- there is no runtime-level fault latch
- there is no classification of fault types
- there is no explicit recovery flow
- there is no single place that decides what should happen after a node faults

That is enough for bring-up, but not enough for a machine that needs deterministic stop and recovery behavior.

## Best-practice goals

For this runtime, fault handling should:

- stop motion quickly and predictably
- prevent automatic restart after a fault
- separate hardware safety from software recovery policy
- preserve enough fault context to debug the problem
- require explicit operator reset before motion resumes
- be testable without real hardware

## Vendor and architecture guidance

The existing architecture docs already point in the right direction:

- `SafetySupervisor` should own fault gating and latching
- hardware safety should remain outside software
- the Pi runtime remains the machine-state authority

Teknic guidance reinforces the same split:

- SC status exposes alert and readiness state
- faults/alerts can be cleared explicitly
- hardware stop and enable paths should remain independent of normal software sequencing
- the SC platform has a dedicated Global Stop / safety concept distinct from normal motion commands

The runtime should therefore treat motor faults as a latched machine-state condition, not just as an error return from one motion API call.

## Recommended model

Handle faults at two layers.

### 1. Hardware safety layer

This layer is not negotiated by the runtime.

Examples:

- E-stop / kill chain
- hardware enable drop
- ClearCore heartbeat loss dropping SC enable
- external interlocks that remove motor power

Policy:

- hardware safety should win immediately
- software should observe and reflect it
- software should not attempt to fight or mask it

### 2. Runtime fault layer

This is the software policy layer.

Examples:

- motor alert present
- bus power low
- node not ready when it should be
- command timeout
- communications failure with SC hub

Policy:

- detect
- stop
- latch
- block further motion
- require explicit reset path

## Fault classes

Do not treat every fault the same internally.

Use at least these buckets:

### A. Safety-stop faults

These mean the machine must be considered stopped immediately.

Examples:

- kill/E-stop active
- heartbeat lost and enable dropped
- hardware interlock opened

Runtime behavior:

- stop sequences
- cancel pulses and outputs
- mark fault latched
- block all new motion until operator reset and safety chain is healthy again

### B. Motor-drive faults

These come from the SC node itself.

Examples:

- `AlertPresent`
- node not ready after enable
- shutdown/fault indicated by LED / SC alert state
- persistent motion failure

Runtime behavior:

- stop affected motion immediately
- treat the machine as faulted, not just that single axis
- latch fault context with node index and summary reason

### C. Infrastructure faults

These mean the runtime cannot trust the control path.

Examples:

- SC hub comms loss
- wrong node count after reconnect
- sFoundation exceptions during status refresh
- bus power low

Runtime behavior:

- move to machine-faulted state
- do not auto-resume on transient reconnect without explicit reset

## Core design rules

### 1. Faults must latch

Once a motor fault is detected, the runtime should enter a latched fault state.

That state should survive:

- TE JSON changes
- active-variety changes
- `ready_to_run` toggles
- switch input activity

The latch should clear only through an explicit operator reset flow after the cause is gone.

### 2. Do not auto-clear alerts during normal runtime

Clearing motor alerts should be part of an intentional recovery step, not something done automatically every loop.

Reason:

- auto-clear hides intermittent faults
- auto-clear can create surprise restarts
- auto-clear makes debugging much harder

Startup enable can still perform a clear as part of controlled initialization. Runtime recovery should be explicit and logged.

### 3. Stop first, diagnose second

On first detection of a fault:

- stop commanded motion
- cancel active sequences
- force outputs to safe state
- record context

Only after the machine is in a safe software state should the runtime worry about reporting detail.

### 4. Recovery must be explicit

Recommended recovery flow:

1. Underlying fault condition goes away.
2. Operator issues reset/clear command.
3. Runtime verifies safety chain is healthy.
4. Runtime clears node alerts and stop condition.
5. Runtime re-enables node(s).
6. Runtime confirms nodes are ready.
7. Runtime clears software fault latch.
8. Operator must command motion again.

Do not resume motion automatically just because reset succeeded.

### 5. Prefer machine-wide block over partial auto-recovery

For the seeder, the safest default is:

- any motor fault blocks the whole machine

That is simpler and safer than trying to keep some subsystems alive while one axis is faulted.

If later we want axis-scoped degradation, that should be a deliberate design change, not the default.

### 6. Poll for status, do not rely only on command failures

A node can fault between commands.

So the runtime should check fault-related status regularly in the main loop, not only when a move is attempted.

At minimum, each loop should observe:

- node ready state
- alert present state
- bus power state

If any read fails unexpectedly, treat that as an infrastructure fault.

## Proposed runtime state additions

The runtime should carry enough fault state to make decisions and print useful diagnostics.

Suggested shape:

- `fault_latched`
- `fault_active`
- `fault_source`
- `fault_code` or `fault_reason`
- `faulted_node_index`
- `fault_first_seen_ms`
- `fault_last_seen_ms`
- `requires_operator_reset`

The exact types can be decided at implementation time.

The important part is that fault state is explicit and persists long enough to debug.

## Recommended behavior in the main loop

Each iteration should conceptually do this:

1. Refresh machine inputs and TE state.
2. Refresh motor status for every node.
3. Update the safety supervisor with observed faults and safety inputs.
4. If faulted:
   - stop/cancel anything in flight
   - suppress new commands
   - hold outputs safe
5. If not faulted:
   - allow normal sequencing and motion

This puts fault policy in one place instead of scattering it across motor wrappers and `main.cpp`.

## What should trigger a latch immediately

These should all be treated as immediate latched faults by default:

- any node reports `AlertPresent`
- any required node fails to become ready during enable
- any command returns timeout
- bus power low on any required node
- any sFoundation exception during active runtime status read
- SC hub disappears or node count changes unexpectedly
- ClearCore reports heartbeat lost / hardware safety drop

## What should not clear the latch

These should not clear a fault latch by themselves:

- `ready_to_run = 0`
- `ready_to_run = 1`
- TE JSON rewriting values
- active variety changing
- physical trigger switch toggling
- operator fixing the hardware without issuing reset
- transient disappearance of the alert bit without an explicit recovery sequence

## Logging and observability

On fault, log one concise first-fault line with:

- timestamp
- node index
- category
- reason summary
- relevant status bits if available

Then avoid spamming the same log every loop.

Also log:

- reset requested
- reset rejected and why
- reset succeeded
- node re-enabled and ready

The first version can be plain `printf`.

## Suggested implementation shape

Keep the same architectural pattern already established in the docs.

### `SafetySupervisor`

This should become the owner of:

- fault latch state
- safety gate state
- reset eligibility
- decision about whether motion is allowed

### Axis wrappers

Keep `SCVelocityAxis` / `SCPositionAxis` thin.

They should:

- expose status cleanly
- fail commands cleanly
- not own machine recovery policy

### `main.cpp`

Keep wiring thin.

It should:

- collect observations
- feed them into supervisor/runtime
- obey the supervisor's allow/block decision

## Recovery policy recommendation

Use this default unless bench testing proves it is too strict:

- any motor alert latches the machine fault
- machine motion is blocked until explicit reset
- reset is rejected while safety chain is unhealthy
- reset clears alerts only once, intentionally
- reset does not auto-restart motion

This is conservative, but it is the right default for a machine that can move unexpectedly.

## Test plan

### Host-side tests

Add tests for:

- fault latch sets on first detected motor alert
- latch blocks new commands
- `ready_to_run` changes do not clear latch
- reset is rejected while fault cause remains present
- reset clears latch only after fault cause is absent
- reset does not auto-command motion after success
- repeated fault observation does not spam state transitions

### Bench tests

Validate at least:

1. Induce a motor alert.
2. Confirm motion stops and machine becomes fault-latched.
3. Confirm TE changes and trigger inputs do not restart motion.
4. Remove the fault cause.
5. Issue reset.
6. Confirm node clears and re-enables.
7. Confirm machine remains idle until a fresh start command.

Also test:

- bus power low behavior
- unplug/replug comms behavior
- ClearCore heartbeat-loss path if that safety path is active

## Open questions before implementation

1. What explicit operator action should map to reset?
2. Do we want one reset for all machine faults, or separate reset semantics for safety vs motor vs I/O faults?
3. What exact SC status bits do we want to surface beyond `AlertPresent` and `InBusLoss`?
4. Should the first implementation disable all nodes on runtime fault, or just stop and block commands?
5. Do we want fault history beyond the current active/latched fault?

## Recommended next step

Implement fault handling in this order:

1. define explicit fault state types
2. make `SafetySupervisor` real
3. add periodic node health polling
4. latch and block on faults
5. add explicit reset flow
6. add host-side tests
7. bench-test with induced faults

## Comparison to old harvester ClearCore fault handling

The older ClearCore harvester code is useful as a reference, but it is not a fault architecture worth copying.

What it gets right:

- checks fault state before motion and during motion
- prints specific alert bits instead of only a generic fault flag
- distinguishes between "alerts present" and "motor faulted" in some handlers
- reacts to kill input changes quickly

What it gets wrong:

- fault handling is scattered across per-motor functions
- recovery is mostly "cycle enable and clear alerts immediately"
- there is no real machine-level fault latch
- there is no explicit operator-reset gate
- there is no clean separation between detection, policy, and recovery
- it is easy to clear the symptom and continue without proving the cause is gone

That pattern is fine for examples and bench bring-up. It is not strong enough for a machine runtime that is supposed to behave predictably after a real fault.

This seeder plan is better because it treats faults as machine-state, latches them, blocks automatic restart, and puts recovery policy in one place.

The one thing worth preserving from the old harvester code is detailed fault reporting. The seeder implementation should decode and surface specific alert bits where the SC API allows it, rather than collapsing everything into a single generic fault message.

## Sources consulted

- Teknic ClearPath-SC User Manual: https://teknic.com/files/downloads/Clearpath-SC%20User%20Manual.pdf
- Seeder architecture doc: [ARCH.md](/Users/VishalVunnam/Desktop/Rooted/rooted-machines-code/machine-code/sc-motors/r_and_d/seeder/docs/ARCH.md)
- Seeder ClearCore doc: [CLEARCORE.md](/Users/VishalVunnam/Desktop/Rooted/rooted-machines-code/machine-code/sc-motors/r_and_d/seeder/docs/CLEARCORE.md)
- Current runtime wiring: [main.cpp](/Users/VishalVunnam/Desktop/Rooted/rooted-machines-code/machine-code/sc-motors/r_and_d/seeder/runtime/src/main.cpp)
- Current motor adapter behavior: [Axis.cpp](/Users/VishalVunnam/Desktop/Rooted/rooted-machines-code/machine-code/sc-motors/r_and_d/seeder/runtime/src/utils/Axis.cpp)
- Current startup enable path: [ClearCoreClient.cpp](/Users/VishalVunnam/Desktop/Rooted/rooted-machines-code/machine-code/sc-motors/r_and_d/seeder/runtime/src/utils/ClearCoreClient.cpp)
