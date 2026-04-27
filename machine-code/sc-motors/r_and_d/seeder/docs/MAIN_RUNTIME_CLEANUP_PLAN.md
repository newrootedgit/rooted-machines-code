# Main Runtime Cleanup Plan

## Summary

`main.cpp` should be a small production entrypoint, not the owner of runtime state. It should build the machine config, construct `SeederApp`, call `boot()`, call `run()`, and return an exit code.

The runtime loop belongs in `SeederApp` because that object already owns the hardware client, clock, touch encoder state, telemetry logger, safety supervisor, and axes.

## Target Shape

`main.cpp` should be reduced to roughly:

```cpp
int main() {
    ClearCoreConfig config;
    config.expected_node_count = seeder::machine::kExpectedNodeCount;
    config.enable_timeout_ms = seeder::machine::kClearCoreEnableTimeoutMs;

    SeederApp app(
        config,
        seeder::machine::kTouchEncoderJsonPath,
        seeder::machine::kTelemetryLogPath);

    if (app.boot().code != ResultCode::Ok) {
        return 1;
    }

    return app.run().code == ResultCode::Ok ? 0 : 1;
}
```

`main.cpp` should not know about:

- selected drive axis pointers
- node index selection
- telemetry sequence numbers
- boot IDs
- periodic tickers
- uptime counters
- edge/change tracking
- motor fault transition handling

## SeederApp Runtime Ownership

`SeederApp` should own the production runtime state directly as private members:

- selected drive axis pointer
- selected drive config pointer
- selected drive name
- selected drive node index
- boot ID
- telemetry sequence
- snapshot ticker
- active-drive uptime accumulator
- previous machine state snapshot

`SeederApp` should expose:

```cpp
Result run();
```

The public API should stay small:

- constructor
- `boot()`
- `run()`

## Replace RuntimeContext

`RuntimeContext` should be removed.

It was useful as a temporary grouping while cleaning up `main.cpp`, but it is not a production abstraction. It duplicates what `SeederApp` should already represent: the running seeder application.

The replacement is direct private state on `SeederApp`.

## Replace EdgeTracker

Do not keep `EdgeTracker` in the runtime loop.

Use explicit current and previous machine state structs instead:

```cpp
struct MachineState {
    bool initialized = false;
    bool ready_to_run = false;
    int active_variety = -1;

    int belt_speed = 0;
    int roller_speed = 0;

    bool fault_latched = false;
};
```

Each loop iteration should:

1. Read current TE and safety state into `current_state_`.
2. Compare `current_state_` with `previous_state_`.
3. Emit transition telemetry when values change.
4. Command or stop motion when motion-relevant values change.
5. Assign `current_state_` back to `previous_state_` at the end of the iteration.

This is more obvious than a generic template helper because these are not generic edges; they are specific machine state transitions.

`roller_speed` should be a first-class field, not an alias hidden behind `belt_speed`. The current code still uses `belt_speed` while driving the roller during single-node bring-up; production cleanup should make that explicit.

This means `TouchEncoderState` and `PresetValues` should also gain `roller_speed`, parsed from the TE JSON. If TE JSON does not provide `roller_speed` yet, the fallback should be named explicitly in code as a temporary compatibility path.

## SeederApp Private Helpers

Split `SeederApp::run()` into small private helpers by behavior:

- `select_drive_axis()`
- `emit_boot_event()`
- `latch_drive_faults(const AxisStatus& drive_status)`
- `handle_ready_change(bool ready_to_run, std::uint64_t now_ms)`
- `handle_fault_change(bool fault_latched, std::uint64_t now_ms)`
- `apply_motion_command(bool ready_to_run, int belt_speed, bool fault_latched)`
- `emit_status_snapshot(...)`
- `read_machine_state()`
- `update_previous_state()`

These helpers should stay in `SeederApp.cpp` unless a clear reuse need appears.

## Behavior To Preserve

- Single-node setup maps roller to axis 0.
- Multi-node setup maps belt to node 0 and roller to node 1.
- The active drive is roller when only the roller is initialized.
- The active drive uses its matching `VelocityAxisConfig`.
- Belt motion uses `belt_speed`; roller motion uses `roller_speed`.
- Fault latch remains restart-only for now.
- A latched fault stops the active drive once and blocks future motion commands.
- Ready/fault transition telemetry remains event-based.
- Status telemetry remains periodic.
- Temporary triggered roller test code stays removed.

## Non-Goals

- Do not move axis or input implementation classes into `MachineConfig.h`.
- Do not redesign telemetry schema in this step.
- Do not add fault reset behavior in this step.
- Do not add photoeye production behavior in this step.
- Do not change ClearPath trigger setup in this step.

## Validation

After implementation:

- `main.cpp` should not reference `SCVelocityAxis`, `VelocityAxisConfig`, `EdgeTracker`, `PeriodicTicker`, `UptimeAccumulator`, boot IDs, or telemetry sequence numbers.
- `runtime/include/utils/Helpers.h` can be removed if nothing else uses `EdgeTracker`, `PeriodicTicker`, or `UptimeAccumulator`.
- Build on the Pi with `make -C runtime`.
- Run `./seeder_test` and verify boot, TE-driven motion, fault latch behavior, and periodic telemetry still work.
