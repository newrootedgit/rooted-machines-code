#include "seeder/SeederApp.h"
#include "seeder/MachineConfig.h"
#include <cstdio>

SeederApp::SeederApp(const ClearCoreConfig& clearcore_cfg,
                     const char* te_json_path,
                     IClock& clock,
                     ITelemetrySink& telemetry)
    : client_(clearcore_cfg),
      clock_(clock),
      telemetry_(telemetry),
      te_state_(te_json_path),
      safety_() {}


// MAIN SETUP FUNCTION. Initializes ClearCore and maps available axes.
Result SeederApp::boot() {
    printf("=== ClearCoreClient init ===\n");
    Result r = client_.init();
    if (r.code != ResultCode::Ok) {
        printf("Init failed: %s\n", r.message);
        return r;
    }
    printf("Init OK, %zu node(s)\n", client_.node_count());

    printf("\n=== Enabling nodes ===\n");
    r = client_.enable_all();
    if (r.code != ResultCode::Ok) {
        printf("Enable failed: %s\n", r.message);
        client_.shutdown();
        return r;
    }
    printf("All nodes enabled\n");

    if (client_.node_count() > 1) {
        belt_.emplace(client_.node(seeder::machine::kBeltNodeIndex), seeder::machine::kBelt);
        roller_.emplace(client_.node(seeder::machine::kRollerNodeIndexMultiNode), seeder::machine::kRoller);
    } else {
        roller_.emplace(client_.node(seeder::machine::kRollerNodeIndexSingleNode), seeder::machine::kRoller);
        printf("Single-node setup detected: roller mapped to %s\n",
               seeder::machine::kRollerSingleNodeAxisLabel);
    }
    safety_.set_kill_ok(true);
    return {ResultCode::Ok};
}

Result SeederApp::start() {
    if (!belt_.has_value() && !roller_.has_value()) {
        printf("No seeder axes initialized\n");
        return {ResultCode::Error, "No seeder axes initialized"};
    }

    boot_id_ = clock_.now_ms();
    last_uptime_sample_ms_ = boot_id_;

    printf("\n=== TE-driven seeder control ===\n");
    printf("Watching %s\n", seeder::machine::kTouchEncoderJsonPath);
    printf("Telemetry log: %s\n", seeder::machine::kTelemetryLogPath);

    return {ResultCode::Ok};
}


// THIS IS THE MAIN CONTROL LOGIC, RUNS EVERY LOOP ITERATION
Result SeederApp::run_once() {

    // Read TE_vars, fail silently, wait 100ms and try again
    Result r = te_state_.refresh();
    if (r.code != ResultCode::Ok) {
        printf("Touch encoder refresh failed: %s\n", r.message);
        return {ResultCode::Ok};
    }

    // Latch faults based on current axis status and bus power.
    const std::uint64_t now_ms = clock_.now_ms();
    if (belt_.has_value()) {
        latch_axis_faults(
            "belt",
            seeder::machine::kBeltNodeIndex,
            belt_->status());
    }
    if (roller_.has_value()) {
        latch_axis_faults(
            "roller",
            roller_node_index(),
            roller_->status());
    }

    // Simple helper functions and tracks uptime for logging
    const MachineState state_after_safety = read_machine_state();
    const std::uint64_t elapsed_ms = now_ms - last_uptime_sample_ms_;
    if (belt_active(state_after_safety)) {
        belt_uptime_ms_ += elapsed_ms;
    }
    if (roller_active(state_after_safety)) {
        roller_uptime_ms_ += elapsed_ms;
    }
    last_uptime_sample_ms_ = now_ms;

    handle_ready_change(state_after_safety, now_ms);
    handle_fault_change(state_after_safety, now_ms);

    const bool state_changed =
        !previous_state_.initialized ||
        state_after_safety.ready_to_run != previous_state_.ready_to_run ||
        state_after_safety.active_variety != previous_state_.active_variety ||
        state_after_safety.belt_speed != previous_state_.belt_speed ||
        state_after_safety.roller_speed != previous_state_.roller_speed;

    if (state_changed) {
        apply_motion_command(state_after_safety);
    }

    if (last_snapshot_ms_ == 0 ||
        now_ms - last_snapshot_ms_ >= seeder::machine::kSnapshotIntervalMs) {
        emit_status_snapshot(
            state_after_safety,
            now_ms);
        last_snapshot_ms_ = now_ms;
    }

    update_previous_state(state_after_safety);
    return {ResultCode::Ok};
}

