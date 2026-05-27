#include "seeder/SeederApp.h"
#include "seeder/MachineConfig.h"
#include "pubCpmRegs.h"
#include <cstdio>

SeederApp::SeederApp(const ClearCoreConfig& clearcore_cfg,
                     const char* te_json_path,
                     JsonlTelemetryLogger& telemetry)
    : client_(clearcore_cfg),
      telemetry_(telemetry),
      te_state_(te_json_path) {}


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

    if (roller_.has_value()) {
        photoeye_.emplace(client_.node(roller_node_index()));
    }

    solenoid_.emplace(client_.port(), seeder::machine::kSolenoidBrakeNum);
    Result solr = solenoid_->set_off();
    if (solr.code != ResultCode::Ok) {
        printf("Solenoid initial set_off failed: %s\n", solr.message);
    }

    return {ResultCode::Ok};
}

Result SeederApp::start() {
    if (!belt_.has_value() && !roller_.has_value()) {
        printf("No seeder axes initialized\n");
        return {ResultCode::Error, "No seeder axes initialized"};
    }

    boot_id_ = static_cast<std::uint64_t>(sFnd::SysManager::Instance()->TimeStampMsec());
    last_uptime_sample_ms_ = boot_id_;

    printf("\n=== TE-driven seeder control ===\n");
    printf("Watching %s\n", seeder::machine::kTouchEncoderJsonPath);
    printf("Telemetry log: %s\n", seeder::machine::kTelemetryLogPath);

    return {ResultCode::Ok};
}


// Main control loop. Runs every ~100 ms from main().
Result SeederApp::run_once() {
    // Read TE_vars; fail silently and retry next tick.
    Result r = te_state_.refresh();
    if (r.code != ResultCode::Ok) {
        printf("Touch encoder refresh failed: %s\n", r.message);
        return {ResultCode::Ok};
    }
    ready_to_run_ = te_state_.ready_to_run();
    active_variety_ = te_state_.active_variety();
    belt_speed_ = te_state_.belt_speed();
    roller_speed_ = te_state_.roller_speed();

    if (photoeye_.has_value()) {
        Result pe_r = photoeye_->refresh();
        if (pe_r.code != ResultCode::Ok) {
            printf("Photoeye refresh failed: %s\n", pe_r.message);
        }
    }

    const std::uint64_t now_ms = static_cast<std::uint64_t>(sFnd::SysManager::Instance()->TimeStampMsec());

    if (belt_.has_value()) {
        latch_axis_faults("belt", seeder::machine::kBeltNodeIndex, belt_->status());
    }
    if (roller_.has_value()) {
        latch_axis_faults("roller", roller_node_index(), roller_->status());
    }

    const std::uint64_t elapsed_ms = now_ms - last_uptime_sample_ms_;
    const bool belt_running = ready_to_run_ && !fault_latched_ && belt_.has_value() && belt_speed_ > 0;
    if (belt_running) {
        belt_uptime_ms_ += elapsed_ms;
    }
    if (roller_.has_value() && roller_->status().moving) {
        roller_uptime_ms_ += elapsed_ms;
    }
    last_uptime_sample_ms_ = now_ms;

    // ready_to_run edge → telemetry event.
    if (prev_initialized_ && ready_to_run_ != prev_ready_to_run_) {
        MachineEvent event;
        event.boot_id = boot_id_;
        event.seq = ++telemetry_seq_;
        event.timestamp_ms = now_ms;
        event.type = TelemetryEventType::ReadyToRunChanged;
        event.role = "operator";
        event.node_index = -1;
        event.value_i64 = ready_to_run_ ? 1 : 0;
        Result tr = telemetry_.log_event(event);
        if (tr.code != ResultCode::Ok) {
            printf("Telemetry ready event failed: %s\n", tr.message);
        }
    }

    // fault edge → telemetry event + hard stop on entry.
    if (prev_initialized_ && fault_latched_ != prev_fault_latched_) {
        MachineEvent event;
        event.boot_id = boot_id_;
        event.seq = ++telemetry_seq_;
        event.timestamp_ms = now_ms;
        event.type = fault_latched_ ? TelemetryEventType::FaultEntered : TelemetryEventType::FaultCleared;
        event.role = "safety_supervisor";
        event.node_index = -1;
        event.value_i64 = 0;
        Result tr = telemetry_.log_event(event);
        if (tr.code != ResultCode::Ok) {
            printf("Telemetry fault event failed: %s\n", tr.message);
        }
        if (fault_latched_) {
            stop_all_axes();
        }
    }

    // Photoeye edge fires the on-node Move Trigger via the PLA; host only needs
    // the level for the solenoid pulse. Roller motion is no longer host-gated.
    const bool is_blocked = photoeye_.has_value() && photoeye_->blocked();
    update_solenoid_pulse(is_blocked, now_ms);

    // Push fire-delay (CPM_P_GP_TIMER) on belt-speed change; no-op if unchanged.
    if (!prev_initialized_ || belt_speed_ != prev_belt_speed_) {
        update_fire_delay(belt_speed_);
    }

    const bool state_changed =
        !prev_initialized_ ||
        ready_to_run_ != prev_ready_to_run_ ||
        active_variety_ != prev_active_variety_ ||
        belt_speed_ != prev_belt_speed_ ||
        roller_speed_ != prev_roller_speed_;

    if (state_changed) {
        apply_motion_command();
    }

    if (last_snapshot_ms_ == 0 ||
        now_ms - last_snapshot_ms_ >= seeder::machine::kSnapshotIntervalMs) {
        emit_status_snapshot(now_ms);
        last_snapshot_ms_ = now_ms;
    }

    prev_initialized_ = true;
    prev_ready_to_run_ = ready_to_run_;
    prev_active_variety_ = active_variety_;
    prev_belt_speed_ = belt_speed_;
    prev_roller_speed_ = roller_speed_;
    prev_fault_latched_ = fault_latched_;
    prev_blocked_ = is_blocked;

    sFnd::SysManager::Instance()->Delay(100);
    return {ResultCode::Ok};
}

