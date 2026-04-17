#ifndef RUNTIME_TYPES_H
#define RUNTIME_TYPES_H

#include <cstdint>
#include <cstddef>

enum class ResultCode {
    Ok,
    Blocked,
    Error,
};

struct Result {
    ResultCode code = ResultCode::Ok;
    const char* message = "";
};

struct AxisStatus {
    bool enabled = false;
    bool faulted = false;
    bool moving = false;
};

struct SafetyState {
    bool kill_ok = false;
    bool fault_latched = false;
};

struct SeederCommand {};

struct SeederEffect {};

struct SeederState;

struct PresetValues {
    bool ready_to_run = false;
    int active_variety = -1;
    int belt_speed = 0;
};

struct DesiredPreset {
    PresetValues values {};
    std::uint64_t revision = 0;
};

struct ClearCoreConfig {
    std::size_t expected_node_count = 3;
    double enable_timeout_ms = 3000.0;
};
#endif
