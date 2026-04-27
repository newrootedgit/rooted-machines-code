#include "../include/seeder/MachineConfig.h"
#include "../include/seeder/SeederApp.h"
#include "../include/telemetry/JsonlTelemetryLogger.h"
#include "../include/utils/IClock.h"

int main() {
    ClearCoreConfig config;
    config.expected_node_count = seeder::machine::kExpectedNodeCount;
    config.enable_timeout_ms = seeder::machine::kClearCoreEnableTimeoutMs;

    SFoundationClock clock;
    JsonlTelemetryLogger telemetry(seeder::machine::kTelemetryLogPath);

    SeederApp app(
        config,
        seeder::machine::kTouchEncoderJsonPath,
        clock,
        telemetry);

    if (app.boot().code != ResultCode::Ok) {
        return 1;
    }

    if (app.start().code != ResultCode::Ok) {
        return 1;
    }

    while (true) {
        Result r = app.run_once();
        if (r.code != ResultCode::Ok) {
            return 1;
        }
        // Clock is abstracted, for testing and logging, so sleep is handled by app, I know it seems more complicated than it needs to be.
        app.sleep_loop_interval();
    }
}
