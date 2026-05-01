#include "telemetry/JsonlTelemetryLogger.h"

#include <nlohmann/json.hpp>
#include <ctime>

using nlohmann::json;

static const char* telemetry_event_code_to_string(TelemetryEventType type) {
    switch (type) {
        case TelemetryEventType::BootStarted:
            return "BOOT_STARTED";
        case TelemetryEventType::ReadyToRunChanged:
            return "READY_TO_RUN_CHANGED";
        case TelemetryEventType::FaultEntered:
            return "FAULT_ENTERED";
        case TelemetryEventType::FaultCleared:
            return "FAULT_CLEARED";
        case TelemetryEventType::TrayCountIncremented:
            return "TRAY_COUNT_INCREMENTED";
        case TelemetryEventType::RollerLingerStarted:
            return "ROLLER_LINGER_STARTED";
        case TelemetryEventType::RollerLingerEnded:
            return "ROLLER_LINGER_ENDED";
        case TelemetryEventType::RollerLingerCancelled:
            return "ROLLER_LINGER_CANCELLED";
        case TelemetryEventType::SolenoidPulseStarted:
            return "SOLENOID_PULSE_STARTED";
        case TelemetryEventType::SolenoidPulseEnded:
            return "SOLENOID_PULSE_ENDED";
    }

    return "UNKNOWN";
}

static std::string session_id_for_boot(std::uint64_t boot_id) {
    return std::string("boot-") + std::to_string(boot_id);
}

static std::int64_t received_at_epoch_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

static const MotorSnapshot* find_motor(const std::vector<MotorSnapshot>& motors, const char* role) {
    for (const MotorSnapshot& motor : motors) {
        if (motor.role == role) {
            return &motor;
        }
    }

    return nullptr;
}

JsonlTelemetryLogger::JsonlTelemetryLogger(const std::string& path)
    : path_(path) {}

Result JsonlTelemetryLogger::ensure_open() {
    if (file_.is_open()) {
        return {ResultCode::Ok};
    }

    file_.open(path_, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        return {ResultCode::Error, "Failed to open telemetry log file"};
    }

    return {ResultCode::Ok};
}

Result JsonlTelemetryLogger::log_event(const MachineEvent& event) {
    Result r = ensure_open();
    if (r.code != ResultCode::Ok) {
        return r;
    }

    json j = {
        {"type", "event"},
        {"session_id", session_id_for_boot(event.boot_id)},
        {"schema_ver", 1},
        {"boot_id", event.boot_id},
        {"seq", event.seq},
        {"uptime_ms", event.timestamp_ms >= event.boot_id ? event.timestamp_ms - event.boot_id : 0},
        {"uptime_s", event.timestamp_ms >= event.boot_id ? (event.timestamp_ms - event.boot_id) / 1000 : 0},
        {"delta_steps", 0},
        {"torque_pct", 0},
        {"belt_fault", 0},
        {"roller_fault", 0},
        {"alert_bits", 0},
        {"kill_switch", 0},
        {"cmd_age_ms", 0},
        {"belt_motor_uptime_ms", 0},
        {"roller_motor_uptime_ms", 0},
        {"udp_fail_count", 0},
        {"event_code", telemetry_event_code_to_string(event.type)},
        {"event_value", event.value_i64},
        {"trays_processed", 0},
        {"received_at", received_at_epoch_seconds()},
        {"fault_type", ""},
        {"motor", event.role},
    };

    file_ << j.dump() << '\n';
    file_.flush();

    if (!file_) {
        return {ResultCode::Error, "Failed to write telemetry event"};
    }

    return {ResultCode::Ok};
}


Result JsonlTelemetryLogger::log_snapshot(const MachineSnapshot& snapshot) {
    Result r = ensure_open();
    if (r.code != ResultCode::Ok) {
        return r;
    }

    const MotorSnapshot* belt = find_motor(snapshot.motors, "belt");
    const MotorSnapshot* roller = find_motor(snapshot.motors, "roller");

    json j = {
        {"type", "status_update"},
        {"session_id", session_id_for_boot(snapshot.boot_id)},
        {"schema_ver", 1},
        {"boot_id", snapshot.boot_id},
        {"seq", snapshot.seq},
        {"uptime_ms", snapshot.uptime_ms},
        {"uptime_s", snapshot.uptime_ms / 1000},
        {"delta_steps", 0},
        {"torque_pct", 0},
        {"belt_fault", belt && belt->faulted ? 1 : 0},
        {"roller_fault", roller && roller->faulted ? 1 : 0},
        {"alert_bits", belt ? belt->alert_bits : 0},
        {"kill_switch", snapshot.kill_ok ? 1 : 0},
        {"cmd_age_ms", 0},
        {"belt_motor_uptime_ms", belt ? belt->active_uptime_ms : 0},
        {"roller_motor_uptime_ms", roller ? roller->active_uptime_ms : 0},
        {"udp_fail_count", 0},
        {"event_code", ""},
        {"event_value", 0},
        {"trays_processed", snapshot.tray_count},
        {"received_at", received_at_epoch_seconds()},
        {"fault_type", ""},
        {"motor", belt ? belt->role : ""},
        {"ready_to_run", snapshot.ready_to_run},
    };

    file_ << j.dump() << '\n';
    file_.flush();

    if (!file_) {
        return {ResultCode::Error, "Failed to write telemetry snapshot"};
    }

    return {ResultCode::Ok};
}
