#include "../include/utils/ClearCoreClient.h"
#include "../include/utils/Axis.h"
#include "../include/utils/SFoundationClock.h"
#include "../include/utils/TouchEncoderState.h"
#include <cstdio>

int main() {
    constexpr const char* TE_JSON_PATH = "/home/rooted/te-cli/TE_Variable_Values.json";

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

    VelocityAxisConfig belt_config;
    belt_config.vel_limit_rpm = 120;
    belt_config.acc_limit_rpm_per_sec = 100000;
    belt_config.velocity_timeout_ms = 3000.0;

    SFoundationClock clock;
    SCVelocityAxis belt(client.node(0), belt_config);
    TouchEncoderState te_state(TE_JSON_PATH);

    bool last_ready_to_run = false;
    int last_active_variety = -2;
    int last_belt_speed = -1;

    printf("\n=== TE-driven belt control ===\n");
    printf("Watching %s\n", TE_JSON_PATH);

    while (true) {
        r = te_state.refresh();
        if (r.code != ResultCode::Ok) {
            printf("Touch encoder refresh failed: %s\n", r.message);
            clock.sleep_ms(100);
            continue;
        }

        const bool ready_to_run = te_state.ready_to_run();
        const int active_variety = te_state.active_variety();
        const int belt_speed = te_state.belt_speed();

        const bool state_changed =
            ready_to_run != last_ready_to_run ||
            active_variety != last_active_variety ||
            belt_speed != last_belt_speed;

        if (state_changed) {
            printf("TE update: ready_to_run=%d active_variety=%d belt_speed=%d\n",
                   ready_to_run ? 1 : 0,
                   active_variety,
                   belt_speed);

            if (!ready_to_run || belt_speed <= 0) {
                r = belt.stop();
                if (r.code != ResultCode::Ok) {
                    printf("Stop failed: %s\n", r.message);
                } else {
                    printf("Belt stopped\n");
                }
            } else {
                r = belt.set_velocity_rpm(belt_speed);
                if (r.code != ResultCode::Ok) {
                    printf("Velocity move failed: %s\n", r.message);
                } else {
                    printf("Belt commanded to %d RPM\n", belt_speed);
                }
            }

            last_ready_to_run = ready_to_run;
            last_active_variety = active_variety;
            last_belt_speed = belt_speed;
        }

        clock.sleep_ms(100);
    }
}