void SeederApp::latch_fault(const char* reason) {
    if (fault_latched_) {
        return;
    }
    fault_latched_ = true;
    fault_reason_ = reason;
}

void SeederApp::latch_axis_faults(const char* axis_name,
                                  std::size_t node_index,
                                  const AxisStatus& axis_status) {
    if (!client_.is_bus_power_ok(node_index)) {
        if (!fault_latched_) {
            latch_fault("Axis bus power low");
            printf("Fault latched on %s: %s\n", axis_name, fault_reason_);
        }
    }

    if (axis_status.faulted) {
        if (!fault_latched_) {
            latch_fault("Axis has alerts");
            printf("Fault latched on %s: %s\n", axis_name, fault_reason_);
        }
    }
}

void SeederApp::apply_motion_command() {
    printf("TE update: ready_to_run=%d active_variety=%d belt_speed=%d roller_speed=%d\n",
           ready_to_run_ ? 1 : 0,
           active_variety_,
           belt_speed_,
           roller_speed_);

    if (fault_latched_) {
        printf("Motion blocked: fault latched (%s)\n", fault_reason_);
        stop_all_axes();
        return;
    }

    if (belt_.has_value()) {
        apply_axis_motion(*belt_, seeder::machine::kBelt, "belt", belt_speed_);
        if (fault_latched_) {
            stop_all_axes();
            return;
        }
    }

    if (roller_.has_value()) {
        // Roller is in InputTriggered mode: motion fires on-node when the PLA
        // routes Input-A through the GP timer to ISC_PLAOUT_TRIGGER_BIT. The
        // host's job is just to keep exactly one triggered move queued at the
        // right rpm — re-queue on rpm change, NodeStop on disarm.
        const bool want_active = ready_to_run_ && roller_speed_ > 0;
        if (want_active) {
            if (!roller_armed_ || roller_speed_ != roller_armed_rpm_) {
                if (roller_armed_) {
                    // Clear any pending triggered move so the new rpm takes
                    // effect on the next photoeye edge, not the one after.
                    Result sr = roller_->stop();
                    if (sr.code != ResultCode::Ok) {
                        printf("roller stop failed: %s\n", sr.message);
                    }
                }
                apply_axis_motion(*roller_, seeder::machine::kRoller, "roller", roller_speed_);
                roller_armed_ = !fault_latched_;
                roller_armed_rpm_ = roller_speed_;
            }
        } else if (roller_armed_) {
            Result sr = roller_->stop();
            if (sr.code != ResultCode::Ok) {
                printf("roller stop failed: %s\n", sr.message);
            }
            roller_armed_ = false;
            roller_armed_rpm_ = 0;
        }
        if (fault_latched_) {
            stop_all_axes();
        }
    }
}

void SeederApp::apply_axis_motion(SCVelocityAxis& axis,
                                  const VelocityAxisConfig& config,
                                  const char* axis_name,
                                  int te_speed) {
    if (!ready_to_run_ || te_speed <= 0) {
        Result sr = axis.stop();
        if (sr.code != ResultCode::Ok) {
            printf("%s stop failed: %s\n", axis_name, sr.message);
        }
        return;
    }

    Result r = axis.set_velocity_rpm(te_speed);
    if (r.code != ResultCode::Ok) {
        latch_fault(r.message);
        printf("Velocity move failed: %s\n", r.message);
    } else {
        printf("%s commanded at TE=%d (scaled to %d RPM)\n",
               axis_name, te_speed, te_speed * config.rpm_per_te_unit);
    }
}

void SeederApp::stop_all_axes() {
    if (belt_.has_value()) {
        Result r = belt_->stop();
        if (r.code != ResultCode::Ok) {
            printf("belt stop failed: %s\n", r.message);
        }
    }
    if (roller_.has_value()) {
        Result r = roller_->stop();
        if (r.code != ResultCode::Ok) {
            printf("roller stop failed: %s\n", r.message);
        }
    }
    roller_armed_ = false;
    roller_armed_rpm_ = 0;
}


