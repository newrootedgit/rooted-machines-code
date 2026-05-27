#include "seeder/MachineConfig.h"
#include "seeder/SeederApp.h"
#include "telemetry/JsonlTelemetryLogger.h"

int main() {
    ClearCoreConfig config;
    config.expected_node_count = seeder::machine::kExpectedNodeCount;
    config.enable_timeout_ms = seeder::machine::kClearCoreEnableTimeoutMs;

    JsonlTelemetryLogger telemetry(seeder::machine::kTelemetryLogPath);
    SeederApp app(config, seeder::machine::kTouchEncoderJsonPath, telemetry);

    if (app.boot().code != ResultCode::Ok) return 1;
    if (app.start().code != ResultCode::Ok) return 1;

    while (true) {
        if (app.run_once().code != ResultCode::Ok) return 1;
    }
}
