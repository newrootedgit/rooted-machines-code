#include "utils/JsonlTelemetryLogger.h"

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
        {"blade_fault", 0},
        {"alert_bits", 0},
        {"kill_switch", 0},
        {"cmd_age_ms", 0},
        {"belt_motor_uptime_ms", 0},
        {"blade_motor_uptime_ms", 0},
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


Result JsonlTelemetryLogger::event_handler(
    uint64_t boot_id,
    uint64_t seq,
    uint64_t timestamp_ms,
    TelemetryEventType event_type,
    const std::string& role,
    int node_index,
    int64_t value_i64
) {
    MachineEvent event;
    event.boot_id = boot_id;
    event.seq = seq;
    event.timestamp_ms = timestamp_ms;
    event.type = event_type;
    event.role = role;
    event.node_index = node_index;
    event.value_i64 = value_i64;

    return log_event(event);
}

Result JsonlTelemetryLogger::status_update_handler(
    uint64_t boot_id,
    uint64_t seq,
    uint64_t timestamp_ms,
    bool ready_to_run,
    bool kill_ok,
    bool fault_latched,
    int active_variety,
    int belt_speed,
    uint64_t tray_count,
    int belt_node_index,
    int belt_serial,
    const std::string& belt_model,
    const std::string& belt_firmware,
    const AxisStatus& belt_status,
    bool belt_bus_power_ok,
    double belt_measured_velocity,
    uint64_t belt_active_uptime_ms,
    uint32_t belt_alert_bits
) {
    MachineSnapshot snapshot;
    snapshot.boot_id = boot_id;
    snapshot.seq = seq;
    snapshot.timestamp_ms = timestamp_ms;
    snapshot.uptime_ms = timestamp_ms >= boot_id ? timestamp_ms - boot_id : 0;
    snapshot.ready_to_run = ready_to_run;
    snapshot.kill_ok = kill_ok;
    snapshot.fault_latched = fault_latched;
    snapshot.active_variety = active_variety;
    snapshot.belt_speed = belt_speed;
    snapshot.tray_count = tray_count;

    MotorSnapshot belt_snapshot;
    belt_snapshot.role = "belt";
    belt_snapshot.node_index = belt_node_index;
    belt_snapshot.serial = belt_serial;
    belt_snapshot.model = belt_model;
    belt_snapshot.firmware = belt_firmware;
    belt_snapshot.enabled = belt_status.enabled;
    belt_snapshot.ready = belt_status.enabled;
    belt_snapshot.moving = belt_status.moving;
    belt_snapshot.faulted = belt_status.faulted;
    belt_snapshot.bus_power_ok = belt_bus_power_ok;
    belt_snapshot.measured_velocity = belt_measured_velocity;
    belt_snapshot.active_uptime_ms = belt_active_uptime_ms;
    belt_snapshot.alert_bits = belt_alert_bits;

    snapshot.motors.push_back(belt_snapshot);

    return log_snapshot(snapshot);
}




// --------------------------------------------------------------------------

Result JsonlTelemetryLogger::log_snapshot(const MachineSnapshot& snapshot) {
    Result r = ensure_open();
    if (r.code != ResultCode::Ok) {
        return r;
    }

    const MotorSnapshot* belt = find_motor(snapshot.motors, "belt");
    const MotorSnapshot* blade = find_motor(snapshot.motors, "blade");

    json j = {
        {"type", "status_update"},
        {"session_id", session_id_for_boot(snapshot.boot_id)},
        {"schema_ver", 1},
        {"boot_id", snapshot.boot_id},
        {"seq", snapshot.seq},
        {"uptime_ms", snapshot.uptime_ms},
        {"uptime_s", snapshot.uptime_ms / 1000},
        {"delta_steps", 0},
        // {"torque_pct", belt && belt->load_metric_valid ? belt->load_metric_pct : 0},
        {"torque_pct", 0},
        {"belt_fault", belt && belt->faulted ? 1 : 0},
        {"blade_fault", blade && blade->faulted ? 1 : 0},
        {"alert_bits", belt ? belt->alert_bits : 0},
        {"kill_switch", snapshot.kill_ok ? 1 : 0},
        {"cmd_age_ms", 0},
        {"belt_motor_uptime_ms", belt ? belt->active_uptime_ms : 0},
        {"blade_motor_uptime_ms", blade ? blade->active_uptime_ms : 0},
        {"udp_fail_count", 0},
        {"event_code", ""},
        {"event_value", 0},
        {"trays_processed", snapshot.tray_count},
        {"received_at", received_at_epoch_seconds()},
        {"fault_type", ""},
        {"motor", belt ? belt->role : ""},
        // {"active_variety", snapshot.active_variety},
        // {"belt_speed", snapshot.belt_speed},
        {"ready_to_run", snapshot.ready_to_run},
    };

    file_ << j.dump() << '\n';
    file_.flush();

    if (!file_) {
        return {ResultCode::Error, "Failed to write telemetry snapshot"};
    }

    return {ResultCode::Ok};
}
