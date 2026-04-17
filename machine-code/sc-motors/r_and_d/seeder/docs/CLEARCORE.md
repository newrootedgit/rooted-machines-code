# ClearCore

ClearCore is an auxiliary I/O coprocessor.

The Pi-hosted `Harvester` runtime remains the machine-state authority and the Teknic SC motor controller.

## Owns

- kill input monitoring
- tray detection
- timing-critical output behavior such as airknife pulse sequences
- local event reporting to the Pi
- local health and I/O status reporting
- hardware enable line to the SC motors, dropped on heartbeat loss

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
- `heartbeat_tick`

## ClearCore to Pi

- `kill_off`
- `kill_on`
- `tray_detected`
- `tray_cleared`
- `airknife_sequence_started`
- `airknife_sequence_done`
- `io_fault`
- `heartbeat_lost`

## Boot handshake

On connect, ClearCore sends one `hello` message with:

- firmware version
- list of supported Pi-to-ClearCore commands
- list of ClearCore-to-Pi events it can emit

The Pi checks that every command it requires is advertised. Missing required commands are a startup failure with a clear error. Missing optional commands cause the Pi to skip the corresponding feature without erroring.

This lets ClearCore firmware and Pi runtime update independently, and turns version mismatches into precise messages instead of malformed-frame faults.

## Heartbeat

The Pi sends `heartbeat_tick` at 20 Hz. ClearCore drops the motor enable line and emits `heartbeat_lost` if it misses ticks for more than 150 ms. Re-arming requires `arm_outputs` from the Pi after ticks resume.

This catches Pi failure modes that TCP does not: application hangs with a healthy socket, and network partitions before TCP keepalive notices.

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