SeederApp::MachineState SeederApp::read_machine_state() const {
    MachineState state;
    state.initialized = true;
    state.ready_to_run = te_state_.ready_to_run();
    state.active_variety = te_state_.active_variety();
    state.belt_speed = te_state_.belt_speed();
    state.roller_speed = te_state_.roller_speed();
    state.fault_latched = safety_.state().fault_latched;
    return state;
}

void SeederApp::latch_axis_faults(const char* axis_name,
                                  std::size_t node_index,
                                  const AxisStatus& axis_status) {
    if (!client_.is_bus_power_ok(node_index)) {
        Result safety_result = safety_.latch_fault("Axis bus power low");
        if (safety_result.code == ResultCode::Ok) {
            printf("Fault latched on %s: %s\n", axis_name, safety_.fault_reason());
        }
    }

    if (axis_status.faulted) {
        Result safety_result = safety_.latch_fault("Axis has alerts");
        if (safety_result.code == ResultCode::Ok) {
            printf("Fault latched on %s: %s\n", axis_name, safety_.fault_reason());
        }
    }
}

void SeederApp::handle_ready_change(const SeederApp::MachineState& current_state, std::uint64_t now_ms) {
    if (!previous_state_.initialized && !current_state.ready_to_run) {
        return;
    }

    if (previous_state_.initialized &&
        current_state.ready_to_run == previous_state_.ready_to_run) {
        return;
    }

    MachineEvent event;
    event.boot_id = boot_id_;
    event.seq = ++telemetry_seq_;
    event.timestamp_ms = now_ms;
    event.type = TelemetryEventType::ReadyToRunChanged;
    event.role = "operator";
    event.node_index = -1;
    event.value_i64 = current_state.ready_to_run ? 1 : 0;

    Result r = telemetry_.log_event(event);
    if (r.code != ResultCode::Ok) {
        printf("Telemetry ready event failed: %s\n", r.message);
    }
}

void SeederApp::handle_fault_change(const SeederApp::MachineState& current_state, std::uint64_t now_ms) {
    if (!previous_state_.initialized && !current_state.fault_latched) {
        return;
    }

    if (previous_state_.initialized &&
        current_state.fault_latched == previous_state_.fault_latched) {
        return;
    }

    MachineEvent event;
    event.boot_id = boot_id_;
    event.seq = ++telemetry_seq_;
    event.timestamp_ms = now_ms;
    event.type = current_state.fault_latched ? TelemetryEventType::FaultEntered : TelemetryEventType::FaultCleared;
    event.role = "safety_supervisor";
    event.node_index = -1;
    event.value_i64 = 0;

    Result telemetry_result = telemetry_.log_event(event);
    if (telemetry_result.code != ResultCode::Ok) {
        printf("Telemetry fault event failed: %s\n", telemetry_result.message);
    }

    if (current_state.fault_latched) {
        stop_all_axes();
    }
}

void SeederApp::apply_motion_command(const SeederApp::MachineState& current_state) {
    printf("TE update: ready_to_run=%d active_variety=%d belt_speed=%d roller_speed=%d\n",
           current_state.ready_to_run ? 1 : 0,
           current_state.active_variety,
           current_state.belt_speed,
           current_state.roller_speed);

    if (current_state.fault_latched) {
        printf("Motion blocked: fault latched (%s)\n", safety_.fault_reason());
        stop_all_axes();
        return;
    }

    if (belt_.has_value()) {
        apply_axis_motion(
            *belt_,
            seeder::machine::kBelt,
            "belt",
            current_state.belt_speed,
            current_state);
        if (safety_.state().fault_latched) {
            stop_all_axes();
            return;
        }
    }

    if (roller_.has_value()) {
        apply_axis_motion(
            *roller_,
            seeder::machine::kRoller,
            "roller",
            current_state.roller_speed,
            current_state);
        if (safety_.state().fault_latched) {
            stop_all_axes();
        }
    }
}

void SeederApp::apply_axis_motion(SCVelocityAxis& axis,
                                  const VelocityAxisConfig& config,
                                  const char* axis_name,
                                  int te_speed,
                                  const SeederApp::MachineState& current_state) {
    if (!current_state.ready_to_run || te_speed <= 0) {
        stop_axis(axis, axis_name);
        return;
    }

    Result r = axis.set_velocity_rpm(te_speed);
    if (r.code != ResultCode::Ok) {
        Result safety_result = safety_.latch_fault(r.message);
        if (safety_result.code == ResultCode::Ok) {
            printf("Fault latched: %s\n", safety_.fault_reason());
        }
        printf("Velocity move failed: %s\n", r.message);
    } else {
        printf("%s commanded at TE=%d (scaled to %d RPM)\n",
               axis_name,
               te_speed,
               te_speed * config.rpm_per_te_unit);
    }
}

