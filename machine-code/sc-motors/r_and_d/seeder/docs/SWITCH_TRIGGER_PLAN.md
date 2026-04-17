# Switch-Triggered Solenoid Control Plan

## Intent

The seeder needs two runtime-controlled solenoids on ClearPath-SC node user outputs:

- irrigation valve
- misting valve

These solenoids must not fire from `ready_to_run` edges alone.

They should fire only when a configured switch input changes state in the expected way.

This means the runtime needs a generic digital-input abstraction alongside the planned solenoid abstraction, so the trigger source is an explicit hardware event instead of an inferred software state transition.

## Current runtime shape

The existing seeder runtime already follows the hardware-adapter pattern we want to preserve:

- `IAxis` plus `SCVelocityAxis` in `runtime/include/utils/Axis.h` and `runtime/src/utils/Axis.cpp`
- machine-level constexpr config in `runtime/include/seeder/MachineConfig.h`
- thin wiring in `runtime/src/main.cpp`
- TE JSON parsing in `runtime/include/utils/TouchEncoderState.h` and `runtime/src/utils/TouchEncoderState.cpp`
- a shared time source through `IClock` and `SFoundationClock`

The new switch and solenoid work should follow the same pattern:

- interface
- SC-backed implementation
- machine config constants
- thin runtime wiring

## Trigger policy

The only valid trigger for irrigation and misting pulses is a switch event.

Not allowed:

- `ready_to_run` false to true as the firing event
- active-variety change as the firing event
- any TE JSON refresh/change as the firing event
- periodic re-arming while the machine is running

Allowed:

- a configured switch edge from a physical digital input

`ready_to_run` should remain a gate, not a trigger.

That means:

- if the machine is not `ready_to_run`, ignore switch triggers and keep both solenoids off
- if the machine drops out of `ready_to_run` during a pulse, cancel the pulse immediately
- if the configured switch edge occurs while `ready_to_run` is true, arm the solenoid pulses once

## Proposed abstractions

### 1. `IInput`

Add a reusable digital-input port.

Suggested location:

- `runtime/include/utils/Input.h`
- `runtime/src/utils/Input.cpp`

Suggested shape:

```cpp
class IInput {
public:
    virtual ~IInput() = default;
    virtual Result refresh() = 0;
    virtual bool is_active() const = 0;
};

struct InputConfig {
    int node_index = 0;
    int input_index = 0;
    bool active_high = true;
};

class SCInput : public IInput {
public:
    SCInput(sFnd::INode& node, const InputConfig& config);

    Result refresh() override;
    bool is_active() const override;

private:
    sFnd::INode& node_;
    InputConfig config_;
    bool state_ = false;
};
```

Purpose:

- isolate the exact sFoundation input-read API
- keep input polarity in config instead of in `main.cpp`
- support future sensors with the same runtime pattern

The trigger switch is then just one configured use of `IInput`, not a special port type.

### 2. `TriggerInput`

Do not put trigger semantics inside `SCInput`.

Keep the hardware adapter responsible only for reading the current level.

Add a small runtime helper that remembers the previous input state and reports when the configured trigger transition happens.

Suggested location:

- `runtime/include/utils/TriggerInput.h`
- `runtime/src/utils/TriggerInput.cpp`

Suggested shape:

```cpp
class TriggerInput {
public:
    TriggerInput(IInput& input, bool trigger_on_active);
    Result refresh();
    bool triggered() const;
    bool is_active() const;

private:
    IInput& input_;
    bool trigger_on_active_ = true;
    bool initialized_ = false;
    bool last_state_ = false;
    bool triggered_ = false;
};
```

Purpose:

- produce one-shot trigger events cleanly
- keep trigger semantics testable without hardware
- avoid a separate generic edge-detector abstraction when this use case only needs one configured transition

### 3. `ISolenoid`

Add the previously proposed output abstraction.

Suggested location:

- `runtime/include/utils/Solenoid.h`
- `runtime/src/utils/Solenoid.cpp`

Suggested shape:

```cpp
class ISolenoid {
public:
    virtual ~ISolenoid() = default;
    virtual Result on() = 0;
    virtual Result off() = 0;
    virtual bool is_on() const = 0;
};

struct SolenoidConfig {
    int node_index = 0;
    int output_index = 0;
    bool active_high = true;
};

class SCSolenoid : public ISolenoid {
public:
    SCSolenoid(sFnd::INode& node, const SolenoidConfig& config);
    Result on() override;
    Result off() override;
    bool is_on() const override;

private:
    sFnd::INode& node_;
    SolenoidConfig config_;
    bool state_ = false;
};
```

Purpose:

- isolate the exact sFoundation user-output write call
- keep output polarity and node routing in machine config
- let pulse sequencing depend on an interface, not the SDK

### 4. `PulseController`

Keep the pulse logic non-blocking and clock-driven.

Suggested location:

- `runtime/include/utils/PulseController.h`
- `runtime/src/utils/PulseController.cpp`

Suggested shape:

```cpp
class PulseController {
public:
    PulseController(ISolenoid& solenoid, IClock& clock);

    void arm(int delay_ms, int duration_ms);
    void tick();
    void cancel();
    bool busy() const;

private:
    enum class State { Idle, Delaying, Firing };

    ISolenoid& solenoid_;
    IClock& clock_;
    State state_ = State::Idle;
    std::uint64_t delay_end_ms_ = 0;
    std::uint64_t fire_end_ms_ = 0;
};
```

Purpose:

- schedule delayed one-shot pulses
- keep the main loop single-threaded
- allow irrigation and misting to run independently

## Trigger composition in `main.cpp`

The runtime loop should become:

