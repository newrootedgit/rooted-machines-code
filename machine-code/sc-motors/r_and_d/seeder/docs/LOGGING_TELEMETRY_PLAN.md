# Logging and Telemetry Plan

## Summary

The old machine logged by having ClearCore assemble ad hoc UDP strings and send them back to the Pi.

That made sense when ClearCore owned most of the machine-adjacent state.

That is not the right design now.

The Pi C++ runtime owns SC motor control directly, so telemetry should be gathered in the same runtime that owns:

- motion commands
- fault state
- input state
- machine state

The new path should be:

- runtime gathers structured telemetry
- runtime writes Pi-side DB records directly
- Vector continues reading from the DB and sending to cloud

The persistence model should be:

- append-only events
- periodic snapshots

Not:

- ad hoc UDP strings
- one giant logger class
- DB writes scattered through `main.cpp`

## Old path vs new path

Old harvester logging preserved useful metrics:

- `boot_id`
- `seq`
- machine uptime
- per-motor uptime
- tray count
- fault flags
- alert bits
- kill state
- command age

Keep those semantics where they still make sense.

Do not keep the transport.

The old ClearCore logging sketch was a transport workaround, not a telemetry architecture.

Now that the runtime talks to the SC nodes directly, we can gather cleaner data from the actual source of truth.

That also means we can log more than the old machine could:

- node serial
- node model
- firmware version
- ready/enabled/moving state
- bus power health
- measured velocity
- fault state from the same code that owns recovery

For load or torque-style data:

- design the model to support it
- do not hardcode a final field name until the exact SC API field is confirmed on the Pi

If the API exposes load percentage or another derived signal instead of true torque, log that honestly.

## Abstraction stack

Do not overload `ILogger`.

The existing `ILogger` placeholder is the wrong shape for structured telemetry persistence.

Keep human-readable logs and structured telemetry separate.

### `MachineEvent`

One-shot records with machine meaning.

Examples:

- `tray_count_incremented`
- `fault_entered`
- `fault_cleared`
- `run_started`
- `run_stopped`

```cpp
struct MachineEvent {
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    const char* type = "";
    std::int64_t value = 0;
};
```

### `MachineSnapshot`

Periodic state record.

This is the current machine health snapshot, not an edge.

`tray_count` belongs here.

That keeps parity with the old periodic status model while still allowing exact increment events.

```cpp
struct MachineSnapshot {
    std::uint64_t boot_id = 0;
    std::uint64_t seq = 0;
    std::uint64_t uptime_ms = 0;

    bool ready_to_run = false;
    bool kill_ok = false;
    bool fault_latched = false;

    int active_variety = -1;
    std::uint64_t tray_count = 0;
    std::uint64_t command_age_ms = 0;
};
```

### `MotorSnapshot`

One plain struct per motor/node.

```cpp
struct MotorSnapshot {
    int node_index = -1;
    const char* role = "";

    int serial = 0;
    const char* model = "";
    const char* firmware = "";

    bool enabled = false;
    bool ready = false;
    bool moving = false;
    bool faulted = false;
    bool bus_power_ok = false;

    double measured_velocity = 0.0;
    std::uint64_t active_uptime_ms = 0;
    std::uint32_t alert_bits = 0;
};
```

### `InputCounter`

Tray count should come from a configured input transition.

That input must remain abstractable because seeder and harvester count trays from different physical inputs.

For v1, keep this seeder-first, but make the primitive generic enough to reuse.

```cpp
struct InputCounterConfig {
    const char* counter_name = "";
    bool count_on_active = true;
};

class InputCounter {
public:
    InputCounter(IInput& input, const InputCounterConfig& config);
    Result refresh();
    std::uint64_t count() const;

private:
    bool initialized_ = false;
    bool last_state_ = false;
};
```

This should not know what a tray is.

It only knows:

- input level
- previous input level
- increment once when the configured transition happens

Machine meaning stays above it.

Do not split this into a separate edge-detector abstraction just for counting.

That is unnecessary surface area.

### `RunTimeTracker`

The old machine manually tracked motor uptimes in the ClearCore sketch.

That should move into one runtime helper.

It should track:

- machine uptime for this boot
- per-motor active uptime for this boot

Tie motor uptime to the actual motor definitions in runtime code, not to object construction.

The right shape is role-based binding:

```cpp
struct MotorBinding {
    const char* role = "";
    int node_index = -1;
    IAxis* axis = nullptr;
};
```

Examples:

- `belt`
- `blade`
- `rail`

`RunTimeTracker` should receive these bindings and accumulate uptime from observed motor state.

Do not start uptime just because the C++ object exists.

Only accrue uptime while the bound motor is actually active, enabled, or running according to the chosen observation rule.

### `MotorTelemetryReader`

Thin SC observation boundary.

It reads motor/node facts and returns `MotorSnapshot`.

It owns no policy.

```cpp
class MotorTelemetryReader {
public:
    explicit MotorTelemetryReader(ClearCoreClient& client);
    MotorSnapshot read_motor(int node_index, const char* role);
};
```

### `ITelemetrySink`

Single persistence boundary.

```cpp
class ITelemetrySink {
public:
    virtual ~ITelemetrySink() = default;
    virtual Result write_event(const MachineEvent& event) = 0;
    virtual Result write_snapshot(const MachineSnapshot& snapshot) = 0;
};
```