void SeederApp::stop_axis(SCVelocityAxis& axis, const char* axis_name) {
    Result r = axis.stop();
    if (r.code != ResultCode::Ok) {
        printf("%s stop failed: %s\n", axis_name, r.message);
    } else {
        printf("%s stopped\n", axis_name);
    }
}

void SeederApp::stop_all_axes() {
    if (belt_.has_value()) {
        stop_axis(*belt_, "belt");
    }
    if (roller_.has_value()) {
        stop_axis(*roller_, "roller");
    }
}


// Main TELEMETRY LOGGING function, runs every loop iteration if state has changed or at least every kSnapshotIntervalMs
void SeederApp::emit_status_snapshot(const SeederApp::MachineState& current_state,
                                     std::uint64_t now_ms) {

    // Both roller and belt are optional (in case of single-node setup), but if neither is present then there's no telemetry to emit
    if (!roller_.has_value() && !belt_.has_value()) {
        return;
    }

    const bool report_roller = roller_.has_value();
    const std::size_t node_index = report_roller
        ? roller_node_index()
        : seeder::machine::kBeltNodeIndex;
    const AxisStatus axis_status = report_roller
        ? roller_->status()
        : belt_->status();
    const int commanded_speed = report_roller
        ? current_state.roller_speed
        : current_state.belt_speed;
    const std::uint64_t motor_uptime_ms = report_roller
        ? roller_uptime_ms_
        : belt_uptime_ms_;

    MachineSnapshot snapshot;
    snapshot.boot_id = boot_id_;
    snapshot.seq = ++telemetry_seq_;
    snapshot.timestamp_ms = now_ms;
    snapshot.uptime_ms = now_ms >= boot_id_ ? now_ms - boot_id_ : 0;
    snapshot.ready_to_run = current_state.ready_to_run;
    snapshot.kill_ok = safety_.state().kill_ok;
    snapshot.fault_latched = current_state.fault_latched;
    snapshot.active_variety = current_state.active_variety;
    snapshot.belt_speed = commanded_speed;
    snapshot.tray_count = 0;

    MotorSnapshot motor;
    // Schema parity: legacy field set keys per-motor data on role == "belt",
    // even on single-node setups where we are actually reporting the roller.
    motor.role = "belt";
    motor.node_index = static_cast<int>(node_index);
    motor.serial = client_.node(node_index).Info.SerialNumber.Value();
    motor.model = client_.node(node_index).Info.Model.Value();
    motor.firmware = client_.node(node_index).Info.FirmwareVersion.Value();
    motor.enabled = axis_status.enabled;
    motor.ready = axis_status.enabled;
    motor.moving = axis_status.moving;
    motor.faulted = axis_status.faulted;
    motor.bus_power_ok = client_.is_bus_power_ok(node_index);
    motor.measured_velocity = client_.node(node_index).Motion.VelMeasured.Value();
    motor.active_uptime_ms = motor_uptime_ms;
    motor.alert_bits = 0;

    snapshot.motors.push_back(motor);

    Result telemetry_result = telemetry_.log_snapshot(snapshot);
    if (telemetry_result.code != ResultCode::Ok) {
        printf("Telemetry status update failed: %s\n", telemetry_result.message);
    }
}

void SeederApp::update_previous_state(const SeederApp::MachineState& current_state) {
    previous_state_ = current_state;
}

bool SeederApp::belt_active(const SeederApp::MachineState& current_state) const {
    if (!current_state.ready_to_run || current_state.fault_latched) {
        return false;
    }

    return belt_.has_value() && current_state.belt_speed > 0;
}

bool SeederApp::roller_active(const SeederApp::MachineState& current_state) const {
    if (!current_state.ready_to_run || current_state.fault_latched) {
        return false;
    }

    return roller_.has_value() && current_state.roller_speed > 0;
}

std::size_t SeederApp::roller_node_index() const {
    return belt_.has_value()
        ? seeder::machine::kRollerNodeIndexMultiNode
        : seeder::machine::kRollerNodeIndexSingleNode;
}

void SeederApp::sleep_loop_interval() {
    clock_.sleep_ms(100);
}
