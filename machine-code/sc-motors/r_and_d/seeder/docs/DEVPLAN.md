# Development Plan

Build the system in small stages.

Do not connect to real machines until the runtime shape, test harness, and adapter boundaries are stable.

## Phase 1: Foundation

Goal:

- create the C++ project layout
- choose build and test tooling
- make the code compile locally
- prove the Teknic toolchain works on the target Pi before any runtime code is written

Deliverables:

- source tree
- build system
- test runner
- fake ports and basic test utilities
- Exar USB serial kernel driver built and loaded on the target Pi OS
- sFoundation cross-compiled or built for the Pi
- a minimal smoke binary that opens the SC Hub and moves one motor

Exit criteria:

- clean local build
- unit and component tests run without hardware
- on a fresh target Pi with the Exar driver loaded, sFoundation enumerates one SC motor through the SC Hub and round-trips a `MoveVelocity` command and a position read

If the driver does not build or load on the current Pi OS kernel, stop and resolve before Phase 2. Options at that point are: patch the driver, pin to an older kernel (with OTA implications), or change host platform.

## Phase 2: Core runtime skeleton

Goal:

- introduce `Harvester`
- introduce `SafetySupervisor`
- introduce machine-specific `SequenceRunner`
- define state, command, and effect types

Deliverables:

- runtime loop skeleton
- command submission path
- tick path
- basic sequencing state machine

Exit criteria:

- component tests prove preset revision handling
- component tests prove kill and fault gating

## Phase 3: Motor abstraction layer

Goal:

- define shared axis interfaces
- add thin typed wrappers for belt, blade, and rail
- keep Teknic out of machine logic

Deliverables:

- `IAxis`, `IVelocityAxis`, `IPositionAxis`
- `BeltMotor`, `BladeMotor`, `RailMotor`
- fake axis implementations for tests

Exit criteria:

- machine tests run entirely against fakes
- no Teknic types leak into machine-level code

## Phase 4: Preset application flow

Goal:

- add desired preset/config input
- track revisioned desired state
- apply latest-wins semantics

Deliverables:

- desired vs applied revision tracking
- preset-to-sequence handoff
- sequence completion updates

Exit criteria:

- tests prove latest revision wins
- tests prove applied revision is updated only after successful completion

## Phase 5: ClearCore boundary

Goal:

- define the Pi/ClearCore protocol
- simulate ClearCore events before hardware integration

Deliverables:

- command/event/status types
- fake ClearCore adapter
- event handling in `Harvester`

Exit criteria:

- tests prove kill, tray, and airknife events are handled correctly
- no machine logic depends on real ClearCore hardware

## Phase 6: Teknic adapter bring-up

Goal:

- implement the SC adapter boundary
- validate command and status mapping without full machine integration

Deliverables:

- `TeknicAxis` adapter
- adapter integration tests
- unit conversion and error mapping checks

Exit criteria:

- adapter tests pass
- runtime still passes host-side tests without hardware

## Phase 7: Bench integration

Goal:

- connect to real hardware gradually
- validate one subsystem at a time

Order:

- rail only
- belt only
- blade only
- ClearCore I/O only
- full machine sequence

Exit criteria:

- each subsystem works in isolation first
- full sequence tests pass on the bench

## Rules

- start with host-side tests
- add hardware only after the surrounding layer is stable
- keep one runtime owner of machine state
- test every new boundary before composing the next one
