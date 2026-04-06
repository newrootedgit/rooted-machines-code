# Testing

The testing strategy should match the architecture:

- pure logic tested without hardware
- runtime orchestration tested through fake ports
- Teknic adapters tested separately from machine policy
- real hardware tests kept explicit, serialized, and opt-in

## Goals

- prove machine behavior without motors attached
- catch regressions in sequencing and safety logic early
- keep failures local and repeatable
- make hardware tests small and intentional

## Test layers

### 1. Unit

Scope:

- pure functions
- state transitions
- command validation
- effect planning

Examples:

- preset revision handling
- height-change planning
- kill/fault gating decisions
- resume rules

These tests should have no real time, no Teknic SDK, and no filesystem or network access.

### 2. Component

Scope:

- `Harvester`
- `SafetySupervisor`
- machine-specific `SequenceRunner`
- typed motor wrappers

Use:

- fake axes
- fake clock
- fake kill switch
- fake tray sensor
- fake airknife

These tests should verify machine workflows end-to-end inside one process.

Examples:

- apply preset starts a height-change sequence
- kill drops mid-sequence and motion stays blocked
- latest preset revision wins
- fault latch blocks further motion until cleared

### 3. Adapter integration

Scope:

- Teknic adapter behavior
- physical I/O adapter behavior

Verify:

- command translation
- status reads
- fault propagation
- unit conversions

These tests should not also test machine sequencing. Their job is to prove the adapter boundary.

### 4. Hardware

Scope:

- full runtime against a dedicated bench setup

Examples:

- rail move completes and reports observed state correctly
- kill input stops motion and blocks resume
- fault handling transitions to the expected machine state

Hardware tests should be:

- opt-in
- serialized
- safe-by-default on startup and teardown

## Doubles

Prefer fakes over mocks for most ports.

Use fakes for:

- `IVelocityAxis`
- `IPositionAxis`
- `IClock`
- `IKillSwitch`
- `ITraySensor`
- `IAirKnife`

Use mocks only when interaction order is the behavior under test.

Do not mock the Teknic SDK directly in machine tests. Wrap it once behind our own adapter and test against our interfaces.

## What to test where

Unit tests should assert:

- returned state
- returned effects
- revision and sequence transitions

Component tests should assert:

- observable machine behavior
- sequence progress across ticks
- safety blocking and latching

Adapter tests should assert:

- correct SDK calls
- correct status mapping
- correct error mapping

Hardware tests should assert:

- only the minimum real-world contract
- never internal implementation details

## Tooling

Use:

- GoogleTest for test structure
- gMock only where interaction verification is needed
- CTest for discovery, filtering, labeling, and test orchestration
- AddressSanitizer and UndefinedBehaviorSanitizer in host-side test runs

Suggested labels:

- `unit`
- `component`
- `integration`
- `hardware`
- `fuzz`

Hardware tests should use a shared resource lock so they never run concurrently.

## Fuzzing

Add fuzz targets for any parser or untrusted input boundary.

Candidates:

- preset/config parsing
- command parsing
- message framing
- state deserialization

Fuzzing is for robustness, not behavior coverage.

## Execution policy

Run on every change:

- unit
- component

Run regularly in a controlled environment:

- adapter integration

Run on dedicated hardware only:

- hardware

Use sanitizers in normal host-side test runs. Do not require hardware for the default developer test path.

## Layout

```text
tests/
  unit/
  component/
  integration/
  hardware/
  fuzz/
  fakes/
```
