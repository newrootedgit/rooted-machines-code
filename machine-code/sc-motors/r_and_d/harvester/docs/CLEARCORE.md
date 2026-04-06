# ClearCore

ClearCore is an auxiliary I/O coprocessor.

The Pi-hosted `Harvester` runtime remains the machine-state authority and the Teknic SC motor controller.

## Owns

- kill input monitoring
- tray detection
- timing-critical output behavior such as airknife pulse sequences
- local event reporting to the Pi
- local health and I/O status reporting

## Does not own

- Teknic motor control
- preset interpretation
- machine sequencing policy
- desired/applied machine configuration
- overall fault recovery policy

## Pi to ClearCore

- `set_airknife_mode`
- `arm_outputs`
- `disarm_outputs`
- `clear_io_fault`
- `ping`

## ClearCore to Pi

- `kill_off`
- `kill_on`
- `tray_detected`
- `tray_cleared`
- `airknife_sequence_started`
- `airknife_sequence_done`
- `io_fault`

## Status snapshot

Expose a small health/status snapshot containing:

- kill input state
- tray sensor state
- airknife mode
- output armed/disarmed
- fault present
- boot/session id
- last message or heartbeat age

## Design rule

ClearCore should report local facts and execute local timing-critical output behavior.

The Pi should interpret those facts in machine context.
