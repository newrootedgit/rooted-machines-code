# Runtime Simplification

## Why

`runtime/` is ~1900 LOC across 25 files for a control loop that reads a JSON file, commands 1-2 motors, reacts to a photoeye, pulses a solenoid, and appends JSONL. The shape of the code does not match the size of the problem.

`runtime/src/main.cpp:33` already concedes the point ("I know it seems more complicated than it needs to be"). The cleanup formalizes that admission.

Two things drove the over-engineering:

1. **Testability scaffolding with no tests.** `IClock`, `IInput`, `ITelemetrySink`, `IAxis` exist to allow mocking. There is no `tests/` directory. The interfaces are pure cost.
2. **Generality that never landed.** `SCPositionAxis` is fully stubbed (`runtime/src/utils/Axis.cpp:97-147`). `SafetyState.kill_ok` is wired but never set false. The single-vs-multi-node case is handled via `std::optional` on every actuator instead of one boot-time branch.

The cleanup also dovetails with the on-node trigger work in [sensor-triggered-motion.md](sensor-triggered-motion.md): that change removes `update_roller_linger`, the `photoeye_edge` gate, the host-side `SCNodeInput` read path, and the `previous_blocked_` state. Doing the cleanup *after* that change avoids rewriting the same code twice.

## Scope

Mechanical refactor only. No behavior change, no new features. Same JSONL output, same Touch Encoder contract, same SDK calls.

## What goes

### Interfaces with one implementation
- `runtime/include/utils/IClock.h` — `SeederApp` calls `clock_.now_ms()` and `clock_.sleep_ms(100)`. Replace with `sFnd::SysManager::TimeStampMsec()` direct and a `usleep(100'000)` in the main loop.
- `runtime/include/utils/IInput.h` (the `IInput` part) — only `SCNodeInput` implements it.
- `runtime/include/telemetry/ITelemetrySink.h` — only `JsonlTelemetryLogger` implements it.
- `IAxis` (referenced only by `MotorBinding`, which is itself unused).

### Dead or near-dead classes
- `SCPositionAxis` (`runtime/src/utils/Axis.cpp:97-147`, `runtime/include/utils/Axis.h`) — both methods return "not implemented".
- `SafetySupervisor` (`runtime/src/utils/SafetySupervisor.cpp`, `runtime/include/utils/SafetySupervisor.h`) — wraps two bools and a `const char*`. Inline into `SeederApp`: a `bool fault_latched_` and a `const char* fault_reason_`. `kill_ok` is always true; delete it.
- `MotorBinding`, `SeederTelemetryInputs` (`runtime/include/telemetry/TelemetryTypes.h:78-90`) — unused.

### Layer collapses
- Merge `SCNodeInput` and `Photoeye`. After the sensor-triggered-motion change, the host no longer reads Input-A for motion gating, so the only remaining consumer is diagnostic. One small `Photoeye` class that pokes `node.Status.RT.Value().cpm.InA` directly is enough; the `IInput` indirection and the edge-count baseline machinery in `Input.cpp` go away.
- Drop the `MachineState` struct + `previous_state_` diff in `SeederApp` (`runtime/include/seeder/SeederApp.h:31-38`, `SeederApp.cpp:159-168`). The actual diff is 4 fields (`ready_to_run`, `active_variety`, `belt_speed`, `roller_speed`); keep them as four members, not a struct that has to be recomputed and compared each tick.

### Method explosion in `SeederApp`
`SeederApp` has 20 private methods. After the sensor-triggered-motion change removes `update_roller_linger`, `roller_uptime_active`, `roller_should_run`, and the `photoeye_edge` block, several of the remaining helpers are one-call wrappers. Fold these back into `run_once()`:
- `read_machine_state`, `update_previous_state` — gone with the struct.
- `belt_active`, `roller_active` — one-line predicates, inline.
- `stop_axis`, `stop_all_axes` — inline; `axis->stop()` is already one call.
- `handle_ready_change`, `handle_fault_change` — each is a single edge-detect + telemetry write. Inline.

`apply_motion_command`, `apply_axis_motion`, `latch_axis_faults`, `emit_status_snapshot`, `update_solenoid_pulse` stay as methods — they have real bodies and are called from more than one place or are long enough to be worth a name.

## What stays

- `ClearCoreClient` — earns its keep as the SDK boilerplate wrapper (port discovery, node enable, bus-power check). The only thing it owns that other code shouldn't.
- `SCVelocityAxis` — thin RPM-command adapter over `sFnd::IMotion`. The clamp + unit setup is genuinely worth a class.
- `SCBrakeOutput` — three lines, but the `GPO_ON / GPO_OFF` modes are non-obvious and the name carries intent. Keep.
- `MachineConfig.h` — single place for tunables. Untouched by this refactor; updated separately by sensor-triggered-motion.
- `JsonlTelemetryLogger` and the `MachineEvent` / `MachineSnapshot` / `MotorSnapshot` structs. The JSONL schema is a contract with downstream consumers; collapsing the structs into ad-hoc `fprintf` calls would put that contract at risk. Drop the `ITelemetrySink` interface, keep the concrete logger and its types.
- `TouchEncoderState` — file-locked JSON read is non-trivial; the encapsulation is doing real work.
- `Result` / `ResultCode` — consistent error path across the runtime. Keeping it is cheaper than rewriting every call site.

## Expected outcome

- 25 files → ~14 files.
- ~1900 LOC → ~1100 LOC (roughly 40%).
- `SeederApp.h` private surface drops from 20 methods to ~7.
- `main.cpp` loses the `IClock` indirection comment and shrinks to ~20 lines.

## Sequencing

1. Land [sensor-triggered-motion.md](sensor-triggered-motion.md) first. It removes the linger machinery, the host photoeye read path, and `previous_blocked_` — roughly 80 LOC of what this cleanup would otherwise have to touch.
2. Then this cleanup as **one PR**, no behavior change. Diff is large but mechanical: deletions, header consolidations, inlines. Reviewable by reading the new `SeederApp.cpp` end-to-end and confirming the JSONL output is byte-identical against a captured pre-cleanup log.
3. No deprecation shims, no "old + new in parallel." The interfaces have one caller each; rip them out.

## Non-goals

- Not rewriting `ClearCoreClient` or the axis classes.
- Not changing the JSONL schema.
- Not introducing tests as part of this PR. (If tests come later, they go in against the simpler surface, not the current one.)
- Not touching `third_party/` or `scripts/`.
