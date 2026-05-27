#ifndef SEEDER_APP_H
#define SEEDER_APP_H

#include "../RuntimeTypes.h"
#include "../utils/Axis.h"
#include "../utils/ClearCoreClient.h"
#include "../utils/Photoeye.h"
#include "../utils/SCBrakeOutput.h"
#include "../utils/TouchEncoderState.h"
#include "../telemetry/JsonlTelemetryLogger.h"
#include <cstddef>
#include <cstdint>
#include <optional>

class SeederApp {
public:
    SeederApp(const ClearCoreConfig& clearcore_cfg,
              const char* te_json_path,
              JsonlTelemetryLogger& telemetry);

    Result boot();
    Result start();
    Result run_once();

private:
    void latch_axis_faults(const char* axis_name,
                           std::size_t node_index,
                           const AxisStatus& axis_status);
    void apply_motion_command();
    void apply_axis_motion(SCVelocityAxis& axis,
                           const VelocityAxisConfig& config,
                           const char* axis_name,
                           int te_speed);
    void stop_all_axes();
    void emit_status_snapshot(std::uint64_t now_ms);
    void update_solenoid_pulse(bool is_blocked, std::uint64_t now_ms);
    void update_fire_delay(int belt_speed);
    void latch_fault(const char* reason);
    std::size_t roller_node_index() const;

    ClearCoreClient client_;
    JsonlTelemetryLogger& telemetry_;
    TouchEncoderState te_state_;
    std::optional<SCVelocityAxis> belt_;
    std::optional<SCVelocityAxis> roller_;
    std::optional<Photoeye> photoeye_;
    std::optional<SCBrakeOutput> solenoid_;

    // Current TE-derived setpoint + latched fault. Mirrored to the prev_*
    // fields at the end of each tick for edge detection.
    bool ready_to_run_ = false;
    int active_variety_ = -1;
    int belt_speed_ = 0;
    int roller_speed_ = 0;
    bool fault_latched_ = false;
    const char* fault_reason_ = "";

    bool prev_initialized_ = false;
    bool prev_ready_to_run_ = false;
    int prev_active_variety_ = -1;
    int prev_belt_speed_ = 0;
    int prev_roller_speed_ = 0;
    bool prev_fault_latched_ = false;
    bool prev_blocked_ = false;

    std::uint64_t boot_id_ = 0;
    std::uint64_t telemetry_seq_ = 0;
    std::uint64_t last_snapshot_ms_ = 0;
    std::uint64_t last_uptime_sample_ms_ = 0;
    std::uint64_t belt_uptime_ms_ = 0;
    std::uint64_t roller_uptime_ms_ = 0;

    // Tracks whether a triggered velocity move is currently queued on the roller
    // node, and at what rpm. Re-armed on rpm change or after NodeStop.
    bool roller_armed_ = false;
    int roller_armed_rpm_ = 0;

    // Last value written to CPM_P_GP_TIMER on the roller node; used to skip
    // redundant parameter writes when belt_speed hasn't moved.
    std::uint64_t last_gp_timer_period_ms_ = 0;
    bool gp_timer_initialized_ = false;

    std::uint64_t solenoid_off_deadline_ms_ = 0;
};

#endif
