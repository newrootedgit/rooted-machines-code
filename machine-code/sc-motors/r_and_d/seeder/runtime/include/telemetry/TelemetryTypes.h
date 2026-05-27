#ifndef TELEMETRY_TYPES_H
#define TELEMETRY_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

enum class TelemetryEventType {
    BootStarted,
    ReadyToRunChanged,
    FaultEntered,
    FaultCleared,
    TrayCountIncremented,
    SolenoidPulseStarted,
    SolenoidPulseEnded,
};

struct MachineEvent {
    std::uint64_t boot_id = 0;
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    TelemetryEventType type = TelemetryEventType::BootStarted;
    std::string role = "";
    int node_index = -1;
    std::int64_t value_i64 = 0;
};

struct MotorSnapshot {
    std::string role = "";
    int node_index = -1;

    int serial = 0;
    std::string model = "";
    std::string firmware = "";

    bool enabled = false;
    bool ready = false;
    bool moving = false;
    bool faulted = false;
    bool bus_power_ok = false;

    double measured_velocity = 0.0;

    bool load_metric_valid = false;
    double load_metric_pct = 0.0;

    bool torque_saturated = false;
    bool torque_limited = false;
    bool temp_warning = false;
    bool temp_shutdown = false;

    std::uint64_t active_uptime_ms = 0;
    std::uint32_t alert_bits = 0;
};

struct MachineSnapshot {
    std::uint64_t boot_id = 0;
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t uptime_ms = 0;

    bool ready_to_run = false;
    bool kill_ok = false;
    bool fault_latched = false;

    int active_variety = -1;
    int belt_speed = 0;
    std::uint64_t tray_count = 0;

    std::vector<MotorSnapshot> motors {};
};

#endif
