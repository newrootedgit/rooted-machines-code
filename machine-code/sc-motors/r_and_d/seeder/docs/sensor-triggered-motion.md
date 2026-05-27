# Sensor-Triggered Motion (on-node)

## Why

Today the Pi reads the photoeye level, computes the trigger, and commands motion. Two known bugs result: (1) trigger doesn't refire reliably after the first fire, (2) no per-fire delay available. Both come from doing edge detection + command dispatch on the host. ClearView also exposes no trigger-delay knob because the delay lives in the on-node PLA, not the input page.

The fix: queue *triggered* moves and let the SC node fire them itself when Input-A asserts, via its PLA. Host involvement drops to "queue the next move" and "occasionally rewrite the delay parameter."

Existing code that needs to change is rooted at `runtime/src/utils/SCNodeInput.cpp:17` — that comment documents why Input-A was *removed* from Move Trigger mode (so the host could read its level). Going back to on-node triggering reverses that decision.

## Library

All references are to the Teknic sFoundation SDK vendored at
`/machine-code/sc-motors/r_and_d/harvester/pi/teknic/Linux_Software/sFoundation/`.

## Pieces used

### Triggered move APIs (host queues, node fires)
- `sFnd::IMotion::MoveVelStart(target, isTriggered=true)` — `inc/inc-pub/pubSysCls.h:5771`
- `sFnd::IMotion::MovePosnStart(...)` — `pubSysCls.h:5666`
- `sFnd::IMotionAdv::MovePosnStart / MovePosnHeadTailStart / MovePosnAsymStart / MoveVelStart` — `inc/inc-pub/pubCpmCls.h:456-473`
- Move buffer depth: up to 16, gated by `cpmStatusRegFlds::MoveBufAvail` (`pubSysCls.h:5660`).

Roller path will use `MoveVelStart(rpm, true)` so each photoeye edge drives one velocity segment; meter index moves can use `MovePosnAsymStart(..., true, ...)`.

### Software trigger entry points (kept as fallback / e-stop)
- `CPMmotionAdv::TriggerMove()` — `pubCpmCls.h:476`
- `CPMmotionAdv::TriggerMovesInMyGroup()` — `pubCpmCls.h:477`
- `CPMmotionAdv::TriggerGroup(n)` / overload — `pubCpmCls.h:478-479`
- `IPortAdv::TriggerMovesInGroup(n)` — `pubCpmCls.h:909`

These are *not* the production trigger path — they exist if we ever want the host to inject a manual fire.

### PLA (configured once in ClearView, runtime parameters writable)
PLA output bits — `inc/inc-pub/pubIscRegs.h`:
- `ISC_PLAOUT_TRIGGER_BIT` (line 321) — fires any queued `isTriggered=true` move
- `ISC_PLAOUT_RESET_TIMER_BIT` (line 333) — starts/restarts the GP timer

PLA wiring in ClearView:
```
Input-A rising edge  → ResetTimer
TimerExpired         → Trigger
```

### General-purpose timer (the trigger-delay knob)
- Period parameter: `ISC_P_GP_TIMER_PERIOD` — `pubIscRegs.h:113` (CPM equivalent `CPM_P_GP_TIMER` — `pubCpmRegs.h:111`)
- Status bit `TimerExpired` — `pubCpmRegs.h:1467`
- Writable from the host via the standard node parameter accessors in `pubCpmAPI.h` / `pubSysCls.h` (same path we already use for vel/acc limits).

Host writes `GP_TIMER_PERIOD = clamp(scale / belt_speed, min, max)` only on TE `belt_speed` change — low frequency, off the per-tray hot path. Only one GP timer per node, so it serves either the fire-delay OR pulse-width role, not both.

### Input status (for telemetry / diagnostics, not gating)
- `InA` — `pubCpmRegs.h:1366`
- `InB` — `pubCpmRegs.h:1375`
- `InvInA` / `InvInB` — `pubCpmRegs.h:1384, 1393`

With Input-A back in Move-Trigger mode the host can still observe attention-driven edges; it just shouldn't gate motion off them.

### Alternative: position-triggered moves
If the meter index needs to land at an exact belt position (not "N ms after edge"):
- `ISC_P_POSN_TRIG_PT` — `pubIscRegs.h:126, 1200` (At-Position trigger point)
- Move styles `MG_MOVE_STYLE_NORM_TRIG_ABS`, `..._ASYM_TRIG_ABS`, `..._TAIL_TRIG_ABS`, `..._HEAD_TRIG_ABS`, `..._HEADTAIL_TRIG_ABS` — `inc/inc-pub/pubMotion.h:94-98`
- Command `ISC_CMD_MOVE_POSN_ABS_TRIG` — `pubIscRegs.h:2824`

Position-triggered moves auto-compensate for belt speed variation; defer until the time-based path is proven.

## Solenoid

SC-Hub brake terminals (where the solenoid is wired) are *not* PLA-routable. Only modes: `BRAKE_AUTOCONTROL`, `GPO_ON`, `GPO_OFF` (`inc/inc-pub/pubNetAPI.h:303-326`), driven by `IBrakeControl::BrakeSetting()` — `pubCpmCls.h:822`. The Hub has no separate PLA-driven GPO pins.

Therefore solenoid stays host-pulsed exactly as `SeederApp::update_solenoid_pulse` does today (`runtime/src/seeder/SeederApp.cpp:496`). To keep determinism where it matters (seed delivery), the photoeye is mounted upstream by enough distance that the solenoid fires well before the tray reaches the meter — host-side jitter falls inside the wet-zone window and is mechanically invisible. The roller motion, which *does* require precise timing, is the deterministic on-node path.

## Constraint to size for

Photoeye-to-meter distance sets a **minimum belt speed**: below it, the required GP timer delay exceeds either the timer max or the available linger budget. Pick the offset against the slowest expected belt speed, not nominal.

## Code that will change

- `runtime/src/utils/SCNodeInput.cpp` — drop the "force input to GPI mode" path; leave Input-A in Move-Trigger mode (the comment at line 17 inverts).
- `runtime/src/seeder/SeederApp.cpp` — remove `update_roller_linger` / `photoeye_edge` gating of motion; replace with a pre-queue of `MoveVelStart(rpm, /*isTriggered=*/true)`. Keep `update_solenoid_pulse` unchanged.
- `runtime/include/seeder/MachineConfig.h` — replace `kRollerLingerScale` / `kRollerMaxLingerMs` with `kFireDelayScale` / `kFireDelay{Min,Max}Ms`; keep `kSolenoidPulseMs`. Drop `kSolenoidArmDelayMs` (the PLA debounces).
- Add one host-side hook on TE `belt_speed` change: write `GP_TIMER_PERIOD`.