1. Refresh TE state.
2. Refresh switch input state.
3. Refresh trigger input state.
4. If `ready_to_run` is false, cancel pulses and force outputs off.
5. If the configured trigger transition is present while `ready_to_run` is true, arm irrigation and misting once.
6. Tick both pulse controllers every loop iteration.

Illustrative shape:

```cpp
const bool ready = te_state.ready_to_run();

trigger_input.refresh();

if (!ready) {
    irrigation.cancel();
    misting.cancel();
} else if (trigger_input.triggered()) {
    irrigation.arm(te_state.irrigation_delay(), te_state.irrigation_duration());
    misting.arm(te_state.misting_delay(), te_state.misting_duration());
}

irrigation.tick();
misting.tick();
```

The trigger transition should be configurable or at least explicitly chosen in code once the sensor is known.

If the physical switch is normally-closed or the sensor asserts low when blocked, the trigger may need to be "becomes inactive" instead of "becomes active".

## TE JSON state

The active-variety TE JSON still appears to be the right place for timing values.

Extend `PresetValues` in `runtime/include/RuntimeTypes.h` with:

```cpp
int irrigation_delay = 0;
int irrigation_duration = 0;
int misting_delay = 0;
int misting_duration = 0;
```

Update `TouchEncoderState` to parse those from the active-variety object, matching how `belt_speed` is handled today.

These fields remain configuration only.

They do not trigger anything by themselves.

## Machine config additions

Add placeholder configs in `runtime/include/seeder/MachineConfig.h`.

Suggested additions:

```cpp
inline constexpr InputConfig kTriggerInput {
    /*node_index=*/0,
    /*input_index=*/0,
    /*active_high=*/true,
};

inline constexpr SolenoidConfig kIrrigation {
    /*node_index=*/0,
    /*output_index=*/0,
    /*active_high=*/true,
};

inline constexpr SolenoidConfig kMisting {
    /*node_index=*/0,
    /*output_index=*/1,
    /*active_high=*/true,
};
```

These are placeholders until wiring is confirmed on the machine.

## Expected file touches when implementation starts

- `runtime/include/utils/Input.h`
- `runtime/src/utils/Input.cpp`
- `runtime/include/utils/TriggerInput.h`
- `runtime/src/utils/TriggerInput.cpp`
- `runtime/include/utils/Solenoid.h`
- `runtime/src/utils/Solenoid.cpp`
- `runtime/include/utils/PulseController.h`
- `runtime/src/utils/PulseController.cpp`
- `runtime/include/RuntimeTypes.h`
- `runtime/include/utils/TouchEncoderState.h`
- `runtime/src/utils/TouchEncoderState.cpp`
- `runtime/include/seeder/MachineConfig.h`
- `runtime/src/main.cpp`
- `runtime/Makefile`

## Suggested implementation order

1. Confirm the exact sFoundation input-read API for SC node user inputs.
2. Confirm the exact sFoundation output-write API for SC node user outputs.
3. Add `IInput` and `SCInput`.
4. Add `TriggerInput`.
5. Add `ISolenoid` and `SCSolenoid`.
6. Add `PulseController`.
7. Extend TE parsing for irrigation and misting timing.
8. Add machine config constants for one trigger switch and two solenoids.
9. Wire the runtime loop so switch edge drives pulse arming and `ready_to_run` only gates behavior.
10. Bench-test with the actual machine wiring.

## Verification plan

### Host-side

Add small tests around pure logic boundaries:

- `TriggerInput` reports one trigger only once per configured transition
- `TriggerInput` does not retrigger while the input remains steady
- `PulseController` waits for delay, fires for duration, then turns off
- `PulseController::cancel()` turns the solenoid off immediately
- switch activity while not ready does not arm either pulse

### Bench

With the seeder on the bench:

1. Set TE values for irrigation and misting delay and duration.
2. Hold `ready_to_run=0` and actuate the trigger switch.
3. Confirm neither user output changes.
4. Set `ready_to_run=1`.
5. Actuate the trigger switch once.
6. Confirm irrigation and misting outputs pulse according to their configured timing.
7. Hold the switch active and confirm pulses do not continuously retrigger.
8. Toggle the switch inactive and active again and confirm a second pulse sequence occurs only on the configured edge.
9. Drop `ready_to_run` during an active pulse and confirm both outputs drop immediately.

Use a scope or multimeter on the SC node user output terminals to verify real electrical output, not just logs.

## Open decisions before implementation

These need explicit confirmation:

1. Which physical switch is the trigger source?
2. Which node input and input index is that switch wired to?
3. Is the trigger edge rising or falling?
4. Should repeated switch hits while a pulse is already busy be ignored or restart the timing?
5. If irrigation and misting are both armed from the same trigger, should they always fire together or should each have an independent enable flag in TE data?
6. What is the exact sFoundation API for SC user inputs and outputs on this target build?

## Recommended default decisions

Unless hardware testing says otherwise:

- use one dedicated trigger switch
- use one-shot edge semantics
- ignore retriggers while a pulse is already busy
- treat `ready_to_run` as a safety gate only
- keep all timing values in TE JSON per variety
- keep input and solenoid adapters thin and machine-agnostic

## Hardware note

Do not assume the SC output can drive the solenoid coil directly.

- `CS2-024DC` is `24V`, `24W`, about `1.0A`
- `CS1-024DC` is `24V`, `13W`, about `0.54A`

If the available SC output is limited to about `500mA`, both coils should be treated as external loads and switched through a relay or MOSFET driver.

The relay does not increase power. It lets a small control output switch a separate 24V power path to the solenoid.

- `V` is voltage: electrical pressure
- `A` is current: electrical flow
- `W = V * A`

The design assumption should be:

- runtime controls a low-power output
- external switching hardware drives the valve coil