### `DbTelemetrySink`

Concrete Pi-side DB writer.

This owns:

- insert logic
- schema details
- batching policy if needed later

The runtime should know nothing about SQL strings or table layout outside this boundary.

### `SeederTelemetryCoordinator`

Seeder-first coordinator.

This is the right place to assemble telemetry from runtime state plus machine semantics.

```cpp
class SeederTelemetryCoordinator {
public:
    SeederTelemetryCoordinator(
        IClock& clock,
        ClearCoreClient& client,
        InputCounter& tray_counter,
        const MotorBinding* motors,
        std::size_t motor_count,
        ITelemetrySink& sink);

    void tick(const SeederState& state);
};
```

This coordinator should:

- emit events immediately on relevant edges
- emit periodic snapshots on a fixed cadence
- own `seq`
- own `boot_id`
- gather `MotorSnapshot` records
- translate generic counters into machine events like `tray_count_incremented`

## How it fits the current runtime

This should fit the existing code with minimal disruption.

### `ClearCoreClient`

Already owns direct SC node access.

It is the right source for:

- node serial
- model
- firmware
- bus power health
- node count / node identity

### `Axis` wrappers

Already expose the beginning of a useful observation surface:

- moving
- enabled
- faulted
- measured velocity concepts

Do not put DB or telemetry policy into the axis classes.

They should stay thin.

### `RuntimeTypes.h` and `SeederState`

These should remain the plain state surface the coordinator reads.

Telemetry should observe `SeederState`, not own it.

### `main.cpp`

`main.cpp` should stay thin.

The intended integration shape is:

```cpp
SFoundationClock clock;
ClearCoreClient client(config);

InputCounter tray_counter(tray_input, {
    .counter_name = "tray_count",
    .count_on_active = true,
});

MotorBinding motors[] = {
    { "belt", 0, &belt },
    { "blade", 1, &blade },
    { "rail", 2, &rail },
};

DbTelemetrySink sink(/* db config */);
SeederTelemetryCoordinator telemetry(
    clock, client, tray_counter, motors, 3, sink);

while (true) {
    // refresh inputs
    // refresh runtime state
    // run machine logic
    telemetry.tick(seeder_state);
    clock.sleep_ms(100);
}
```

That is the right shape.

Do not write telemetry rows directly inside the belt, switch, solenoid, or fault classes.

## Event and snapshot model

The runtime should write two kinds of records.

### Events

Use for edges and state transitions:

- tray counted
- fault entered
- fault cleared
- reset requested
- ready-to-run changed
- run started
- run stopped

### Snapshots

Use for current health and counters:

- uptime
- active variety
- ready state
- fault state
- command age
- tray count
- motor snapshots

That avoids the old problem of mixing event semantics and current-state semantics into one loose string payload.

## V1 metrics

Preserve these old metrics semantically:

- `boot_id`
- `seq`
- `uptime_ms`
- per-motor active uptime
- `tray_count`
- kill/interlock state
- fault flags
- decoded alert bits
- command age if we still track it

Add these SC-native metrics:

- node serial
- model
- firmware
- ready
- enabled
- moving
- bus power health
- measured velocity
- load/torque-style observation only after API verification

## Seeder-first, harvester reuse second

Do not force one generic coordinator now.

That is fake reuse.

The right split is:

- shared value structs later if they survive first implementation
- shared sink boundary if both machines write the same DB shape
- shared input-counter primitive
- machine-specific coordinator per machine

So v1 should be:

- `SeederTelemetryCoordinator`

Later, harvester can add:

- `HarvesterTelemetryCoordinator`

with minimal reuse pain if:

- `ITelemetrySink`
- `DbTelemetrySink`
- `InputCounter`
- `RunTimeTracker`
- `MotorSnapshot`
- `MachineEvent`
- `MachineSnapshot`

are kept clean and machine-agnostic

The tray-count abstraction is the key here.

Seeder and harvester can count from different physical inputs while reusing:

- the same input-counter primitive
- the same persistence sink
- the same event/snapshot model

The machine-specific coordinator is where input meaning stays local.

## Why this is better

This is better than the old machine because:

- telemetry comes from the same runtime that owns control and faults
- transport hacks disappear
- DB persistence is centralized
- event semantics and snapshot semantics are separated
- motor data is more trustworthy in SC mode than it was through old ClearCore-side approximations
- harvester reuse is possible without pretending the two machines are the same

## Validation

The first implementation should prove:

- tray count increments exactly once per configured input edge
- no duplicate count on sustained-high or sustained-low input
- machine uptime is monotonic for the boot
- motor active uptime increases only while the motor is active
- events are written immediately on tray count and fault transitions
- snapshots are written at the configured cadence
- missing SC metrics fail cleanly instead of writing fake values
- harvester can reuse the input-counter + sink + snapshot/event model without changing seeder semantics

## References

- old logging reference: `machine-code/harvester/Auto Adjust Harvester/ClearCore/autoadjust_harvester/autoadjust_harvester_clearcore_square_roots_with_logging_prod.ino`
- current SC client: `runtime/src/utils/ClearCoreClient.cpp`
- current runtime state: `runtime/include/RuntimeTypes.h`
- current machine shell: `runtime/include/seeder/Seeder.h`
- old fault/logging context: `docs/MOTOR_FAULT_HANDLING_PLAN.md`
