#include "../include/utils/ClearCoreClient.h"
#include "../include/utils/Axis.h"
#include "../include/utils/JsonlTelemetryLogger.h"
#include "../include/utils/IClock.h"
#include "../include/utils/SafetySupervisor.h"
#include "../include/utils/TelemetryTypes.h"
#include "../include/utils/TouchEncoderState.h"
#include "../include/seeder/MachineConfig.h"
#include <cstdint>
#include <cstdio>

int main() {
    constexpr const char* TE_JSON_PATH = "/home/rooted/te-cli/TE_Variable_Values.json";
    constexpr const char* TELEMETRY_LOG_PATH = "/home/rooted/telemetry_log.jsonl";
    constexpr std::uint64_t SNAPSHOT_INTERVAL_MS = 1000;

    ClearCoreConfig config;
    config.expected_node_count = 1;
    config.enable_timeout_ms = 5000.0;

    ClearCoreClient client(config);

    printf("=== ClearCoreClient init ===\n");
    Result r = client.init();
    if (r.code != ResultCode::Ok) {
        printf("Init failed: %s\n", r.message);
        return 1;
    }
    printf("Init OK, %zu node(s)\n", client.node_count());

    printf("\n=== Enabling nodes ===\n");
    r = client.enable_all();
    if (r.code != ResultCode::Ok) {
        printf("Enable failed: %s\n", r.message);
        client.shutdown();
        return 1;
    }
    printf("All nodes enabled\n");

    SFoundationClock clock;
    SCVelocityAxis belt(client.node(0), seeder::machine::kBelt);
    TouchEncoderState te_state(TE_JSON_PATH);
    JsonlTelemetryLogger telemetry(TELEMETRY_LOG_PATH);
    SafetySupervisor safety;
    safety.set_kill_ok(true);

    const std::uint64_t boot_id = clock.now_ms();
    std::uint64_t telemetry_seq = 0;
    std::uint64_t last_snapshot_ms = 0;
    std::uint64_t belt_active_uptime_ms = 0;
    std::uint64_t last_uptime_sample_ms = clock.now_ms();

    bool last_ready_to_run = false;
    int last_active_variety = -2;
    int last_belt_speed = -1;
    bool last_fault_latched = false;

    MachineEvent boot_event;
    boot_event.boot_id = boot_id;
    boot_event.seq = ++telemetry_seq;
    boot_event.timestamp_ms = clock.now_ms();
    boot_event.type = TelemetryEventType::BootStarted;

    printf("\n=== TE-driven belt control ===\n");
    printf("Watching %s\n", TE_JSON_PATH);
    printf("Telemetry log: %s\n", TELEMETRY_LOG_PATH);

    while (true) {
        // Refresh operator inputs first so motion, safety, and telemetry use one consistent TE view.
        r = te_state.refresh();
        if (r.code != ResultCode::Ok) {
            printf("Touch encoder refresh failed: %s\n", r.message);
            clock.sleep_ms(100);
            continue;
        }

        const bool ready_to_run = te_state.ready_to_run();
        const int active_variety = te_state.active_variety();
        const int belt_speed = te_state.belt_speed();
        const AxisStatus belt_status = belt.status();
        const std::uint64_t now_ms = clock.now_ms();

        // Latch hardware and motor faults before deciding whether motion is allowed this loop.
        if (!client.is_bus_power_ok(0)) {
            Result safety_result = safety.latch_fault("Belt node bus power low");
            if (safety_result.code == ResultCode::Ok) {
                printf("Fault latched: %s\n", safety.fault_reason());
            }
        }

        if (belt_status.faulted) {
            Result safety_result = safety.latch_fault("Belt node has alerts");
            if (safety_result.code == ResultCode::Ok) {
                printf("Fault latched: %s\n", safety.fault_reason());
            }
        }

        const bool fault_latched = safety.state().fault_latched;

        const bool state_changed =
            ready_to_run != last_ready_to_run ||
            active_variety != last_active_variety ||
            belt_speed != last_belt_speed;

        // Backend expects this as cumulative-per-boot active time, so pause accumulation when command goes idle.
        const std::uint64_t elapsed_ms = now_ms - last_uptime_sample_ms;
        const bool belt_commanded_active = ready_to_run && belt_speed > 0 && !fault_latched;
        if (belt_commanded_active) {
            belt_active_uptime_ms += elapsed_ms;
        }
        last_uptime_sample_ms = now_ms;

        if (ready_to_run != last_ready_to_run) {
            MachineEvent ready_event;
            ready_event.boot_id = boot_id;
            ready_event.seq = ++telemetry_seq;
            ready_event.timestamp_ms = now_ms;
            ready_event.type = TelemetryEventType::ReadyToRunChanged;
            ready_event.value_i64 = ready_to_run ? 1 : 0;

        }

        if (fault_latched != last_fault_latched) {
            Result telemetry_result = telemetry.event_handler(
                boot_id,
                ++telemetry_seq,
                now_ms,
                fault_latched ? TelemetryEventType::FaultEntered : TelemetryEventType::FaultCleared,
                "safety_supervisor",
                -1,
                0
            );

            if (telemetry_result.code != ResultCode::Ok) {
                printf("Telemetry fault event failed: %s\n", telemetry_result.message);
            }

            if (fault_latched) {
                Result stop_result = belt.stop();
                if (stop_result.code != ResultCode::Ok) {
                    printf("Fault stop failed: %s\n", stop_result.message);
                } else {
                    printf("Fault latched, belt stop requested\n");
                }
            }
        }

        if (state_changed) {
            // Only emit motor commands when the TE state changes; steady-state iterations just monitor.
            printf("TE update: ready_to_run=%d active_variety=%d belt_speed=%d\n",
                   ready_to_run ? 1 : 0,
                   active_variety,
                   belt_speed);

            if (fault_latched) {
                printf("Motion blocked: fault latched (%s)\n", safety.fault_reason());
            } else if (!ready_to_run || belt_speed <= 0) {
                r = belt.stop();
                if (r.code != ResultCode::Ok) {
                    printf("Stop failed: %s\n", r.message);
                } else {
                    printf("Belt stopped\n");
                }
            } else {
                r = belt.set_velocity_rpm(belt_speed);
                if (r.code != ResultCode::Ok) {
                    Result safety_result = safety.latch_fault(r.message);
                    if (safety_result.code == ResultCode::Ok) {
                        printf("Fault latched: %s\n", safety.fault_reason());
                    }
                    printf("Velocity move failed: %s\n", r.message);
                } else {
                    printf("Belt commanded at TE=%d (scaled to %d RPM)\n",
                           belt_speed,
                           belt_speed * seeder::machine::kBelt.rpm_per_te_unit);
                }
            }

            last_ready_to_run = ready_to_run;
            last_active_variety = active_variety;
            last_belt_speed = belt_speed;
        }

        if (last_snapshot_ms == 0 || now_ms - last_snapshot_ms >= SNAPSHOT_INTERVAL_MS) {
            // Periodic status frames carry cumulative counters; transition events are handled separately above.
            Result telemetry_result = telemetry.status_update_handler(
                boot_id,
                ++telemetry_seq,
                now_ms,
                ready_to_run,
                safety.state().kill_ok,
                fault_latched,
                active_variety,
                belt_speed,
                0,
                0,
                client.node(0).Info.SerialNumber.Value(),
                client.node(0).Info.Model.Value(),
                client.node(0).Info.FirmwareVersion.Value(),
                belt_status,
                client.is_bus_power_ok(0),
                client.node(0).Motion.VelMeasured.Value(),
                belt_active_uptime_ms
            );

            if (telemetry_result.code != ResultCode::Ok) {
                printf("Telemetry status update failed: %s\n", telemetry_result.message);
            }

            last_snapshot_ms = now_ms;
        }

        last_fault_latched = fault_latched;

        clock.sleep_ms(100);
    }
}
