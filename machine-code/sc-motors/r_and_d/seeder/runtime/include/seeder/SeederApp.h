#ifndef SEEDER_APP_H
#define SEEDER_APP_H

#include "../RuntimeTypes.h"
#include "../utils/Axis.h"
#include "../utils/ClearCoreClient.h"
#include "../utils/IClock.h"
#include "../utils/IInput.h"
#include "../utils/SCNodeInput.h"
#include "../utils/SCBrakeOutput.h"
#include "../telemetry/ITelemetrySink.h"
#include "../utils/SafetySupervisor.h"
#include "../utils/TouchEncoderState.h"
#include <cstddef>
#include <cstdint>
#include <optional>

class SeederApp {
public:
    SeederApp(const ClearCoreConfig& clearcore_cfg,
              const char* te_json_path,
              IClock& clock,
              ITelemetrySink& telemetry);

    Result boot();
    Result start();
    Result run_once();
    void sleep_loop_interval();

private:
    struct MachineState {
        bool initialized = false;
        bool ready_to_run = false;
        int active_variety = -1;
        int belt_speed = 0;
        int roller_speed = 0;
        bool fault_latched = false;
    };

    MachineState read_machine_state() const;
    void latch_axis_faults(const char* axis_name,
                           std::size_t node_index,
                           const AxisStatus& axis_status);
    void handle_ready_change(const MachineState& current_state, std::uint64_t now_ms);
    void handle_fault_change(const MachineState& current_state, std::uint64_t now_ms);
    void apply_motion_command(const MachineState& current_state);
    void apply_axis_motion(SCVelocityAxis& axis,
                           const VelocityAxisConfig& config,
                           const char* axis_name,
                           int te_speed,
                           const MachineState& current_state);
    void stop_axis(SCVelocityAxis& axis, const char* axis_name);
    void stop_all_axes();
    void emit_status_snapshot(const MachineState& current_state,
                              std::uint64_t now_ms);
    void update_previous_state(const MachineState& current_state);
    bool belt_active(const MachineState& current_state) const;
    bool roller_active(const MachineState& current_state) const;
    bool roller_uptime_active(const MachineState& current_state) const;
    bool roller_should_run(const MachineState& current_state) const;
    void update_roller_linger(const MachineState& current_state, std::uint64_t now_ms);
    void update_solenoid_pulse(bool is_blocked, std::uint64_t now_ms);
    std::size_t roller_node_index() const;

    ClearCoreClient client_;
    IClock& clock_;
    ITelemetrySink& telemetry_;
    TouchEncoderState te_state_;
    SafetySupervisor safety_;
    std::optional<SCVelocityAxis> belt_;
    std::optional<SCVelocityAxis> roller_;
    std::optional<SCNodeInput> roller_input_;
    std::optional<Photoeye> photoeye_;
    std::optional<SCBrakeOutput> solenoid_;

    std::uint64_t boot_id_ = 0;
    std::uint64_t telemetry_seq_ = 0;
    std::uint64_t last_snapshot_ms_ = 0;
    std::uint64_t last_uptime_sample_ms_ = 0;
    std::uint64_t belt_uptime_ms_ = 0;
    std::uint64_t roller_uptime_ms_ = 0;
    MachineState previous_state_;

    bool previous_blocked_ = false;
    std::uint64_t roller_stop_deadline_ms_ = 0;
    int linger_belt_speed_latched_ = 0;

    std::uint64_t solenoid_off_deadline_ms_ = 0;
    bool solenoid_armed_ = false;
};

#endif