// Emit 1-Hz status snapshot to the JSONL log.
void SeederApp::emit_status_snapshot(std::uint64_t now_ms) {
    if (!roller_.has_value() && !belt_.has_value()) {
        return;
    }

    const bool report_roller = roller_.has_value();
    const std::size_t node_index = report_roller
        ? roller_node_index()
        : seeder::machine::kBeltNodeIndex;
    const AxisStatus axis_status = report_roller ? roller_->status() : belt_->status();
    const int commanded_speed = report_roller ? roller_speed_ : belt_speed_;
    const std::uint64_t motor_uptime_ms = report_roller ? roller_uptime_ms_ : belt_uptime_ms_;

    MachineSnapshot snapshot;
    snapshot.boot_id = boot_id_;
    snapshot.seq = ++telemetry_seq_;
    snapshot.timestamp_ms = now_ms;
    snapshot.uptime_ms = now_ms >= boot_id_ ? now_ms - boot_id_ : 0;
    snapshot.ready_to_run = ready_to_run_;
    snapshot.kill_ok = !fault_latched_;
    snapshot.fault_latched = fault_latched_;
    snapshot.active_variety = active_variety_;
    snapshot.belt_speed = commanded_speed;
    snapshot.tray_count = 0;

    MotorSnapshot motor;
    motor.role = report_roller ? "roller" : "belt";
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

    Result tr = telemetry_.log_snapshot(snapshot);
    if (tr.code != ResultCode::Ok) {
        printf("Telemetry status update failed: %s\n", tr.message);
    }
}

void SeederApp::update_solenoid_pulse(bool is_blocked, std::uint64_t now_ms) {
    if (!solenoid_.has_value()) {
        return;
    }

    auto emit = [&](TelemetryEventType type, std::int64_t value) {
        MachineEvent event;
        event.boot_id = boot_id_;
        event.seq = ++telemetry_seq_;
        event.timestamp_ms = now_ms;
        event.type = type;
        event.role = "solenoid";
        event.node_index = -1;
        event.value_i64 = value;
        Result r = telemetry_.log_event(event);
        if (r.code != ResultCode::Ok) {
            printf("Telemetry solenoid event failed: %s\n", r.message);
        }
    };

    if (!prev_blocked_ && is_blocked) {
        printf("[solenoid] rising edge @ %llu ms -> set_on (brake_num=%zu, pulse=%llu ms)\n",
               static_cast<unsigned long long>(now_ms),
               seeder::machine::kSolenoidBrakeNum,
               static_cast<unsigned long long>(seeder::machine::kSolenoidPulseMs));
        Result r = solenoid_->set_on();
        if (r.code != ResultCode::Ok) {
            printf("[solenoid] set_on failed: %s\n", r.message);
            return;
        }
        solenoid_off_deadline_ms_ = now_ms + seeder::machine::kSolenoidPulseMs;
        emit(TelemetryEventType::SolenoidPulseStarted,
             static_cast<std::int64_t>(seeder::machine::kSolenoidPulseMs));
    } else if (solenoid_off_deadline_ms_ != 0 && now_ms >= solenoid_off_deadline_ms_) {
        Result r = solenoid_->set_off();
        if (r.code != ResultCode::Ok) {
            printf("[solenoid] set_off failed: %s\n", r.message);
        }
        solenoid_off_deadline_ms_ = 0;
        emit(TelemetryEventType::SolenoidPulseEnded, 0);
    }
}

void SeederApp::update_fire_delay(int belt_speed) {
    if (!roller_.has_value()) {
        return;
    }

    std::uint64_t period_ms = seeder::machine::kFireDelayMaxMs;
    if (belt_speed > 0) {
        period_ms = static_cast<std::uint64_t>(seeder::machine::kFireDelayScale) /
                    static_cast<std::uint64_t>(belt_speed);
        if (period_ms < seeder::machine::kFireDelayMinMs) {
            period_ms = seeder::machine::kFireDelayMinMs;
        }
        if (period_ms > seeder::machine::kFireDelayMaxMs) {
            period_ms = seeder::machine::kFireDelayMaxMs;
        }
    }

    if (gp_timer_initialized_ && period_ms == last_gp_timer_period_ms_) {
        return;
    }

    try {
        sFnd::INode& node = client_.node(roller_node_index());
        sFnd::ValueDouble gp_timer(node, CPM_P_GP_TIMER);
        gp_timer = static_cast<double>(period_ms);
        last_gp_timer_period_ms_ = period_ms;
        gp_timer_initialized_ = true;
        printf("[fire-delay] GP_TIMER_PERIOD = %llu ms (belt_speed=%d)\n",
               static_cast<unsigned long long>(period_ms), belt_speed);
    } catch (...) {
        printf("[fire-delay] failed to write CPM_P_GP_TIMER\n");
    }
}

std::size_t SeederApp::roller_node_index() const {
    return belt_.has_value()
        ? seeder::machine::kRollerNodeIndexMultiNode
        : seeder::machine::kRollerNodeIndexSingleNode;
}
